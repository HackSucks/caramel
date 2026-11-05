// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "ps4-ccp: " fmt

#include <crypto/internal/skcipher.h>
#include <crypto/xts.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/hex.h>
#include <linux/initrd.h>
#include <linux/kernel_read_file.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/delay.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <asm/ps4.h>

#define DM_MSG_PREFIX		"ps4crypt"
#define PS4_CCP_SECTOR		512
#define PS4_CCP_OP_SIZE		0x88
#define PS4_CCP_XTS_CMD		0x02000000
#define PS4_CCP_ENCRYPT		0x00001000
#define PS4_CCP_KEY_SIZE	32
#define PS4_CCP_PRIORITY	300

#define PS4_EAP_KEY_PATH	"/key/eap_hdd_key.bin"
#define PS4_EAP_KEY_SIZE	32

struct ps4_ccp_ctx {
	u8 key[PS4_CCP_KEY_SIZE];
};

static u8 ps4_eap_key[PS4_EAP_KEY_SIZE];
static struct kobject *ps4_fw_kobj;

static void ps4_ccp_reverse_key(u8 *dst, const u8 *src)
{
	int i;

	for (i = 0; i < 16; i++) {
		dst[i] = src[15 - i];
		dst[16 + i] = src[31 - i];
	}
}

struct ps4_ccp_req {
	struct list_head list;
	struct skcipher_request *req;
	bool encrypt;
};

static LIST_HEAD(ps4_ccp_queue);
static DEFINE_SPINLOCK(ps4_ccp_queue_lock);
static struct workqueue_struct *ps4_ccp_wq;
static struct work_struct ps4_ccp_work;
static struct ps4_ccp_req **ps4_ccp_batch;
static unsigned int ps4_ccp_batch_max;
static unsigned long ps4_ccp_ops;
static unsigned long ps4_ccp_sectors;
static unsigned long ps4_ccp_mapped;
static bool ps4_ccp_map_ok;
module_param_named(ops, ps4_ccp_ops, ulong, 0444);
module_param_named(sectors, ps4_ccp_sectors, ulong, 0444);
module_param_named(mapped, ps4_ccp_mapped, ulong, 0444);
module_param_named(zero_copy, ps4_ccp_map_ok, bool, 0644);

static int ps4_ccp_run(const u8 *key, u64 va, u64 sector, u32 count,
		       bool encrypt)
{
	u8 op[PS4_CCP_OP_SIZE] = { 0 };

	*(u32 *)op = PS4_CCP_XTS_CMD | (encrypt ? PS4_CCP_ENCRYPT : 0);
	*(u32 *)(op + 0x08) = count;
	*(u64 *)(op + 0x10) = va;
	*(u64 *)(op + 0x18) = va;
	*(u64 *)(op + 0x20) = sector;
	memcpy(op + 0x28, key, PS4_CCP_KEY_SIZE);
	return ps4_samu_ccp(op);
}

static int ps4_ccp_setkey(struct crypto_skcipher *tfm, const u8 *key,
			  unsigned int len)
{
	struct ps4_ccp_ctx *ctx = crypto_skcipher_ctx(tfm);
	int ret;

	if (len != PS4_CCP_KEY_SIZE)
		return -EINVAL;
	ret = xts_verify_key(tfm, key, len);
	if (ret)
		return ret;
	ps4_ccp_reverse_key(ctx->key, key);
	return 0;
}

static u64 ps4_ccp_req_sector(struct ps4_ccp_req *r)
{
	return get_unaligned_le64(r->req->iv);
}

static struct ps4_ccp_ctx *ps4_ccp_req_ctx(struct ps4_ccp_req *r)
{
	return crypto_skcipher_ctx(crypto_skcipher_reqtfm(r->req));
}

