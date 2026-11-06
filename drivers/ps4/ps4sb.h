#ifndef _PS4SB_H
#define _PS4SB_H

#include <linux/io.h>
#include <linux/pci.h>
#include <linux/i2c.h>
#include <asm/ps4.h>

struct ps4sb_msi_domain_ctx;

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
	struct ps4sb_msi_domain_ctx *msi_domain_ctx;
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

enum aeolia_func_id {
	AEOLIA_FUNC_ID_ACPI = 0,
	AEOLIA_FUNC_ID_GBE,
	AEOLIA_FUNC_ID_AHCI,
	AEOLIA_FUNC_ID_SDHCI,
	AEOLIA_FUNC_ID_PCIE,
	AEOLIA_FUNC_ID_DMAC,
	AEOLIA_FUNC_ID_MEM,
	AEOLIA_FUNC_ID_XHCI,

	AEOLIA_NUM_FUNCS
};

/* MSI registers for up to 31, but only 23 known. */
#define AEOLIA_NUM_SUBFUNC		23

/* Sub-functions, aka MSI vectors */
enum aeolia_subfunc {
	AEOLIA_SUBFUNC_GLUE	= 0,
	AEOLIA_SUBFUNC_ICC	= 3,
	AEOLIA_SUBFUNC_HPET	= 5,
	AEOLIA_SUBFUNC_SFLASH	= 11,
	AEOLIA_SUBFUNC_RTC	= 13,
	AEOLIA_SUBFUNC_UART0	= 19,
	AEOLIA_SUBFUNC_UART1	= 20,
	AEOLIA_SUBFUNC_TWSI	= 21,

	AEOLIA_NUM_SUBFUNCS	= 23
};

#define AEOLIA_NR_UARTS 2

/* Relative to BAR2 */
#define AEOLIA_RGN_RTC_BASE		0x0
#define AEOLIA_RGN_RTC_SIZE		0x1000

#define AEOLIA_RGN_CHIPID_BASE		0x1000
#define AEOLIA_RGN_CHIPID_SIZE		0x1000

#define AEOLIA_REG_CHIPID_0		0x1104
#define AEOLIA_REG_CHIPID_1		0x1108
#define AEOLIA_REG_CHIPREV		0x110c

/* Relative to BAR4 */
#define AEOLIA_RGN_UART_BASE		0x140000
#define AEOLIA_RGN_UART_SIZE		0x1000

#define AEOLIA_RGN_PCIE_BASE		0x1c8000
#define AEOLIA_RGN_PCIE_SIZE		0x1000

#define AEOLIA_RGN_ICC_BASE		0x184000
#define AEOLIA_RGN_ICC_SIZE		0x1000

#define AEOLIA_REG_BAR(x)		(AEOLIA_RGN_PCIE_BASE + (x))
#define AEOLIA_REG_BAR_MASK(func, bar)	AEOLIA_REG_BAR(((func) * 0x30) + \
						((bar) << 3))
#define AEOLIA_REG_BAR_ADDR(func, bar)	AEOLIA_REG_BAR(((func) * 0x30) + \
						((bar) << 3) + 0x4)

#define AEOLIA_REG_MSI(x)		(AEOLIA_RGN_PCIE_BASE + 0x400 + (x))
#define AEOLIA_REG_MSI_CONTROL		AEOLIA_REG_MSI(0x0)
#define AEOLIA_REG_MSI_MASK(func)	AEOLIA_REG_MSI(0x4c + ((func) << 2))
#define AEOLIA_REG_MSI_DATA_HI(func)	AEOLIA_REG_MSI(0x8c + ((func) << 2))
#define AEOLIA_REG_MSI_ADDR(func)	AEOLIA_REG_MSI(0xac + ((func) << 2))
/* This register has non-uniform structure per function, dealt with in code */
#define AEOLIA_REG_MSI_DATA_LO(off)	AEOLIA_REG_MSI(0x100 + (off))

/* Not sure what the two individual bits do */
#define AEOLIA_REG_MSI_CONTROL_ENABLE	0x05

/* Enable for the entire function, 4 is special */
#define AEOLIA_REG_MSI_MASK_FUNC		0x01000000
#define AEOLIA_REG_MSI_MASK_FUNC4	0x80000000

#define AEOLIA_REG_ICC(x)		(AEOLIA_RGN_ICC_BASE + (x))
#define AEOLIA_REG_ICC_DOORBELL		AEOLIA_REG_ICC(0x804)
#define AEOLIA_REG_ICC_STATUS		AEOLIA_REG_ICC(0x814)
#define AEOLIA_REG_ICC_IRQ_MASK		AEOLIA_REG_ICC(0x824)

/* Apply to both DOORBELL and STATUS */
#define AEOLIA_ICC_SEND			0x01
#define AEOLIA_ICC_ACK			0x02

/* Relative to func6 BAR5 */
#define AEOLIA_SPM_ICC_BASE		0x2c000
#define AEOLIA_SPM_ICC_SIZE		0x1000

/* Boot params passed from southbridge */
#define AEOLIA_SPM_BP_BASE		0x2f000
#define AEOLIA_SPM_BP_SIZE		0x20

#define AEOLIA_SPM_ICC_REQUEST		0x0
#define AEOLIA_SPM_ICC_REPLY		0x800


enum baikal_func_id {
	BAIKAL_FUNC_ID_ACPI = 0,
	BAIKAL_FUNC_ID_GBE,
	BAIKAL_FUNC_ID_AHCI,
	BAIKAL_FUNC_ID_SDHCI,
	BAIKAL_FUNC_ID_PCIE,
	BAIKAL_FUNC_ID_DMAC,
	BAIKAL_FUNC_ID_MEM,
	BAIKAL_FUNC_ID_XHCI,

