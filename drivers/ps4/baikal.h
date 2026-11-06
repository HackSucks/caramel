#ifndef _BAIKAL_H
#define _BAIKAL_H

#include <linux/io.h>
#include <linux/pci.h>
#include <linux/i2c.h>
#include "ps4sb.h"

#define bpcie_dev		ps4sb_dev
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

enum bpcie_subfuncs_per_func {
	SUBFUNCS_PER_FUNC4 = 31,
	SUBFUNCS_PER_FUNC2 = 1,
	SUBFUNCS_PER_FUNC7 = 3,
	SUBFUNCS_PER_FUNC0 = 2,
	SUBFUNCS_PER_FUNC1 = 1,
	SUBFUNCS_PER_FUNC3 = 1,
	SUBFUNCS_PER_FUNC5 = 2,
	SUBFUNCS_PER_FUNC6 = 3,
};

enum bpcie_subfunc {
	BPCIE_SUBFUNC_GLUE	= 0,
	BPCIE_SUBFUNC_ICC	= 3,
	BPCIE_SUBFUNC_HPET	= 22,
	BPCIE_SUBFUNC_SFLASH	= 19,
	BPCIE_SUBFUNC_RTC	= 21,
	BPCIE_SUBFUNC_UART0	= 26,
	BPCIE_SUBFUNC_UART1	= 27,

	BPCIE_SUBFUNC_USB0	= 0, BPCIE_SUBFUNC_USB2 = 2,
	BPCIE_SUBFUNC_ACPI= 1,
	BPCIE_SUBFUNC_SPM = 1,
	BPCIE_SUBFUNC_DMAC1	= 0,
	BPCIE_SUBFUNC_DMAC2	= 1,
	BPCIE_NUM_SUBFUNCS	= 32
};

#define BPCIE_NR_UARTS 1

#define BPCIE_RGN_CHIPID_BASE		0x4000
#define BPCIE_RGN_CHIPID_SIZE		0x9000

#define BPCIE_REG_CHIPID_0		0xC020
#define BPCIE_REG_CHIPID_1		0xC024
#define BPCIE_REG_CHIPREV		0x4084

#define BPCIE_HPET_BASE         0x109000
#define BPCIE_HPET_SIZE         0x400

#define BPCIE_RGN_UART_BASE		0x10E000
#define BPCIE_RGN_UART_SIZE		0x1000
#define BPCIE_RGN_ICC_BASE		(0x108000 - 0x800)
#define BPCIE_RGN_ICC_SIZE		0x1000

#define BPCIE_ACK_WRITE 		0x110084
#define BPCIE_ACK_READ  		0x110088

#define BPCIE_REG_ICC(x)		(BPCIE_RGN_ICC_BASE + (x))
#define BPCIE_REG_ICC_DOORBELL		BPCIE_REG_ICC(0x804)
#define BPCIE_REG_ICC_STATUS		BPCIE_REG_ICC(0x814)
#define BPCIE_REG_ICC_IRQ_MASK		BPCIE_REG_ICC(0x824)

#define BPCIE_ICC_SEND			0x01
#define BPCIE_ICC_ACK			0x02

#define BPCIE_USB_BASE			0x180000

#define BPCIE_SPM_ICC_BASE		0x2c000
#define BPCIE_SPM_ICC_SIZE		0x1000

#define BPCIE_SPM_BP_BASE		0x2f000
#define BPCIE_SPM_BP_SIZE		0x20

#define BPCIE_SPM_ICC_REQUEST		0x0
#define BPCIE_SPM_ICC_REPLY		0x800

static inline int bpcie_irqnum(struct bpcie_dev *sc, int index)
{
	
	if (sc->nvec > 1) {
		return sc->pdev->irq + index;
	} else {
		return sc->pdev->irq;
	}
}


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

static inline void stop_hpet_timers(struct bpcie_dev *sc) {
		*(volatile u64 *)(sc->bar2 + BPCIE_HPET_BASE + 0x10) &= ~(1UL << 0);
		u64 NUM_TIM_CAP;
		NUM_TIM_CAP = *(volatile u64 *)(sc->bar2 + BPCIE_HPET_BASE) & 0x1F00;
		u64 N;
		for (N = 0; N <= NUM_TIM_CAP; N++) {
			*(volatile u64 *)(sc->bar2 + BPCIE_HPET_BASE + (0x20*N) + 0x100) &= ~(1UL << 2);
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

int bpcie_is_compatible_device(struct pci_dev *dev);
u32 glue_read32(struct bpcie_dev *sc, u32 offset);
void glue_write32(struct bpcie_dev *sc, u32 offset, u32 value);
int bpcie_baikal_sata_phy_init(struct pci_dev *pdev, void __iomem *ahci_mmio);

#endif