static void ps4_ccp_process(struct ps4_ccp_req **batch, unsigned int count)
{
	struct ps4_ccp_ctx *ctx = ps4_ccp_req_ctx(batch[0]);
	size_t window;
	unsigned int i;
	u64 va;
	u8 *buf;
	int ret;

	ps4_samu_lock();
	buf = ps4_samu_data(&va, &window);
	if (!buf) {
		ret = -ENODEV;
		goto out;
	}
	for (i = 0; i < count; i++)
		sg_pcopy_to_buffer(batch[i]->req->src, sg_nents(batch[i]->req->src),
				   buf + i * PS4_CCP_SECTOR, PS4_CCP_SECTOR, 0);
	ret = ps4_ccp_run(ctx->key, va, ps4_ccp_req_sector(batch[0]), count,
			  batch[0]->encrypt);
	ps4_ccp_ops++;
	ps4_ccp_sectors += count;
	if (!ret)
		for (i = 0; i < count; i++)
			sg_pcopy_from_buffer(batch[i]->req->dst,
					     sg_nents(batch[i]->req->dst),
					     buf + i * PS4_CCP_SECTOR,
					     PS4_CCP_SECTOR, 0);
out:
	ps4_samu_unlock();
	local_bh_disable();
	for (i = 0; i < count; i++)
		skcipher_request_complete(batch[i]->req, ret);
	local_bh_enable();
}

static bool ps4_ccp_joins(struct ps4_ccp_req *first, unsigned int count,
			  struct ps4_ccp_req *next)
{
	return count < ps4_ccp_batch_max &&
	       next->encrypt == first->encrypt &&
	       ps4_ccp_req_ctx(next) == ps4_ccp_req_ctx(first) &&
	       ps4_ccp_req_sector(next) == ps4_ccp_req_sector(first) + count;
}

static void ps4_ccp_worker(struct work_struct *work)
{
	struct ps4_ccp_req *r, *tmp;
	unsigned int count;
	LIST_HEAD(local);

	for (;;) {
		spin_lock_bh(&ps4_ccp_queue_lock);
		list_splice_tail_init(&ps4_ccp_queue, &local);
		spin_unlock_bh(&ps4_ccp_queue_lock);
		if (list_empty(&local))
			return;

		count = 0;
		list_for_each_entry_safe(r, tmp, &local, list) {
			if (count && !ps4_ccp_joins(ps4_ccp_batch[0], count, r)) {
				ps4_ccp_process(ps4_ccp_batch, count);
				count = 0;
			}
			list_del(&r->list);
			ps4_ccp_batch[count++] = r;
		}
		if (count)
			ps4_ccp_process(ps4_ccp_batch, count);
	}
}

static int ps4_ccp_queue_req(struct skcipher_request *req, bool encrypt)
{
	struct ps4_ccp_req *r = skcipher_request_ctx(req);

	if (req->cryptlen != PS4_CCP_SECTOR ||
	    get_unaligned_le64(req->iv + 8))
		return -EINVAL;

	r->req = req;
	r->encrypt = encrypt;
	spin_lock_bh(&ps4_ccp_queue_lock);
	list_add_tail(&r->list, &ps4_ccp_queue);
	spin_unlock_bh(&ps4_ccp_queue_lock);
	queue_work(ps4_ccp_wq, &ps4_ccp_work);
	return -EINPROGRESS;
}

static int ps4_ccp_encrypt(struct skcipher_request *req)
{
	return ps4_ccp_queue_req(req, true);
}

static int ps4_ccp_decrypt(struct skcipher_request *req)
{
	return ps4_ccp_queue_req(req, false);
}

static int ps4_ccp_init_tfm(struct crypto_skcipher *tfm)
{
	crypto_skcipher_set_reqsize(tfm, sizeof(struct ps4_ccp_req));
	return 0;
}

