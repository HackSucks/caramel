#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <asm/ps4.h>
#include "baikal.h"

#define bpcie_icc_dev abpcie_icc_dev

static struct bpcie_dev *icc_sc;

DEFINE_MUTEX(bcpie_icc_mutex);

#define REQUEST (sc->icc.spm + BPCIE_SPM_ICC_REQUEST)
#define REPLY (sc->icc.spm + BPCIE_SPM_ICC_REPLY)

static u16 checksum(const void *p, int length)
{
	const u8 *pp = p;
	u16 sum = 0;
	while (length--)
		sum += *pp++;
	return sum;
}

static void dump_message(struct bpcie_dev *sc, int offset)
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
static bpcie_icc_event_fn icc_event_handler;
static void *icc_event_ctx;

static bool icc_event_queue(struct bpcie_dev *sc, struct icc_message_hdr *msg)
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
		memcpy_fromio(event->data, sc->icc.spm + BPCIE_SPM_ICC_REPLY +
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
	bpcie_icc_event_fn handler;
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

int bpcie_icc_register_event_handler(u8 major, bpcie_icc_event_fn fn, void *ctx)
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
EXPORT_SYMBOL_GPL(bpcie_icc_register_event_handler);

void bpcie_icc_unregister_event_handler(u8 major)
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
EXPORT_SYMBOL_GPL(bpcie_icc_unregister_event_handler);

static void handle_event(struct bpcie_dev *sc, struct icc_message_hdr *msg)
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
			dump_message(sc, BPCIE_SPM_ICC_REPLY);
			break;
	}
}

static void handle_message(struct bpcie_dev *sc)
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
			dump_message(sc, BPCIE_SPM_ICC_REPLY);
			return;
		}
		handle_event(sc, &msg);
	} else if (msg.minor & ICC_REPLY) {
		if (msg.magic != ICC_MAGIC) {
			sc_err("icc: reply has bad magic\n");
			dump_message(sc, BPCIE_SPM_ICC_REPLY);
			return;
		}
		spin_lock(&sc->icc.reply_lock);
		if (!sc->icc.reply_pending) {
			spin_unlock(&sc->icc.reply_lock);
			sc_err("icc: unexpected reply\n");
			dump_message(sc, BPCIE_SPM_ICC_REPLY);
			return;
		}
		if (msg.cookie != sc->icc.request.cookie) {
			spin_unlock(&sc->icc.reply_lock);
			sc_err("icc: reply has bad cookie %d\n", msg.cookie);
			dump_message(sc, BPCIE_SPM_ICC_REPLY);
			return;
		}
		if (msg.length < ICC_HDR_SIZE || msg.length > ICC_MAX_SIZE) {
			spin_unlock(&sc->icc.reply_lock);
			sc_err("icc: reply has bad length %d\n", msg.length);
			dump_message(sc, BPCIE_SPM_ICC_REPLY);
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
		dump_message(sc, BPCIE_SPM_ICC_REPLY);
	}
}

static irqreturn_t icc_interrupt(int irq, void *arg)
{
	struct bpcie_dev *sc = arg;
	u32 status;
	u32 ret = IRQ_NONE;

	do {
		status = ioread32(sc->bar2 + BPCIE_REG_ICC_STATUS);

		if (status & BPCIE_ICC_ACK) {
			iowrite32(BPCIE_ICC_ACK,
				  sc->bar2 + BPCIE_REG_ICC_STATUS);
			ret = IRQ_HANDLED;
		}

		if (status & BPCIE_ICC_SEND) {
			iowrite32(BPCIE_ICC_SEND,
				  sc->bar2 + BPCIE_REG_ICC_STATUS);
			handle_message(sc);
			iowrite32(0, REPLY + BUF_FULL);
			iowrite32(1, REPLY + BUF_EMPTY);
			iowrite32(BPCIE_ICC_ACK,
				  sc->bar2 + BPCIE_REG_ICC_DOORBELL);
			ret = IRQ_HANDLED;
		}
	} while (status);

	return ret;
}

