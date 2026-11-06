#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <asm/ps4.h>
#include "ps4sb.h"


static struct ps4sb_dev *icc_sc;

DEFINE_MUTEX(icc_mutex);

#define ICC_REGION_SIZE 0x1000
#define REQUEST (sc->icc.spm + ICC_SPM_REQUEST)
#define REPLY (sc->icc.spm + ICC_SPM_REPLY)

static u16 checksum(const void *p, int length)
{
	const u8 *pp = p;
	u16 sum = 0;
	while (length--)
		sum += *pp++;
	return sum;
}

static void dump_message(struct ps4sb_dev *sc, int offset)
{
	int len;
	struct icc_message_hdr hdr;
	memcpy_fromio(&hdr, sc->icc.spm + offset, ICC_HDR_SIZE);

	sc_err("icc: hdr: [%02x] %02x:%04x unk %x #%d len %d cksum 0x%x\n",
	       hdr.magic, hdr.major, hdr.minor, hdr.unknown, hdr.cookie,
	       hdr.length, hdr.checksum);
	len = min(hdr.length - ICC_HDR_SIZE, ICC_MAX_PAYLOAD);
	if (len > 0) {
		sc_err("icc: data:");
		while (len--)
			printk(" %02x", ioread8(sc->icc.spm + (offset++) +
			                         ICC_HDR_SIZE));
		printk("\n");
	}
}

#define ICC_EVENT_RING 8

struct icc_event {
	u16 minor;
	u16 length;
	u8 data[ICC_MAX_PAYLOAD];
};

static void icc_event_work_fn(struct work_struct *work);

static DEFINE_SPINLOCK(icc_event_lock);
static DECLARE_WORK(icc_event_work, icc_event_work_fn);
static struct icc_event icc_event_ring[ICC_EVENT_RING];
static struct icc_event icc_event_current;
static unsigned int icc_event_head;
static unsigned int icc_event_tail;
static u8 icc_event_major;
static ps4sb_icc_event_fn icc_event_handler;
static void *icc_event_ctx;

static bool icc_event_queue(struct ps4sb_dev *sc, struct icc_message_hdr *msg)
{
	struct icc_event *event;
	unsigned long flags;
	bool queued = false;
	u16 length;

	spin_lock_irqsave(&icc_event_lock, flags);
	if (icc_event_handler && msg->major == icc_event_major &&
	    icc_event_head - icc_event_tail < ICC_EVENT_RING) {
		length = msg->length > ICC_HDR_SIZE ? msg->length - ICC_HDR_SIZE : 0;
		length = min_t(u16, length, ICC_MAX_PAYLOAD);
		event = &icc_event_ring[icc_event_head % ICC_EVENT_RING];
		event->minor = msg->minor;
		event->length = length;
		memcpy_fromio(event->data, sc->icc.spm + ICC_SPM_REPLY +
			      ICC_HDR_SIZE, length);
		icc_event_head++;
		queued = true;
	}
	spin_unlock_irqrestore(&icc_event_lock, flags);

	if (queued)
		schedule_work(&icc_event_work);
	return queued;
}

static void icc_event_work_fn(struct work_struct *work)
{
	ps4sb_icc_event_fn handler;
	unsigned long flags;
	void *ctx;

	for (;;) {
		spin_lock_irqsave(&icc_event_lock, flags);
		if (icc_event_head == icc_event_tail) {
			spin_unlock_irqrestore(&icc_event_lock, flags);
			return;
		}
		icc_event_current = icc_event_ring[icc_event_tail % ICC_EVENT_RING];
		icc_event_tail++;
		handler = icc_event_handler;
		ctx = icc_event_ctx;
		spin_unlock_irqrestore(&icc_event_lock, flags);

		if (handler)
			handler(ctx, icc_event_current.minor,
				icc_event_current.data, icc_event_current.length);
	}
}