static struct skcipher_alg ps4_ccp_alg = {
	.base.cra_name = "xts(aes)",
	.base.cra_driver_name = "xts-aes-ps4ccp",
	.base.cra_priority = PS4_CCP_PRIORITY,
	.base.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_KERN_DRIVER_ONLY,
	.base.cra_blocksize = 16,
	.base.cra_ctxsize = sizeof(struct ps4_ccp_ctx),
	.base.cra_module = THIS_MODULE,
	.min_keysize = PS4_CCP_KEY_SIZE,
	.max_keysize = PS4_CCP_KEY_SIZE,
	.ivsize = 16,
	.init = ps4_ccp_init_tfm,
	.setkey = ps4_ccp_setkey,
	.encrypt = ps4_ccp_encrypt,
	.decrypt = ps4_ccp_decrypt,
};

static int ps4_ccp_reference(const u8 *key, u8 *data, u64 sector)
{
	struct crypto_sync_skcipher *tfm;
	struct scatterlist sg;
	u8 iv[16] = { 0 };
	int ret;

	tfm = crypto_alloc_sync_skcipher("xts(aes)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	put_unaligned_le64(sector, iv);
	ret = crypto_sync_skcipher_setkey(tfm, key, PS4_CCP_KEY_SIZE);
	if (!ret) {
		SYNC_SKCIPHER_REQUEST_ON_STACK(req, tfm);

		sg_init_one(&sg, data, PS4_CCP_SECTOR);
		skcipher_request_set_sync_tfm(req, tfm);
		skcipher_request_set_callback(req, 0, NULL, NULL);
		skcipher_request_set_crypt(req, &sg, &sg, PS4_CCP_SECTOR, iv);
		ret = crypto_skcipher_encrypt(req);
		skcipher_request_zero(req);
	}
	crypto_free_sync_skcipher(tfm);
	return ret;
}

#define PS4_CCP_MAP_TEST_PAGES	3
#define PS4_CCP_MAP_TEST_SECTORS	10

static void ps4_ccp_map_copy(struct page **pages, unsigned int first, u8 *data,
			     size_t len, bool to_pages)
{
	size_t pos, chunk;
	u8 *p;

	for (pos = 0; pos < len; pos += chunk) {
		p = page_address(pages[(first + pos) / PAGE_SIZE]) +
		    (first + pos) % PAGE_SIZE;
		chunk = min_t(size_t, len - pos,
			      PAGE_SIZE - (first + pos) % PAGE_SIZE);
		if (to_pages)
			memcpy(p, data + pos, chunk);
		else
			memcpy(data + pos, p, chunk);
	}
}

static int ps4_ccp_map_round(const u8 *samu_key, struct page **pages,
			     unsigned int first, u64 sector)
{
	u64 va;
	int ret;

	ps4_samu_lock();
	ret = ps4_samu_map(pages, PS4_CCP_MAP_TEST_PAGES, &va);
	if (!ret) {
		ret = ps4_ccp_run(samu_key, va + first, sector,
				  PS4_CCP_MAP_TEST_SECTORS, true);
		ps4_samu_unmap();
	}
	ps4_samu_unlock();
	return ret;
}

static int ps4_ccp_map_check(const u8 *key, struct page **pages,
			     unsigned int first, u8 *plain, u8 *got,
			     u64 sector)
{
	size_t len = PS4_CCP_MAP_TEST_SECTORS * PS4_CCP_SECTOR;
	int ret = 0, i;

	for (i = 0; i < PS4_CCP_MAP_TEST_SECTORS && !ret; i++)
		ret = ps4_ccp_reference(key, plain + i * PS4_CCP_SECTOR,
					sector + i);
	ps4_ccp_map_copy(pages, first, got, len, false);
	if (!ret && memcmp(got, plain, len))
		ret = -EBADMSG;
	return ret;
}

static int ps4_ccp_map_selftest(const u8 *key, const u8 *samu_key)
{
	size_t len = PS4_CCP_MAP_TEST_SECTORS * PS4_CCP_SECTOR;
	unsigned int first = PAGE_SIZE - PS4_CCP_SECTOR;
	struct page *pages[2 * PS4_CCP_MAP_TEST_PAGES] = { NULL };
	struct page **a = pages, **b = pages + PS4_CCP_MAP_TEST_PAGES;
	u64 sector_a = 0x5678, sector_b = 0x9abc;
	u8 *plain_a, *plain_b, *got;
	int ret = -ENOMEM, i;

	plain_a = kmalloc(len, GFP_KERNEL);
	plain_b = kmalloc(len, GFP_KERNEL);
	got = kmalloc(len, GFP_KERNEL);
	if (!plain_a || !plain_b || !got)
		goto out;
	for (i = ARRAY_SIZE(pages) - 1; i >= 0; i--) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i])
			goto out;
	}
	for (i = 0; i < len; i++) {
		plain_a[i] = (u8)(i * 7 + 3);
		plain_b[i] = (u8)(i * 11 + 5);
	}
	ps4_ccp_map_copy(a, first, plain_a, len, true);
	ps4_ccp_map_copy(b, first, plain_b, len, true);

	ret = ps4_ccp_map_round(samu_key, a, first, sector_a);
	if (!ret)
		ret = ps4_ccp_map_round(samu_key, b, first, sector_b);
	if (!ret)
		ret = ps4_ccp_map_check(key, a, first, plain_a, got, sector_a);
	if (!ret)
		ret = ps4_ccp_map_check(key, b, first, plain_b, got, sector_b);
