/*
 * ps4.h: Sony PS4 platform setup code
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#ifndef _ASM_X86_PS4_H
#define _ASM_X86_PS4_H

#include <linux/irqdomain.h>
#include <linux/pci.h>

#ifdef CONFIG_X86_PS4

#define PS4_DEFAULT_TSC_FREQ 1594000000
#define PS4_PRO_TSC_FREQ 2100000000

#define EMC_TIMER_BASE 0xd0281000
#define EMC_TIMER_VALUE 0x28
#define EMC_TIMER_HZ 32768
#define EMC_TIMER_BAIKAL_OFFSET 0x9000
#define EMC_TIMER_BAIKAL_VALUE 0x18

extern unsigned long ps4_calibrate_tsc(void);

/*
 * The PS4 Aeolia southbridge device is a composite device containing some
 * standard-ish, some not-so-standard, and some completely custom functions,
 * all using special MSI handling. This function does the equivalent of
 * pci_enable_msi_range and friends, for those devices. Only works after the
 * Aeolia MSR routing function device (function 4) has been probed.
 * Returns 1 or count, depending on IRQ allocation constraints, or negative on
 * error. Assigned IRQ(s) start at dev->irq.
 */
extern int aeolia_assign_irqs(struct pci_dev *dev, int nvec);
extern void aeolia_free_irqs(unsigned int virq, unsigned int nr_irqs);

typedef void (*ps4sb_icc_event_fn)(void *ctx, u16 minor, const u8 *data,
				   u16 length);
extern void ps4_samu_lock(void);
extern void ps4_samu_unlock(void);
extern void *ps4_samu_data(u64 *va, size_t *size);
extern int ps4_samu_map(struct page **pages, unsigned int count, u64 *va);
extern void ps4_samu_unmap(void);
extern int ps4_samu_rng(void *buf, size_t len);
extern int ps4_samu_ccp(void *op);
extern bool ps4sb_initialized;
extern int ps4sb_status(void);
extern int ps4sb_assign_irqs(struct pci_dev *dev, int nvec);
extern void ps4sb_free_irqs(unsigned int virq, unsigned int nr_irqs);
extern bool ps4_is_baikal(void);
extern int baikal_assign_irqs(struct pci_dev *dev, int nvec);
extern void baikal_free_irqs(unsigned int virq, unsigned int nr_irqs);
extern void ps4sb_icc_reboot(void);
extern int ps4sb_icc_cmd(u8 major, u16 minor, const void *data,
			 u16 length, void *reply, u16 reply_length);
extern int ps4sb_icc_register_event_handler(u8 major, ps4sb_icc_event_fn fn,
					    void *ctx);
extern void ps4sb_icc_unregister_event_handler(u8 major);


#else

typedef void (*ps4sb_icc_event_fn)(void *ctx, u16 minor, const u8 *data,
				   u16 length);
static inline int ps4sb_assign_irqs(struct pci_dev *dev, int nvec)
{
	return -ENODEV;
}
static inline void ps4sb_free_irqs(unsigned int virq, unsigned int nr_irqs)
{
}
static inline int ps4sb_status(void)
{
	return -ENODEV;
}
static inline bool ps4_is_baikal(void)
{
	return false;
}
static inline int ps4sb_icc_cmd(u8 major, u16 minor, const void *data,
				u16 length, void *reply, u16 reply_length)
{
	return -ENODEV;
}
static inline int ps4sb_icc_register_event_handler(u8 major,
						   ps4sb_icc_event_fn fn,
						   void *ctx)
{
	return -ENODEV;
}
static inline void ps4sb_icc_unregister_event_handler(u8 major)
{
}

#endif
#endif