static int icc_post_request(struct bpcie_dev *sc, u8 major, u16 minor,
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

static int _bpcie_icc_cmd(struct bpcie_dev *sc, u8 major, u16 minor, const void *data,
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

	iowrite32(BPCIE_ICC_SEND, sc->bar2 + BPCIE_REG_ICC_DOORBELL);

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

int bpcie_icc_cmd(u8 major, u16 minor, const void *data, u16 length,
		   void *reply, u16 reply_length)
{
	int ret;

	mutex_lock(&bcpie_icc_mutex);
	if (!icc_sc) {
		pr_err("icc: not ready\n");
		ret = -EAGAIN;
		goto out_unlock;
	}
	ret = _bpcie_icc_cmd(icc_sc, major, minor, data, length, reply, reply_length,
		       false);
out_unlock:
	mutex_unlock(&bcpie_icc_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(bpcie_icc_cmd);

static int bpcie_icc_post_atomic(u8 major, u16 minor, const void *data, u16 length)
{
	struct bpcie_dev *sc = READ_ONCE(icc_sc);
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

	iowrite32(BPCIE_ICC_SEND | BPCIE_ICC_ACK, sc->bar2 + BPCIE_REG_ICC_STATUS);
	iowrite32(BPCIE_ICC_SEND, sc->bar2 + BPCIE_REG_ICC_DOORBELL);
	return 0;
}

static void __maybe_unused bpcie_init_usb(struct bpcie_dev *sc, int usb_no)
{
	u32 value_to_write;
	u32 offset;
	void __iomem *addr;

	offset = usb_no ? 0x68 : 0x64;

	addr = sc->bar2 + BPCIE_USB_BASE + offset;
	value_to_write = ioread32(addr) | 1;
	iowrite32(value_to_write, addr);

	offset = usb_no ? 0x28 : 0x24;

	addr = sc->bar2 + BPCIE_USB_BASE + offset;
	value_to_write = ioread32(addr) | 1;
	iowrite32(value_to_write, addr);

	offset = usb_no ? 0x68 : 0x64;

	addr = sc->bar2 + BPCIE_USB_BASE + offset;
	value_to_write = ioread32(addr) & ~1U;
	iowrite32(value_to_write, addr);
}

static void __maybe_unused resetUsbPort(void)
{
	u8 on = 1;
	u8 resp[20];
	int ret;
	
	ret = bpcie_icc_cmd(5, 0x11, NULL, 0, resp, 20);
	printk("usb0 status: ret=%d, reply %02x %02x %02x %02x", ret, resp[0], resp[1], resp[2], resp[3]);
	if(ret < 0) {
		printk("USB status failed");
	} else if (resp[2]) {
		printk("USB is already turned ON!");
		return;
	}
	ret = bpcie_icc_cmd(5, 0x10, &on, sizeof(on), resp, 20);
	printk("Turn ON USB: ret=%d, reply %02x %02x %02x %02x", ret, resp[0], resp[1], resp[2], resp[3]);
	if(ret < 0)
	{
		printk("Turn on USB failed");
		return;
	}
}

static void __maybe_unused resetBtWlan(void)
{
	u8 on = 3;
	u8 resp[20];
	int ret;
	

	

	ret = bpcie_icc_cmd(5, 0, &on, sizeof(on), resp, 20);
	printk("Turn ON BT/WLAN: ret=%d, reply %02x %02x %02x %02x", ret, resp[0], resp[1], resp[2], resp[3]);
	if(ret < 0)
	{
		printk("Turn on bt/wlan failed");
		return;
	}
}

static void do_icc_init(void) {
	u8 svc = 0x10;
	u8 reply[0x30];
	static const u8 led_config[] = {
		3, 1, 0, 0,
			0x10, 1,
				2, 0xff, 2, 1, 0x00,
			0x11, 1,
				2, 0x00, 2, 1, 0x00,
			0x02, 3,
				1, 0x00, 4, 1, 0xbf,
				2, 0xff, 5, 1, 0xff,
				2, 0x00, 5, 1, 0xff,
	};
	int ret;
	ret = bpcie_icc_cmd(2, 6, NULL, 0, reply, 0x30);
	printk("ret=%d, reply %02x %02x %02x %02x %02x %02x %02x %02x\n", ret,
		reply[0], reply[1], reply[2], reply[3],
		reply[4], reply[5], reply[6], reply[7]);
	ret = bpcie_icc_cmd(1, 0, &svc, 1, reply, 0x30);
	printk("ret=%d, reply %02x %02x %02x %02x %02x %02x %02x %02x\n", ret,
		reply[0], reply[1], reply[2], reply[3],
		reply[4], reply[5], reply[6], reply[7]);

	ret = bpcie_icc_cmd(9, 0x20, led_config, ARRAY_SIZE(led_config), reply, 0x30);
	printk("ret=%d, reply %02x %02x %02x %02x %02x %02x %02x %02x\n", ret,
		reply[0], reply[1], reply[2], reply[3],
		reply[4], reply[5], reply[6], reply[7]);
}

static void icc_power_request(u8 restart)
{
	uint8_t command[] = {
		0, restart, 2, 0, 1, 0
	};
	if (bpcie_status() != 1)
		return;
	if (bpcie_icc_post_atomic(4, 1, command, sizeof(command)))
		return;
	mdelay(3000);
	WARN_ON(1);
}

static void icc_shutdown(void)
{
	icc_power_request(0);
}

void bpcie_icc_reboot(void)
{
	icc_power_request(1);
}

static struct sys_off_handler *icc_sys_off;

static int icc_sys_off_cb(struct sys_off_data *data)
{
	icc_shutdown();
	return NOTIFY_DONE;
}

static void *ioctl_tmp_buf = NULL;
static bool icc_chrdev_registered;

 static long icc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
 {
 	int ret;
 	void __user *uap = (void __user *)arg;
 	switch (cmd) {
 	case ICC_IOCTL_CMD: {
 		struct icc_cmd cmd;
 		int reply_len;
 		ret = copy_from_user(&cmd, uap, sizeof(cmd));
 		if (ret) {
 			ret = -EFAULT;
 			break;
 		}
 		ret = copy_from_user(ioctl_tmp_buf, cmd.data, cmd.length);
 		if (ret) {
 			ret = -EFAULT;
 			break;
 		}
 		reply_len = bpcie_icc_cmd(cmd.major, cmd.minor, ioctl_tmp_buf,
 			cmd.length, ioctl_tmp_buf, cmd.reply_length);
 		if (reply_len < 0) {
 			ret = reply_len;
 			break;
 		}
 		ret = copy_to_user(cmd.reply, ioctl_tmp_buf, cmd.reply_length);
 		if (ret) {
 			ret = -EFAULT;
 			break;
 		}
 		ret = reply_len;
 		} break;
 	default:
 		ret = -ENOENT;
 		break;
 	}
 	return ret;
 }

 static const struct file_operations icc_fops = {
 	.owner = THIS_MODULE,
 	.unlocked_ioctl = icc_ioctl,
 };

int bpcie_icc_init(struct bpcie_dev *sc)
{
	int ret;
	unsigned int mem_devfn = PCI_DEVFN(PCI_SLOT(sc->pdev->devfn), BAIKAL_FUNC_ID_MEM);
	struct pci_dev *mem_dev;
	u32 req_empty, req_full;

	mem_dev = pci_get_slot(sc->pdev->bus, mem_devfn);
	if (!mem_dev) {
		sc_err("icc: could not get handle to mem device\n");
		return -ENODEV;
	}
	sc->icc.spm_base = pci_resource_start(mem_dev, 5) + BPCIE_SPM_ICC_BASE;
	if (!request_mem_region(sc->icc.spm_base, BPCIE_SPM_ICC_SIZE,
				"spm.icc")) {
		sc_err("icc: failed to request ICC SPM region\n");
		ret = -EBUSY;
		goto put_mem_dev;
	}

	sc->icc.spm = ioremap(sc->icc.spm_base, BPCIE_SPM_ICC_SIZE);
	if (!sc->icc.spm) {
		sc_err("icc: failed to map ICC portion of SPM\n");
		ret = -EIO;
		goto release_spm;
	}

	spin_lock_init(&sc->icc.reply_lock);
	init_waitqueue_head(&sc->icc.wq);

	iowrite32(BPCIE_ICC_SEND | BPCIE_ICC_ACK,
		  sc->bar2 + BPCIE_REG_ICC_STATUS);

	ret = request_irq(bpcie_irqnum(sc, BPCIE_SUBFUNC_ICC),
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

	mutex_lock(&bcpie_icc_mutex);
	icc_sc = sc;

	iowrite32(BPCIE_ICC_SEND | BPCIE_ICC_ACK,
		  sc->bar2 + BPCIE_REG_ICC_IRQ_MASK);
	mutex_unlock(&bcpie_icc_mutex);

	ret = icc_i2c_init(sc);
	if (ret) {
		sc_err("icc: i2c init failed: %d\n", ret);
		goto unassign_global;
	}
	resetBtWlan();
	resetUsbPort();

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

	ioctl_tmp_buf = kzalloc(1 << 16, GFP_KERNEL);
 	if (!ioctl_tmp_buf) {
 		sc_err("icc: alloc ioctl_tmp_buf failed\n");
 		goto done;
 	}
 	ret = register_chrdev(ICC_MAJOR, "icc", &icc_fops);
 	if (ret) {
 		sc_err("icc: register_chrdev failed: %d\n", ret);
		kfree(ioctl_tmp_buf);
		ioctl_tmp_buf = NULL;
 		goto done;
 	}
	icc_chrdev_registered = true;
 done:
	pci_dev_put(mem_dev);

	return 0;

unassign_global:
	mutex_lock(&bcpie_icc_mutex);
	iowrite32(0, sc->bar2 + BPCIE_REG_ICC_IRQ_MASK);
	icc_sc = NULL;
	mutex_unlock(&bcpie_icc_mutex);
free_irq:
	free_irq(bpcie_irqnum(sc, BPCIE_SUBFUNC_ICC), sc);
iounmap:
	iounmap(sc->icc.spm);
release_spm:
	release_mem_region(sc->icc.spm_base, BPCIE_SPM_ICC_SIZE);
put_mem_dev:
	pci_dev_put(mem_dev);
	return ret;
}

void bpcie_icc_remove(struct bpcie_dev *sc)
{
	sc_err("bpcie_icc_remove: shouldn't normally be called\n");
	if (icc_chrdev_registered) {
		unregister_chrdev(ICC_MAJOR, "icc");
		icc_chrdev_registered = false;
	}
	kfree(ioctl_tmp_buf);
	ioctl_tmp_buf = NULL;
	if (icc_sys_off) {
		unregister_sys_off_handler(icc_sys_off);
		icc_sys_off = NULL;
	}
	icc_pwrbutton_remove(sc);
	icc_i2c_remove(sc);
	mutex_lock(&bcpie_icc_mutex);
	iowrite32(0, sc->bar2 + BPCIE_REG_ICC_IRQ_MASK);
	icc_sc = NULL;
	mutex_unlock(&bcpie_icc_mutex);
	free_irq(bpcie_irqnum(sc, BPCIE_SUBFUNC_ICC), sc);
	iounmap(sc->icc.spm);
	release_mem_region(sc->icc.spm_base, BPCIE_SPM_ICC_SIZE);
}

#ifdef CONFIG_PM
void bpcie_icc_suspend(struct bpcie_dev *sc, pm_message_t state)
{
}

void bpcie_icc_resume(struct bpcie_dev *sc)
{
}
#endif