out:
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		if (pages[i])
			__free_page(pages[i]);
	kfree(got);
	kfree(plain_b);
	kfree(plain_a);
	return ret;
}

static int ps4_ccp_selftest(void)
{
	u8 key[PS4_CCP_KEY_SIZE], samu_key[PS4_CCP_KEY_SIZE];
	u8 *expected, *buf;
	size_t window;
	u64 va, sector = 0x1234;
	int ret, i;

	expected = kmalloc(PS4_CCP_SECTOR, GFP_KERNEL);
	if (!expected)
		return -ENOMEM;
	for (i = 0; i < PS4_CCP_KEY_SIZE; i++)
		key[i] = 0x40 + i;
	for (i = 0; i < PS4_CCP_SECTOR; i++)
		expected[i] = (u8)(i * 13 + 1);
	ps4_ccp_reverse_key(samu_key, key);

	ps4_samu_lock();
	buf = ps4_samu_data(&va, &window);
	if (!buf) {
		ret = -ENODEV;
		goto out;
	}
	memcpy(buf, expected, PS4_CCP_SECTOR);
	ret = ps4_ccp_run(samu_key, va, sector, 1, true);
	if (ret)
		goto out;
	ret = ps4_ccp_reference(key, expected, sector);
	if (!ret && memcmp(buf, expected, PS4_CCP_SECTOR))
		ret = -EBADMSG;
out:
	ps4_samu_unlock();
	kfree(expected);
	if (ret)
		return ret;

	ret = ps4_ccp_map_selftest(key, samu_key);
	ps4_ccp_map_ok = !ret;
	if (ret)
		pr_warn("mapped xts self test failed: %d, using the copy path\n",
			ret);
	else
		pr_info("mapped xts self test passed\n");
	return 0;
}

struct ps4crypt_c {
	struct dm_target *ti;
	struct dm_dev *dev;
	sector_t start;
	u64 iv_offset;
	u8 key[PS4_CCP_KEY_SIZE];
	struct workqueue_struct *wq;
	struct bio_set bs;
};

struct ps4crypt_io {
	struct work_struct work;
	struct ps4crypt_c *pc;
	struct bio *bio;
	sector_t offset;
	blk_status_t status;
};

#define PS4CRYPT_MAX_PAGES	(SZ_1M / PAGE_SIZE + 1)

static struct page *ps4crypt_pages[PS4CRYPT_MAX_PAGES];