int ps4sb_icc_register_event_handler(u8 major, ps4sb_icc_event_fn fn, void *ctx)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&icc_event_lock, flags);
	if (icc_event_handler) {
		ret = -EBUSY;
	} else {
		icc_event_major = major;
		icc_event_ctx = ctx;
		icc_event_tail = icc_event_head;
		icc_event_handler = fn;
	}
	spin_unlock_irqrestore(&icc_event_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(ps4sb_icc_register_event_handler);

void ps4sb_icc_unregister_event_handler(u8 major)
{
	unsigned long flags;

	spin_lock_irqsave(&icc_event_lock, flags);
	if (icc_event_handler && icc_event_major == major) {
		icc_event_handler = NULL;
		icc_event_ctx = NULL;
		icc_event_tail = icc_event_head;
	}
	spin_unlock_irqrestore(&icc_event_lock, flags);
	flush_work(&icc_event_work);
}
EXPORT_SYMBOL_GPL(ps4sb_icc_unregister_event_handler);

static void handle_event(struct ps4sb_dev *sc, struct icc_message_hdr *msg)
{
	switch ((msg->major << 16) | msg->minor) {
		case 0x088010:
			icc_pwrbutton_trigger(sc, 1);
			break;
		case 0x088011:
			icc_pwrbutton_trigger(sc, 0);
			break;
		default:
			if (icc_event_queue(sc, msg))
				break;
			sc_err("icc: event arrived, not yet supported.\n");
			dump_message(sc, ICC_SPM_REPLY);
			break;
	}
}

static void handle_message(struct ps4sb_dev *sc)
{
	u32 rep_empty, rep_full;
	int off, copy_size;
	struct icc_message_hdr msg;

	rep_empty = ioread32(REPLY + BUF_EMPTY);
	rep_full = ioread32(REPLY + BUF_FULL);

	if (rep_empty != 0 || rep_full != 1) {
		sc_err("icc: reply buffer in bad state (%d, %d)\n",
			rep_empty, rep_full);
		return;
	}

	memcpy_fromio(&msg, REPLY, ICC_HDR_SIZE);

	if (msg.minor & ICC_EVENT) {
		if (msg.magic != ICC_EVENT_MAGIC) {
			sc_err("icc: event has bad magic\n");
			dump_message(sc, ICC_SPM_REPLY);
			return;
		}
		handle_event(sc, &msg);
	} else if (msg.minor & ICC_REPLY) {
		if (msg.magic != ICC_MAGIC) {
			sc_err("icc: reply has bad magic\n");
			dump_message(sc, ICC_SPM_REPLY);
			return;
		}
		spin_lock(&sc->icc.reply_lock);
		if (!sc->icc.reply_pending) {
			spin_unlock(&sc->icc.reply_lock);
			sc_err("icc: unexpected reply\n");
			dump_message(sc, ICC_SPM_REPLY);
			return;
		}
		if (msg.cookie != sc->icc.request.cookie) {
			spin_unlock(&sc->icc.reply_lock);
			sc_err("icc: reply has bad cookie %d\n", msg.cookie);
			dump_message(sc, ICC_SPM_REPLY);
			return;
		}
		if (msg.length < ICC_HDR_SIZE || msg.length > ICC_MAX_SIZE) {
			spin_unlock(&sc->icc.reply_lock);
			sc_err("icc: reply has bad length %d\n", msg.length);
			dump_message(sc, ICC_SPM_REPLY);
			return;
		}
		off = ICC_HDR_SIZE;
		copy_size = min(sc->icc.reply_length,
				(int)(msg.length - off));
		memcpy_fromio(sc->icc.reply_buffer, REPLY + off, copy_size);
		off += copy_size;
		sc->icc.reply_extra_checksum = 0;
		while (off < msg.length)
			sc->icc.reply_extra_checksum += ioread8(REPLY + off++);
		sc->icc.reply_pending = false;
		sc->icc.reply_length = copy_size;
		sc->icc.reply = msg;
		spin_unlock(&sc->icc.reply_lock);
		wake_up(&sc->icc.wq);
	} else {
		sc_err("icc: unknown message arrived\n");
		dump_message(sc, ICC_SPM_REPLY);
	}
}

static irqreturn_t icc_interrupt(int irq, void *arg)
{
	struct ps4sb_dev *sc = arg;
	u32 pending;
	u32 status;
	u32 ret = IRQ_NONE;
	int loops;

	for (loops = 0; loops < 64; loops++) {
		status = ioread32(sc->icc.regs + ICC_REG_STATUS);
		pending = status & (ICC_SEND | ICC_ACK);
		if (!pending)
			return ret;

		if (pending & ICC_ACK) {
			iowrite32(ICC_ACK, sc->icc.regs + ICC_REG_STATUS);
			ret = IRQ_HANDLED;
		}

		if (pending & ICC_SEND) {
			iowrite32(ICC_SEND, sc->icc.regs + ICC_REG_STATUS);
			handle_message(sc);
			iowrite32(0, REPLY + BUF_FULL);
			iowrite32(1, REPLY + BUF_EMPTY);
			iowrite32(ICC_ACK, sc->icc.regs + ICC_REG_DOORBELL);
			ret = IRQ_HANDLED;
		}
	}

	dev_warn_ratelimited(&sc->pdev->dev,
			     "icc: IRQ status stuck after 64 loops (status=0x%08x)\n",
			     status);

	return ret;
}

static int icc_post_request(struct ps4sb_dev *sc, u8 major, u16 minor,
			    const void *data, u16 length)
{
	u32 req_empty, req_full;

	if (length > ICC_MAX_PAYLOAD)
		return -E2BIG;

	sc->icc.request.magic = ICC_MAGIC;
	sc->icc.request.major = major;
	sc->icc.request.minor = minor;
	sc->icc.request.cookie++;
	sc->icc.request.length = ICC_HDR_SIZE + length;
	sc->icc.request.checksum = 0;
	if (sc->icc.request.length < ICC_MIN_SIZE)
		sc->icc.request.length = ICC_MIN_SIZE;

	sc->icc.request.checksum = checksum(&sc->icc.request, ICC_HDR_SIZE);
	sc->icc.request.checksum += checksum(data, length);

	req_empty = ioread32(REQUEST + BUF_EMPTY);
	req_full = ioread32(REQUEST + BUF_FULL);

	if (req_empty != 1 || req_full != 0) {
		sc_err("icc: request buffer is busy: empty=%d full=%d\n",
		       req_empty, req_full);
		return -EIO;
	}

	iowrite32(0, REQUEST + BUF_EMPTY);

	memcpy_toio(REQUEST, &sc->icc.request, ICC_HDR_SIZE);
	memcpy_toio(REQUEST + ICC_HDR_SIZE, data, length);
	if (length < ICC_MIN_PAYLOAD)
		memset_io(REQUEST + ICC_HDR_SIZE + length, 0,
			  ICC_MIN_PAYLOAD - length);

	iowrite32(1, REQUEST + BUF_FULL);
	return 0;
}

static int _ps4sb_icc_cmd(struct ps4sb_dev *sc, u8 major, u16 minor, const void *data,
		    u16 length, void *reply, u16 reply_length, bool intr)
{
	int ret;
	u16 rep_checksum;

	sc->icc.reply_buffer = reply;
	sc->icc.reply_length = reply_length;

	ret = icc_post_request(sc, major, minor, data, length);
	if (ret)
		return ret;

	spin_lock_irq(&sc->icc.reply_lock);
	sc->icc.reply_pending = true;
	spin_unlock_irq(&sc->icc.reply_lock);

	iowrite32(ICC_SEND, sc->icc.regs + ICC_REG_DOORBELL);

	if (intr)
		ret = wait_event_interruptible_timeout(sc->icc.wq,
				!sc->icc.reply_pending, HZ * ICC_TIMEOUT);
	else
		ret = wait_event_timeout(sc->icc.wq,
				!sc->icc.reply_pending, HZ * ICC_TIMEOUT);

	spin_lock_irq(&sc->icc.reply_lock);
	sc->icc.reply_buffer = NULL;
	if (ret < 0 || sc->icc.reply_pending) {
		sc->icc.reply_pending = false;
		spin_unlock_irq(&sc->icc.reply_lock);
		sc_err("icc: interrupted or timeout: ret = %d\n", ret);
		return ret < 0 ? -EINTR : -ETIMEDOUT;
	}
	spin_unlock_irq(&sc->icc.reply_lock);

	rep_checksum = sc->icc.reply.checksum;
	sc->icc.reply.checksum = 0;
	rep_checksum -= checksum(&sc->icc.reply, ICC_HDR_SIZE);
	rep_checksum -= checksum(reply, sc->icc.reply_length);
	rep_checksum -= sc->icc.reply_extra_checksum;

	if (rep_checksum) {
		sc_err("icc: checksum mismatch (diff: %x)\n", rep_checksum);
		return -EIO;
	}

	if (sc->icc.reply.major != major ) {
		sc_err("icc: major mismatch\n");
		return -EIO;
	}
	if (sc->icc.reply.minor != (minor | ICC_REPLY) ) {
		sc_err("icc: minor mismatch\n");
		return -EIO;
	}

	return sc->icc.reply.length - ICC_HDR_SIZE;
}

int ps4sb_icc_cmd(u8 major, u16 minor, const void *data, u16 length,
		   void *reply, u16 reply_length)
{
	int ret;

	mutex_lock(&icc_mutex);
	if (!icc_sc) {
		pr_err("icc: not ready\n");
		ret = -EAGAIN;
		goto out_unlock;
	}
	ret = _ps4sb_icc_cmd(icc_sc, major, minor, data, length, reply, reply_length,
		       false);
out_unlock:
	mutex_unlock(&icc_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(ps4sb_icc_cmd);

static int ps4sb_icc_post_atomic(u8 major, u16 minor, const void *data, u16 length)
{
	struct ps4sb_dev *sc = READ_ONCE(icc_sc);
	int ret;

	if (!sc)
		return -ENODEV;
	if (reboot_force || READ_ONCE(sc->icc.reply_pending))
		return -EBUSY;

	sc->icc.reply_buffer = NULL;
	sc->icc.reply_length = 0;
	ret = icc_post_request(sc, major, minor, data, length);
	if (ret)
		return ret;

	iowrite32(ICC_SEND | ICC_ACK, sc->icc.regs + ICC_REG_STATUS);
	iowrite32(ICC_SEND, sc->icc.regs + ICC_REG_DOORBELL);
	return 0;
}

static void icc_usb_power_on(void)
{
	u8 on = 1;
	u8 resp[20];
	int ret;

	ret = ps4sb_icc_cmd(5, 0x11, NULL, 0, resp, 20);
	if (ret < 0) {
		pr_err("icc: USB status failed: %d\n", ret);
	} else if (resp[2]) {
		return;
	}

	ret = ps4sb_icc_cmd(5, 0x10, &on, sizeof(on), resp, 20);
	if (ret < 0)
		pr_err("icc: USB enable failed: %d\n", ret);
}

static void icc_wlan_power_on(void)
{
	u8 on = 3;
	u8 resp[20];
	int ret;

	ret = ps4sb_icc_cmd(5, 0, &on, sizeof(on), resp, 20);
	if (ret < 0)
		pr_err("icc: BT/WLAN enable failed: %d\n", ret);
}

static void icc_set_led(u8 blue, u8 white)
{
	const u8 led_config[] = {
		2, 1, 0, 0,
			0x10, 1,
				2, blue, 2, 1, 0x00,
			0x11, 1,
				2, white, 2, 1, 0x00,
	};
	u8 reply[0x30];
	int ret;

	ret = ps4sb_icc_cmd(9, 0x20, led_config, ARRAY_SIZE(led_config), reply, 0x30);
	if (ret < 0)
		pr_err("icc: LED set failed: %d\n", ret);
}

static void do_icc_init(void)
{
	u8 svc = 0x10;
	u8 reply[0x30];
	int ret;

	ret = ps4sb_icc_cmd(2, 6, NULL, 0, reply, 0x30);
	if (ret < 0)
		pr_err("icc: firmware query failed: %d\n", ret);
	ret = ps4sb_icc_cmd(1, 0, &svc, 1, reply, 0x30);
	if (ret < 0)
		pr_err("icc: service init failed: %d\n", ret);

	icc_set_led(0x00, 0xff);
}

static int __init icc_boot_complete(void)
{
	if (icc_sc && icc_sc->family->icc_boot_led)
		icc_set_led(0xff, 0x00);
	return 0;
}
late_initcall(icc_boot_complete);

static void icc_power_request(u8 restart)
{
	struct ps4sb_dev *sc = READ_ONCE(icc_sc);
	uint8_t command[] = {
		0, restart, 2, 0, 1, 0
	};

	if (!sc)
		return;

	if (sc->family->icc_atomic_power) {
		if (ps4sb_icc_post_atomic(4, 1, command, sizeof(command)))
			return;
		mdelay(3000);
	} else {
		ps4sb_icc_cmd(4, 1, command, sizeof(command), NULL, 0);
		msleep(3000);
	}
	WARN_ON(1);
}

static void icc_shutdown(void)
{
	icc_power_request(0);
}

void ps4sb_icc_reboot(void)
{
	icc_power_request(1);
}

static struct sys_off_handler *icc_sys_off;

static int icc_sys_off_cb(struct sys_off_data *data)
{
	icc_shutdown();
	return NOTIFY_DONE;
}

static void *ioctl_send_buf;
static void *ioctl_reply_buf;
static DEFINE_MUTEX(icc_ioctl_mutex);
static bool icc_chrdev_registered;
#ifdef CONFIG_PS4_ICCNODE
static struct class *icc_class;
static struct device *icc_device;
#endif

static long icc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uap = (void __user *)arg;
	struct icc_cmd req;
	size_t copy_len;
	int ret;

	if (cmd != ICC_IOCTL_CMD)
		return -ENOENT;
	if (copy_from_user(&req, uap, sizeof(req)))
		return -EFAULT;
	if (req.length > ICC_MAX_PAYLOAD || req.reply_length > ICC_MAX_SIZE)
		return -E2BIG;
	if ((req.length && !req.data) || (req.reply_length && !req.reply))
		return -EINVAL;

	mutex_lock(&icc_ioctl_mutex);
	if (req.length && copy_from_user(ioctl_send_buf, req.data, req.length)) {
		ret = -EFAULT;
		goto out;
	}

	ret = ps4sb_icc_cmd(req.major, req.minor,
			    req.length ? ioctl_send_buf : NULL, req.length,
			    req.reply_length ? ioctl_reply_buf : NULL,
			    req.reply_length);
	if (ret < 0)
		goto out;

	copy_len = min_t(size_t, req.reply_length, ret);
	if (copy_len && copy_to_user(req.reply, ioctl_reply_buf, copy_len))
		ret = -EFAULT;
out:
	mutex_unlock(&icc_ioctl_mutex);
	return ret;
}

static const struct file_operations icc_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = icc_ioctl,
};

int ps4sb_icc_init(struct ps4sb_dev *sc)
{
	int ret;
	unsigned int mem_devfn = PCI_DEVFN(PCI_SLOT(sc->pdev->devfn), PS4SB_FUNC_ID_MEM);
	struct pci_dev *mem_dev;
	u32 req_empty, req_full;

	mem_dev = pci_get_slot(sc->pdev->bus, mem_devfn);
	if (!mem_dev) {
		sc_err("icc: could not get handle to mem device\n");
		return -ENODEV;
	}

	sc->icc.regs = ps4sb_bar(sc, sc->family->icc_bar) + sc->family->icc_base;
	if (sc->family->icc_claim_regs &&
	    !request_mem_region(pci_resource_start(sc->pdev, sc->family->icc_bar) +
				sc->family->icc_base, ICC_REGION_SIZE, "ps4sb.icc")) {
		sc_err("icc: failed to request ICC register region\n");
		ret = -EBUSY;
		goto put_mem_dev;
	}

	sc->icc.spm_base = pci_resource_start(mem_dev, 5) + ICC_SPM_BASE;
	if (!request_mem_region(sc->icc.spm_base, ICC_SPM_SIZE,
				"spm.icc")) {
		sc_err("icc: failed to request ICC SPM region\n");
		ret = -EBUSY;
		goto release_regs;
	}

	sc->icc.spm = ioremap(sc->icc.spm_base, ICC_SPM_SIZE);
	if (!sc->icc.spm) {
		sc_err("icc: failed to map ICC portion of SPM\n");
		ret = -EIO;
		goto release_spm;
	}

	spin_lock_init(&sc->icc.reply_lock);
	init_waitqueue_head(&sc->icc.wq);

	iowrite32(ICC_SEND | ICC_ACK,
		  sc->icc.regs + ICC_REG_STATUS);

	ret = request_irq(ps4sb_irqnum(sc, ICC_SUBFUNC),
			  icc_interrupt, IRQF_SHARED, "icc", sc);
	if (ret) {
		sc_err("icc: could not request IRQ: %d\n", ret);
		goto iounmap;
	}

	req_empty = ioread32(REQUEST + BUF_EMPTY);
	req_full = ioread32(REQUEST + BUF_FULL);

	if (req_empty != 1 || req_full != 0) {
		sc_err("icc: request buffer is busy: empty=%d full=%d\n",
		       req_empty, req_full);
		ret = -EIO;
		goto free_irq;
	}

	mutex_lock(&icc_mutex);
	icc_sc = sc;

	iowrite32(ICC_SEND | ICC_ACK,
		  sc->icc.regs + ICC_REG_IRQ_MASK);
	mutex_unlock(&icc_mutex);

	ret = icc_i2c_init(sc);
	if (ret) {
		sc_err("icc: i2c init failed: %d\n", ret);
		goto unassign_global;
	}
	icc_wlan_power_on();
	if (sc->family->icc_usb_power)
		icc_usb_power_on();

	ret = icc_pwrbutton_init(sc);
	if (ret)
		sc_err("icc: pwrbutton init failed: %d\n", ret);

	do_icc_init();
	icc_sys_off = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
					       SYS_OFF_PRIO_FIRMWARE + 1,
					       icc_sys_off_cb, NULL);
	if (IS_ERR(icc_sys_off)) {
		sc_err("icc: power-off handler failed: %ld\n", PTR_ERR(icc_sys_off));
		icc_sys_off = NULL;
	}

	ioctl_send_buf = kzalloc(ICC_MAX_PAYLOAD, GFP_KERNEL);
	ioctl_reply_buf = kzalloc(ICC_MAX_SIZE, GFP_KERNEL);
	if (!ioctl_send_buf || !ioctl_reply_buf) {
		sc_err("icc: alloc ioctl buffers failed\n");
		goto done;
	}
	ret = register_chrdev(ICC_MAJOR, "icc", &icc_fops);
	if (ret) {
		sc_err("icc: register_chrdev failed: %d\n", ret);
		goto done;
	}
	icc_chrdev_registered = true;

#ifdef CONFIG_PS4_ICCNODE
	icc_class = class_create("icc");
	if (IS_ERR(icc_class)) {
		sc_warn("icc: class_create failed (%ld), /dev/icc unavailable\n",
			PTR_ERR(icc_class));
		icc_class = NULL;
	} else {
		icc_device = device_create(icc_class, NULL,
					   MKDEV(ICC_MAJOR, 0), NULL, "icc");
		if (IS_ERR(icc_device)) {
			sc_warn("icc: device_create failed (%ld), /dev/icc unavailable\n",
				PTR_ERR(icc_device));
			icc_device = NULL;
		}
	}
#endif
 done:
	pci_dev_put(mem_dev);

	return 0;

unassign_global:
	mutex_lock(&icc_mutex);
	iowrite32(0, sc->icc.regs + ICC_REG_IRQ_MASK);
	icc_sc = NULL;
	mutex_unlock(&icc_mutex);
free_irq:
	free_irq(ps4sb_irqnum(sc, ICC_SUBFUNC), sc);
iounmap:
	iounmap(sc->icc.spm);
release_spm:
	release_mem_region(sc->icc.spm_base, ICC_SPM_SIZE);
release_regs:
	if (sc->family->icc_claim_regs)
		release_mem_region(pci_resource_start(sc->pdev, sc->family->icc_bar) +
				   sc->family->icc_base, ICC_REGION_SIZE);
put_mem_dev:
	pci_dev_put(mem_dev);
	return ret;
}

