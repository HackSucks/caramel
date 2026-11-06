// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "ps4-sflash: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include "ps4-sflash.h"

struct ps4_sflash_dev {
	struct pci_dev *pdev;
	void __iomem *bar4;
	void __iomem *sflash_regs;
	void __iomem *clk_regs;
	struct miscdevice misc;
	struct mutex lock;
	u8 *dma_vaddr;
	dma_addr_t dma_paddr;
	u8 *dec_buf;
};

static struct ps4_sflash_dev *g_ps4_sflash;

static const u16 ps4_sflash_pci_ids[] = {
	0x90a1,
	0x90b8,
	0x90c4,
	0x90d4,
};

static inline u32 sflash_read32(struct ps4_sflash_dev *dev, u32 reg)
{
	return ioread32(dev->sflash_regs + reg);
}

static inline void sflash_write32(struct ps4_sflash_dev *dev, u32 reg, u32 val)
{
	iowrite32(val, dev->sflash_regs + reg);
}

static inline u32 sflash_clk_read32(struct ps4_sflash_dev *dev, u32 reg)
{
	return ioread32(dev->clk_regs + reg);
}

static inline void sflash_clk_write32(struct ps4_sflash_dev *dev, u32 reg, u32 val)
{
	iowrite32(val, dev->clk_regs + reg);
}

static int ps4_sflash_clk_enable(struct ps4_sflash_dev *dev)
{
	int timeout = 1000;

	sflash_clk_write32(dev, 0x4, 1);
	while (timeout--) {
		if ((sflash_clk_read32(dev, 0x0) & 7) == 4)
			return 0;
		usleep_range(50, 100);
	}
	return -ETIMEDOUT;
}