static unsigned int ps4crypt_collect(struct bio *bio, unsigned int *first)
{
	struct bvec_iter iter;
	struct bio_vec bv;
	unsigned int n = 0, end = 0;

	bio_for_each_segment(bv, bio, iter) {
		if (n == PS4CRYPT_MAX_PAGES)
			return 0;
		if (n ? bv.bv_offset || end != PAGE_SIZE :
			bv.bv_offset % PS4_CCP_SECTOR)
			return 0;
		if (!n)
			*first = bv.bv_offset;
		ps4crypt_pages[n++] = bv.bv_page;
		end = bv.bv_offset + bv.bv_len;
	}
	return n;
}

static int ps4crypt_decrypt_mapped(struct ps4crypt_c *pc, struct bio *bio,
				   u64 sector)
{
	unsigned int count, first = 0;
	u64 va;
	int ret;

	count = ps4crypt_collect(bio, &first);
	if (!count)
		return -EAGAIN;
	ret = ps4_samu_map(ps4crypt_pages, count, &va);
	if (ret)
		return -EAGAIN;
	ret = ps4_ccp_run(pc->key, va + first, sector,
			  bio->bi_iter.bi_size / PS4_CCP_SECTOR, false);
	ps4_samu_unmap();
	if (!ret)
		ps4_ccp_mapped++;
	return ret;
}

static int ps4crypt_decrypt_copy(struct ps4crypt_c *pc, struct bio *bio,
				 u64 sector)
{
	unsigned int bytes = bio->bi_iter.bi_size;
	struct bvec_iter iter;
	struct bio_vec bv;
	size_t window, off;
	u64 va;
	u8 *buf;
	int ret;

	buf = ps4_samu_data(&va, &window);
	if (!buf)
		return -ENODEV;
	if (bytes > window)
		return -E2BIG;
	off = 0;
	bio_for_each_segment(bv, bio, iter) {
		memcpy_from_bvec(buf + off, &bv);
		off += bv.bv_len;
	}
	ret = ps4_ccp_run(pc->key, va, sector, bytes / PS4_CCP_SECTOR, false);
	if (ret)
		return ret;
	off = 0;
	bio_for_each_segment(bv, bio, iter) {
		memcpy_to_bvec(&bv, buf + off);
		off += bv.bv_len;
	}
	return 0;
}

static int ps4crypt_decrypt(struct ps4crypt_c *pc, struct bio *bio, u64 sector)
{
	int ret = -EAGAIN;

	if (bio->bi_iter.bi_size % PS4_CCP_SECTOR)
		return -EIO;

	ps4_samu_lock();
	if (ps4_ccp_map_ok)
		ret = ps4crypt_decrypt_mapped(pc, bio, sector);
	if (ret == -EAGAIN)
		ret = ps4crypt_decrypt_copy(pc, bio, sector);
	if (!ret) {
		ps4_ccp_ops++;
		ps4_ccp_sectors += bio->bi_iter.bi_size / PS4_CCP_SECTOR;
	}
	ps4_samu_unlock();
	return ret;
}

static void ps4crypt_read_work(struct work_struct *work)
{
	struct ps4crypt_io *io = container_of(work, struct ps4crypt_io, work);
	struct ps4crypt_c *pc = io->pc;
	struct bio *bio = io->bio;
	int ret;

	if (!io->status) {
		ret = ps4crypt_decrypt(pc, bio, pc->iv_offset + io->offset);
		io->status = errno_to_blk_status(ret);
	}
	bio->bi_status = io->status;
	bio_endio(bio);
}

static void ps4crypt_read_endio(struct bio *clone)
{
	struct ps4crypt_io *io = clone->bi_private;

	io->status = clone->bi_status;
	bio_put(clone);
	queue_work(io->pc->wq, &io->work);
}