void ps4sb_icc_remove(struct ps4sb_dev *sc)
{
	sc_err("ps4sb_icc_remove: shouldn't normally be called\n");
	if (icc_chrdev_registered) {
#ifdef CONFIG_PS4_ICCNODE
		if (icc_device) {
			device_destroy(icc_class, MKDEV(ICC_MAJOR, 0));
			icc_device = NULL;
		}
		if (icc_class) {
			class_destroy(icc_class);
			icc_class = NULL;
		}
#endif
		unregister_chrdev(ICC_MAJOR, "icc");
		icc_chrdev_registered = false;
	}
	kfree(ioctl_reply_buf);
	kfree(ioctl_send_buf);
	ioctl_reply_buf = NULL;
	ioctl_send_buf = NULL;
	if (icc_sys_off) {
		unregister_sys_off_handler(icc_sys_off);
		icc_sys_off = NULL;
	}
	icc_pwrbutton_remove(sc);
	icc_i2c_remove(sc);
	mutex_lock(&icc_mutex);
	iowrite32(0, sc->icc.regs + ICC_REG_IRQ_MASK);
	icc_sc = NULL;
	mutex_unlock(&icc_mutex);
	free_irq(ps4sb_irqnum(sc, ICC_SUBFUNC), sc);
	iounmap(sc->icc.spm);
	release_mem_region(sc->icc.spm_base, ICC_SPM_SIZE);
	if (sc->family->icc_claim_regs)
		release_mem_region(pci_resource_start(sc->pdev, sc->family->icc_bar) +
				   sc->family->icc_base, ICC_REGION_SIZE);
}

#ifdef CONFIG_PM
void ps4sb_icc_suspend(struct ps4sb_dev *sc, pm_message_t state)
{
}

void ps4sb_icc_resume(struct ps4sb_dev *sc)
{
}
#endif