static int ps4_sflash_wait_idle(struct ps4_sflash_dev *dev)
{
	int timeout = 10000;

	while (timeout--) {
		if ((sflash_read32(dev, PS4_SFLASH_REG_STATUS) & 1) == 0)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int ps4_sflash_wait_dma(struct ps4_sflash_dev *dev)
{
	int timeout = 10000;

	while (timeout--) {
		if ((sflash_read32(dev, PS4_SFLASH_REG_CMD) & 0x80000000) == 0)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static u32 ps4_sflash_read_jedec(struct ps4_sflash_dev *dev)
{
	ps4_sflash_clk_enable(dev);
	sflash_write32(dev, PS4_SFLASH_REG_CMD, 0x8010009f);
	ps4_sflash_wait_idle(dev);
	return sflash_read32(dev, PS4_SFLASH_REG_RDID);
}

static u32 ps4_sflash_read_status(struct ps4_sflash_dev *dev)
{
	ps4_sflash_clk_enable(dev);
	sflash_write32(dev, PS4_SFLASH_REG_CMD, 0x80000005);
	ps4_sflash_wait_idle(dev);
	return (sflash_read32(dev, PS4_SFLASH_REG_STATUS) >> 16) & 0xff;
}

static void ps4_sflash_unpack_chunk(const u8 *raw, u8 *dec, size_t raw_len)
{
	size_t i;

	for (i = 0; i < raw_len; i += 4) {
		u8 b0 = raw[i];
		u8 b1 = raw[i + 1];
		u8 b2 = raw[i + 2];
		u8 b3 = raw[i + 3];

		dec[i / 4] = ((b0 & 0x20) << 2) |
			     ((b0 & 0x02) << 5) |
			     ((b1 & 0x20) << 0) |
			     ((b1 & 0x02) << 3) |
			     ((b2 & 0x20) >> 2) |
			     ((b2 & 0x02) << 1) |
			     ((b3 & 0x20) >> 4) |
			     ((b3 & 0x02) >> 1);
	}
}

static ssize_t ps4_sflash_read_block(struct ps4_sflash_dev *dev, u32 flash_addr,
				     u8 *out_buf, size_t flash_len)
{
	size_t dma_len = flash_len * 4;
	u32 ctrl_addr = (flash_addr - 1) & 0xffffffff;
	int ret;

	if (dma_len > PS4_SFLASH_CHUNK_DMA)
		return -EINVAL;

	ret = ps4_sflash_clk_enable(dev);
	if (ret)
		return ret;

	sflash_write32(dev, PS4_SFLASH_REG_TIMING, 0x007d0000);
	sflash_write32(dev, PS4_SFLASH_REG_OPCODES, 0x9f050300);
	sflash_write32(dev, PS4_SFLASH_REG_CS,
		       (sflash_read32(dev, PS4_SFLASH_REG_CS) & 0xffffff00) | 1);

	sflash_write32(dev, PS4_SFLASH_REG_STATUS_CLR, 0);
	sflash_write32(dev, PS4_SFLASH_REG_OFFSET, ctrl_addr);
	sflash_write32(dev, PS4_SFLASH_REG_DMA_ADDR, (u32)dev->dma_paddr);
	sflash_write32(dev, PS4_SFLASH_REG_DMA_LEN, 0x80000000 | (u32)dma_len);
	sflash_write32(dev, PS4_SFLASH_REG_CMD, 0x82100103);

	ret = ps4_sflash_wait_dma(dev);
	if (ret)
		return ret;

	ps4_sflash_unpack_chunk(dev->dma_vaddr, out_buf, dma_len);
	return flash_len;
}

static ssize_t ps4_sflash_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct ps4_sflash_dev *dev = g_ps4_sflash;
	size_t total_read = 0;
	ssize_t ret = 0;

	if (!dev)
		return -ENODEV;

	if (*ppos >= PS4_SFLASH_TOTAL_SIZE)
		return 0;

	if (*ppos + count > PS4_SFLASH_TOTAL_SIZE)
		count = PS4_SFLASH_TOTAL_SIZE - *ppos;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	while (count > 0) {
		u32 chunk_flash_off = (u32)(*ppos & ~(PS4_SFLASH_CHUNK_FLASH - 1));
		u32 offset_in_chunk = (u32)(*ppos & (PS4_SFLASH_CHUNK_FLASH - 1));
		size_t to_copy = PS4_SFLASH_CHUNK_FLASH - offset_in_chunk;

		if (to_copy > count)
			to_copy = count;

		ret = ps4_sflash_read_block(dev, chunk_flash_off, dev->dec_buf,
					    PS4_SFLASH_CHUNK_FLASH);
		if (ret < 0)
			break;

		if (copy_to_user(buf + total_read, dev->dec_buf + offset_in_chunk, to_copy)) {
			ret = -EFAULT;
			break;
		}

		*ppos += to_copy;
		total_read += to_copy;
		count -= to_copy;
	}

	mutex_unlock(&dev->lock);
	return total_read ? (ssize_t)total_read : ret;
}

static loff_t ps4_sflash_llseek(struct file *file, loff_t offset, int whence)
{
	return fixed_size_llseek(file, offset, whence, PS4_SFLASH_TOTAL_SIZE);
}

static long ps4_sflash_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ps4_sflash_dev *dev = g_ps4_sflash;
	u32 val;
	int ret = 0;

	if (!dev)
		return -ENODEV;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	switch (cmd) {
	case PS4_SFLASH_IOC_GET_SIZE:
		val = PS4_SFLASH_TOTAL_SIZE;
		if (copy_to_user((void __user *)arg, &val, sizeof(val)))
			ret = -EFAULT;
		break;
	case PS4_SFLASH_IOC_GET_JEDEC:
		val = ps4_sflash_read_jedec(dev);
		if (copy_to_user((void __user *)arg, &val, sizeof(val)))
			ret = -EFAULT;
		break;
	case PS4_SFLASH_IOC_GET_STATUS:
		val = ps4_sflash_read_status(dev);
		if (copy_to_user((void __user *)arg, &val, sizeof(val)))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&dev->lock);
	return ret;
}

static const struct file_operations ps4_sflash_fops = {
	.owner          = THIS_MODULE,
	.read           = ps4_sflash_read,
	.llseek         = ps4_sflash_llseek,
	.unlocked_ioctl = ps4_sflash_ioctl,
};

static int __init ps4_sflash_init(void)
{
	struct ps4_sflash_dev *dev;
	struct pci_dev *pdev = NULL;
	int i, ret;
	u32 jedec;

	for (i = 0; i < ARRAY_SIZE(ps4_sflash_pci_ids); i++) {
		pdev = pci_get_device(PCI_VENDOR_ID_SONY, ps4_sflash_pci_ids[i], NULL);
		if (pdev)
			break;
	}

	if (!pdev) {
		pdev = pci_get_device(0x104d, 0x90a1, NULL);
		if (!pdev)
			return -ENODEV;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		pci_dev_put(pdev);
		return -ENOMEM;
	}

	dev->pdev = pdev;
	mutex_init(&dev->lock);

	dev->bar4 = pci_iomap(pdev, 4, 0x200000);
	if (!dev->bar4) {
		pr_err("failed to map Aeolia Southbridge BAR4\n");
		ret = -ENOMEM;
		goto err_free;
	}

	dev->sflash_regs = dev->bar4 + 0xc2000;
	dev->clk_regs = dev->bar4 + 0xc3000;

	dev->dec_buf = kmalloc(PS4_SFLASH_CHUNK_FLASH, GFP_KERNEL);
	if (!dev->dec_buf) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	dev->dma_vaddr = dma_alloc_coherent(&pdev->dev, PS4_SFLASH_CHUNK_DMA,
					    &dev->dma_paddr, GFP_KERNEL);
	if (!dev->dma_vaddr) {
		pr_err("failed to allocate DMA bounce buffer\n");
		ret = -ENOMEM;
		goto err_free_dec;
	}

	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name  = "ps4sflash";
	dev->misc.fops  = &ps4_sflash_fops;
	ret = misc_register(&dev->misc);
	if (ret) {
		pr_err("failed to register misc device\n");
		goto err_free_dma;
	}

	g_ps4_sflash = dev;
	jedec = ps4_sflash_read_jedec(dev);

	pr_info("Aeolia Southbridge SPI NOR flash driver initialized\n");
	pr_info("JEDEC ID: 0x%08x, Size: 32MB\n", jedec);
	pr_info("Userspace device registered (/dev/ps4sflash)\n");
	return 0;

err_free_dma:
	dma_free_coherent(&pdev->dev, PS4_SFLASH_CHUNK_DMA, dev->dma_vaddr, dev->dma_paddr);
err_free_dec:
	kfree(dev->dec_buf);
err_unmap:
	pci_iounmap(pdev, dev->bar4);
err_free:
	pci_dev_put(pdev);
	kfree(dev);
	return ret;
}

static void __exit ps4_sflash_exit(void)
{
	struct ps4_sflash_dev *dev = g_ps4_sflash;

	if (!dev)
		return;

	misc_deregister(&dev->misc);
	dma_free_coherent(&dev->pdev->dev, PS4_SFLASH_CHUNK_DMA, dev->dma_vaddr, dev->dma_paddr);
	kfree(dev->dec_buf);
	pci_iounmap(dev->pdev, dev->bar4);
	pci_dev_put(dev->pdev);
	kfree(dev);
	g_ps4_sflash = NULL;
}

module_init(ps4_sflash_init);
module_exit(ps4_sflash_exit);

MODULE_AUTHOR("Armandas Kvietkus <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("PS4 Aeolia Southbridge Hardware SPI NOR Flash Driver");
MODULE_LICENSE("GPL");
