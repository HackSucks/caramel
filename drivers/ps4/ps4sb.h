#ifndef _PS4SB_H
#define _PS4SB_H

#include <linux/io.h>
#include <linux/pci.h>
#include <linux/i2c.h>
#include <asm/ps4.h>

struct apcie_msi_domain_ctx;

#define ICC_REPLY 0x4000
#define ICC_EVENT 0x8000

#define ICC_MAGIC 0x42
#define ICC_EVENT_MAGIC 0x24

struct icc_message_hdr {
	u8 magic;
 	u8 major;
 	u16 minor;
	u16 unknown;
	u16 cookie;
	u16 length;
	u16 checksum;
} __packed;

#define ICC_HDR_SIZE sizeof(struct icc_message_hdr)
#define ICC_MIN_SIZE 0x20
#define ICC_MAX_SIZE 0x7f0
#define ICC_MIN_PAYLOAD (ICC_MIN_SIZE - ICC_HDR_SIZE)
#define ICC_MAX_PAYLOAD (ICC_MAX_SIZE - ICC_HDR_SIZE)

#define ICC_TIMEOUT 15

#define ICC_REG_DOORBELL 0x804
#define ICC_REG_STATUS 0x814
#define ICC_REG_IRQ_MASK 0x824
#define ICC_SEND 0x01
#define ICC_ACK 0x02
#define ICC_SUBFUNC 3

#define ICC_SPM_BASE 0x2c000
#define ICC_SPM_SIZE 0x1000
#define ICC_SPM_REQUEST 0x0
#define ICC_SPM_REPLY 0x800

#define PS4SB_FUNC_ID_MEM 6

struct ps4sb_family {
	const char *name;
	unsigned int uart_bar;
	u32 uart_base;
	unsigned int uart_clock;
	unsigned int uart_subfunc;
	int uart_count;
	unsigned int icc_bar;
	u32 icc_base;
	bool icc_claim_regs;
	bool icc_atomic_power;
	bool icc_usb_power;
	bool icc_boot_led;
	u8 i2c_data_off;
	u8 i2c_err_off;
	unsigned int rtc_bar;
	u32 rtc_reg;
	u32 rtc_status;
	bool rtc_byte_regs;
	s64 rtc_epoch;
};

struct ps4sb_icc_dev {
	phys_addr_t spm_base;
	void __iomem *spm;
	void __iomem *regs;

	spinlock_t reply_lock;
	bool reply_pending;

	struct icc_message_hdr request;
	struct icc_message_hdr reply;
	u16 reply_extra_checksum;
	void *reply_buffer;
	int reply_length;
	wait_queue_head_t wq;

	struct i2c_adapter i2c;
	struct input_dev *pwrbutton_dev;
};

struct ps4sb_dev {
	struct pci_dev *pdev;
	const struct ps4sb_family *family;
	struct irq_domain *irqdomain;
	struct apcie_msi_domain_ctx *msi_domain_ctx;
	void __iomem *bar0;
	void __iomem *bar2;
	void __iomem *bar4;

	int nvec;
	int irq_map[100];
	int serial_line[2];
	struct ps4sb_icc_dev icc;
};

#define BUF_FULL 0x7f0
#define BUF_EMPTY 0x7f4
#define HDR(x) (offsetof(struct icc_message_hdr, x))

#define ICC_MAJOR	'I'

 struct icc_cmd {
 	u8 major;
 	u16 minor;
 	void __user *data;
 	u16 length;
 	void __user *reply;
 	u16 reply_length;
 };

#define ICC_IOCTL_CMD _IOWR(ICC_MAJOR, 1, struct icc_cmd)

#define sc_err(...) dev_err(&sc->pdev->dev, __VA_ARGS__)
#define sc_warn(...) dev_warn(&sc->pdev->dev, __VA_ARGS__)
#define sc_notice(...) dev_notice(&sc->pdev->dev, __VA_ARGS__)
#define sc_info(...) dev_info(&sc->pdev->dev, __VA_ARGS__)
#define sc_dbg(...) dev_dbg(&sc->pdev->dev, __VA_ARGS__)

static inline void __iomem *ps4sb_bar(struct ps4sb_dev *sc, unsigned int bar)
{
	return bar == 2 ? sc->bar2 : sc->bar4;
}

static inline int ps4sb_irqnum(struct ps4sb_dev *sc, int index)
{
	return sc->nvec > 1 ? sc->pdev->irq + index : sc->pdev->irq;
}

int ps4sb_uart_init(struct ps4sb_dev *sc);
void ps4sb_uart_remove(struct ps4sb_dev *sc);
int ps4sb_icc_init(struct ps4sb_dev *sc);
void ps4sb_icc_remove(struct ps4sb_dev *sc);
#ifdef CONFIG_PM
void ps4sb_uart_suspend(struct ps4sb_dev *sc, pm_message_t state);
void ps4sb_uart_resume(struct ps4sb_dev *sc);
void ps4sb_icc_suspend(struct ps4sb_dev *sc, pm_message_t state);
void ps4sb_icc_resume(struct ps4sb_dev *sc);
#endif
#ifdef CONFIG_RTC_CLASS
int ps4sb_rtc_init(struct ps4sb_dev *sc);
#else
static inline int ps4sb_rtc_init(struct ps4sb_dev *sc)
{
	return 0;
}
#endif
int icc_i2c_init(struct ps4sb_dev *sc);
void icc_i2c_remove(struct ps4sb_dev *sc);
int icc_pwrbutton_init(struct ps4sb_dev *sc);
void icc_pwrbutton_remove(struct ps4sb_dev *sc);
void icc_pwrbutton_trigger(struct ps4sb_dev *sc, int state);

#endif