	BAIKAL_NUM_FUNCS
};

enum baikal_subfuncs_per_func {
	SUBFUNCS_PER_FUNC4 = 31,
	SUBFUNCS_PER_FUNC2 = 1,
	SUBFUNCS_PER_FUNC7 = 3,
	SUBFUNCS_PER_FUNC0 = 2,
	SUBFUNCS_PER_FUNC1 = 1,
	SUBFUNCS_PER_FUNC3 = 1,
	SUBFUNCS_PER_FUNC5 = 2,
	SUBFUNCS_PER_FUNC6 = 3,
};

enum baikal_subfunc {
	BAIKAL_SUBFUNC_GLUE	= 0,
	BAIKAL_SUBFUNC_ICC	= 3,
	BAIKAL_SUBFUNC_HPET	= 22,
	BAIKAL_SUBFUNC_SFLASH	= 19,
	BAIKAL_SUBFUNC_RTC	= 21,
	BAIKAL_SUBFUNC_UART0	= 26,
	BAIKAL_SUBFUNC_UART1	= 27,

	BAIKAL_SUBFUNC_USB0	= 0, BAIKAL_SUBFUNC_USB2 = 2,
	BAIKAL_SUBFUNC_ACPI= 1,
	BAIKAL_SUBFUNC_SPM = 1,
	BAIKAL_SUBFUNC_DMAC1	= 0,
	BAIKAL_SUBFUNC_DMAC2	= 1,
	BAIKAL_NUM_SUBFUNCS	= 32
};

#define BAIKAL_NR_UARTS 1

#define BAIKAL_RGN_CHIPID_BASE		0x4000
#define BAIKAL_RGN_CHIPID_SIZE		0x9000

#define BAIKAL_REG_CHIPID_0		0xC020
#define BAIKAL_REG_CHIPID_1		0xC024
#define BAIKAL_REG_CHIPREV		0x4084

#define BAIKAL_HPET_BASE         0x109000
#define BAIKAL_HPET_SIZE         0x400

#define BAIKAL_RGN_UART_BASE		0x10E000
#define BAIKAL_RGN_UART_SIZE		0x1000
#define BAIKAL_RGN_ICC_BASE		(0x108000 - 0x800)
#define BAIKAL_RGN_ICC_SIZE		0x1000

#define BAIKAL_ACK_WRITE 		0x110084
#define BAIKAL_ACK_READ  		0x110088

#define BAIKAL_REG_ICC(x)		(BAIKAL_RGN_ICC_BASE + (x))
#define BAIKAL_REG_ICC_DOORBELL		BAIKAL_REG_ICC(0x804)
#define BAIKAL_REG_ICC_STATUS		BAIKAL_REG_ICC(0x814)
#define BAIKAL_REG_ICC_IRQ_MASK		BAIKAL_REG_ICC(0x824)

#define BAIKAL_ICC_SEND			0x01
#define BAIKAL_ICC_ACK			0x02

#define BAIKAL_USB_BASE			0x180000

#define BAIKAL_SPM_ICC_BASE		0x2c000
#define BAIKAL_SPM_ICC_SIZE		0x1000

#define BAIKAL_SPM_BP_BASE		0x2f000
#define BAIKAL_SPM_BP_SIZE		0x20

#define BAIKAL_SPM_ICC_REQUEST		0x0
#define BAIKAL_SPM_ICC_REPLY		0x800


#define CHAR_BIT 8
static inline u32 rol (u32 n, unsigned int c) {
	const unsigned int mask = (CHAR_BIT*sizeof(n) - 1);

	c &= mask;
	return (n<<c) | (n>>( (-c)&mask ));
}

static inline u32 ror (u32 n, unsigned int c) {
	const unsigned int mask = (CHAR_BIT*sizeof(n) - 1);

	c &= mask;
	return (n>>c) | (n<<( (-c)&mask ));
}

static inline void cpu_stop(void)
{
    for (;;)
        asm volatile("cli; hlt;" : : : "memory");
}

static inline void stop_hpet_timers(struct ps4sb_dev *sc) {
		*(volatile u64 *)(sc->bar2 + BAIKAL_HPET_BASE + 0x10) &= ~(1UL << 0);
		u64 NUM_TIM_CAP;
		NUM_TIM_CAP = *(volatile u64 *)(sc->bar2 + BAIKAL_HPET_BASE) & 0x1F00;
		u64 N;
		for (N = 0; N <= NUM_TIM_CAP; N++) {
			*(volatile u64 *)(sc->bar2 + BAIKAL_HPET_BASE + (0x20*N) + 0x100) &= ~(1UL << 2);
		}
		cpu_stop();
}

static inline int pci_pm_stop(struct pci_dev *dev)
{
	u16 csr;

	if (!dev->pm_cap)
		return -ENOTTY;

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &csr);

	csr &= ~PCI_PM_CTRL_STATE_MASK;
	csr |= PCI_D3hot;
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr);

	return 0;
}

static inline void pci_pm_stop_all(struct pci_dev *dev)
{
	struct pci_dev *sc_dev;
	unsigned int func;
	for (func = 0; func < 8; ++func) {
		sc_dev = pci_get_slot(pci_find_bus(pci_domain_nr(dev->bus), 0), PCI_DEVFN(20, func));
		pci_pm_stop(sc_dev);
	}
	cpu_stop();
}

int baikal_is_compatible_device(struct pci_dev *dev);
int baikal_sata_phy_init(struct pci_dev *pdev, void __iomem *ahci_mmio);

#endif