static int ps4crypt_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	u8 key[PS4_CCP_KEY_SIZE];
	unsigned long long start, iv_offset;
	struct ps4crypt_c *pc;
	char dummy;
	int ret;

	if (argc != 4) {
		ti->error = "need <key hex> <device> <start> <iv offset>";
		return -EINVAL;
	}
	if (strlen(argv[0]) != 2 * PS4_CCP_KEY_SIZE ||
	    hex2bin(key, argv[0], PS4_CCP_KEY_SIZE)) {
		ti->error = "key must be 64 hex digits";
		return -EINVAL;
	}
	if (sscanf(argv[2], "%llu%c", &start, &dummy) != 1 ||
	    sscanf(argv[3], "%llu%c", &iv_offset, &dummy) != 1) {
		ti->error = "bad start or iv offset";
		return -EINVAL;
	}
	if (dm_table_get_mode(ti->table) & BLK_OPEN_WRITE) {
		ti->error = "ps4crypt is read only for now";
		return -EROFS;
	}

	pc = kzalloc(sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;
	pc->ti = ti;
	pc->start = start;
	pc->iv_offset = iv_offset;
	ps4_ccp_reverse_key(pc->key, key);
	memzero_explicit(key, sizeof(key));

	ret = dm_get_device(ti, argv[1], dm_table_get_mode(ti->table), &pc->dev);
	if (ret) {
		ti->error = "device lookup failed";
		goto err_free;
	}
	pc->wq = alloc_workqueue("ps4crypt", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!pc->wq) {
		ret = -ENOMEM;
		goto err_dev;
	}
	ret = bioset_init(&pc->bs, BIO_POOL_SIZE, 0, 0);
	if (ret)
		goto err_wq;
	ret = dm_set_target_max_io_len(ti, SZ_1M >> SECTOR_SHIFT);
	if (ret)
		goto err_bs;

	ti->per_io_data_size = sizeof(struct ps4crypt_io);
	ti->num_flush_bios = 1;
	ti->private = pc;
	return 0;

err_bs:
	bioset_exit(&pc->bs);
err_wq:
	destroy_workqueue(pc->wq);
err_dev:
	dm_put_device(ti, pc->dev);
err_free:
	memzero_explicit(pc, sizeof(*pc));
	kfree(pc);
	return ret;
}

static void ps4crypt_dtr(struct dm_target *ti)
{
	struct ps4crypt_c *pc = ti->private;

	destroy_workqueue(pc->wq);
	bioset_exit(&pc->bs);
	dm_put_device(ti, pc->dev);
	memzero_explicit(pc, sizeof(*pc));
	kfree(pc);
}

static int ps4crypt_map(struct dm_target *ti, struct bio *bio)
{
	struct ps4crypt_c *pc = ti->private;
	struct ps4crypt_io *io;
	struct bio *clone;

	if (bio_op(bio) == REQ_OP_FLUSH) {
		bio_set_dev(bio, pc->dev->bdev);
		return DM_MAPIO_REMAPPED;
	}
	if (bio_op(bio) != REQ_OP_READ)
		return DM_MAPIO_KILL;

	io = dm_per_bio_data(bio, sizeof(*io));
	io->pc = pc;
	io->bio = bio;
	io->offset = dm_target_offset(ti, bio->bi_iter.bi_sector);
	io->status = BLK_STS_OK;
	INIT_WORK(&io->work, ps4crypt_read_work);

	clone = bio_alloc_clone(pc->dev->bdev, bio, GFP_NOIO, &pc->bs);
	if (!clone)
		return DM_MAPIO_KILL;
	clone->bi_iter.bi_sector = pc->start + io->offset;
	clone->bi_private = io;
	clone->bi_end_io = ps4crypt_read_endio;
	submit_bio_noacct(clone);
	return DM_MAPIO_SUBMITTED;
}

static void ps4crypt_status(struct dm_target *ti, status_type_t type,
			    unsigned int status_flags, char *result,
			    unsigned int maxlen)
{
	struct ps4crypt_c *pc = ti->private;
	unsigned int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;
	case STATUSTYPE_TABLE:
		DMEMIT("- %s %llu %llu", pc->dev->name,
		       (unsigned long long)pc->start, pc->iv_offset);
		break;
	case STATUSTYPE_IMA:
		result[0] = '\0';
		break;
	}
}

static int ps4crypt_iterate_devices(struct dm_target *ti,
				    iterate_devices_callout_fn fn, void *data)
{
	struct ps4crypt_c *pc = ti->private;

	return fn(ti, pc->dev, pc->start, ti->len, data);
}

static struct target_type ps4crypt_target = {
	.name = "ps4crypt",
	.version = { 1, 0, 0 },
	.module = THIS_MODULE,
	.ctr = ps4crypt_ctr,
	.dtr = ps4crypt_dtr,
	.map = ps4crypt_map,
	.status = ps4crypt_status,
	.iterate_devices = ps4crypt_iterate_devices,
};

static ssize_t eap_hdd_key_read(struct file *file, struct kobject *kobj,
				const struct bin_attribute *attr, char *buf,
				loff_t off, size_t count)
{
	return memory_read_from_buffer(buf, count, &off, ps4_eap_key,
				       PS4_EAP_KEY_SIZE);
}

static const BIN_ATTR_ADMIN_RO(eap_hdd_key, PS4_EAP_KEY_SIZE);

static void ps4_ccp_load_eap_key(void)
{
	void *data = NULL;
	size_t size;
	ssize_t ret;

	wait_for_initramfs();
	ret = kernel_read_file_from_path(PS4_EAP_KEY_PATH, 0, &data,
					 PS4_EAP_KEY_SIZE, &size,
					 READING_UNKNOWN);
	if (ret != PS4_EAP_KEY_SIZE) {
		pr_info("no eap hdd key from the loader (%zd)\n", ret);
		vfree(data);
		return;
	}
	memcpy(ps4_eap_key, data, PS4_EAP_KEY_SIZE);
	memzero_explicit(data, PS4_EAP_KEY_SIZE);
	vfree(data);

	ps4_fw_kobj = kobject_create_and_add("ps4", firmware_kobj);
	if (!ps4_fw_kobj || sysfs_create_bin_file(ps4_fw_kobj, &bin_attr_eap_hdd_key)) {
		pr_err("could not create /sys/firmware/ps4/eap_hdd_key\n");
		return;
	}
	pr_info("eap hdd key from the loader at /sys/firmware/ps4/eap_hdd_key\n");
}

static int __init ps4_ccp_init(void)
{
	int ret;

	ps4_ccp_load_eap_key();

	ret = ps4_ccp_selftest();
	if (ret) {
		pr_err("xts self test failed: %d\n", ret);
		return ret;
	}

	ps4_ccp_batch_max = SZ_1M / PS4_CCP_SECTOR;
	ps4_ccp_batch = kcalloc(ps4_ccp_batch_max, sizeof(*ps4_ccp_batch),
				GFP_KERNEL);
	ps4_ccp_wq = alloc_ordered_workqueue("ps4-ccp", WQ_MEM_RECLAIM);
	if (!ps4_ccp_batch || !ps4_ccp_wq) {
		kfree(ps4_ccp_batch);
		if (ps4_ccp_wq)
			destroy_workqueue(ps4_ccp_wq);
		return -ENOMEM;
	}
	INIT_WORK(&ps4_ccp_work, ps4_ccp_worker);

	ret = crypto_register_skcipher(&ps4_ccp_alg);
	if (ret) {
		pr_err("crypto_register_skcipher failed: %d\n", ret);
		return ret;
	}
	pr_info("xts self test passed, registered xts-aes-ps4ccp\n");

	ret = dm_register_target(&ps4crypt_target);
	if (ret)
		pr_err("dm target ps4crypt failed: %d\n", ret);
	else
		pr_info("registered dm target ps4crypt\n");
	return 0;
}
late_initcall(ps4_ccp_init);

MODULE_AUTHOR("Armandas Kvietkus <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("PS4 SAMU CCP XTS-AES driver");
MODULE_LICENSE("GPL");
