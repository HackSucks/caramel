/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _MTS_H
#define _MTS_H

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct pci_dev;

#define MTS_REG_SMI		0x000
#define MTS_REG_LINK		0x004
#define MTS_REG_CLK_CTRL	0x008
#define MTS_REG_CTRL		0x00c
#define MTS_REG_MODE		0x010
#define MTS_REG_MAC_HI		0x014
#define MTS_REG_MAC_LO		0x018
#define MTS_REG_MC_HASH		0x01c
#define MTS_REG_RXBUF		0x030
#define MTS_REG_TX_DMA_CTRL	0x034
#define MTS_REG_RX_DMA_CTRL	0x038
#define MTS_REG_TX_RING_CUR	0x03c
#define MTS_REG_RX_RING_CUR	0x040
#define MTS_REG_TX_RING_BASE	0x044
#define MTS_REG_RX_RING_BASE	0x048
#define MTS_REG_ISR		0x050
#define MTS_REG_IMR		0x054
#define MTS_REG_SERDES		0x074
#define MTS_REG_RX_PCODE	0x078
#define MTS_REG_CLK_FREQ	0x07c
#define MTS_REG_RESET		0x0ac
#define MTS_REG_MIB_RX_DROP	0x110
#define MTS_REG_MIB_RX_CRC	0x114
#define MTS_REG_RX_EN		0x1d4
#define MTS_REG_WOL_CTRL	0x200
#define MTS_REG_COAL		0x204

#define MTS_GLUE_FUSE_BASE	0xc000
#define MTS_GLUE_FUSE_SIZE	0x100

#define MTS_CLK_ENABLE		0x07597c00
#define MTS_CLK_FREQ_HZ		25000000
#define MTS_SERDES_VAL		0x2277
#define MTS_RXBUF_VAL		0x00010100
#define MTS_RESET_VAL		9

#define MTS_LINK_UP		BIT(0)
#define MTS_LINK_AN_EN		BIT(8)
#define MTS_LINK_SPEED		GENMASK(10, 9)
#define MTS_LINK_SPEED_10	0
#define MTS_LINK_SPEED_100	1
#define MTS_LINK_SPEED_1000	2
#define MTS_LINK_FULL_DPX	BIT(11)
#define MTS_LINK_KEEP		0x7fffcfff

#define MTS_ISR_LINK		BIT(2)
#define MTS_ISR_RX_DONE		BIT(6)
#define MTS_ISR_TX_DONE		BIT(7)
#define MTS_ISR_RX_EMPTY	(BIT(1) | BIT(5))
#define MTS_ISR_TIMER		BIT(18)
#define MTS_ISR_WORK		(MTS_ISR_RX_DONE | MTS_ISR_RX_EMPTY | MTS_ISR_TX_DONE)
#define MTS_ISR_ERRORS		0x7be600
#define MTS_ISR_LSO_RECOVER	0x500000

#define MTS_ERR_LSO_FIFO	BIT(21)
#define MTS_ERR_LSO_PROTO	BIT(19)
#define MTS_ERR_RX_AXI		BIT(17)
#define MTS_ERR_IP_CKSUM	BIT(15)
#define MTS_ERR_TCP_CKSUM	BIT(14)
#define MTS_ERR_UDP_CKSUM	BIT(13)
#define MTS_ERR_RX_PCODE	BIT(10)

#define MTS_IMR_NORMAL		0x1018
#define MTS_IMR_ALL		0x7fffff
#define MTS_IMR_TIMER		(MTS_IMR_ALL & ~MTS_ISR_TIMER)

#define MTS_COAL_EN		BIT(28)
#define MTS_COAL_TICK		0x1388

#define MTS_MODE_PROMISC	BIT(4)

#define MTS_MC_HASH_CLEAR	BIT(31)
#define MTS_MC_HASH_CLEARED	BIT(17)
#define MTS_MC_HASH_BUSY	BIT(14)
#define MTS_MC_HASH_SET		0x7000
#define MTS_MC_HASH_BITS	256

#define MTS_DMA_START		BIT(0)
#define MTS_DMA_STOP		BIT(1)
#define MTS_DMA_KICK		BIT(2)

#define MTS_ST_LSO_RECOVER	0
#define MTS_ST_RX_RESTART	1

struct mts_desc {
	__le32	flags;
	__le32	buf_addr;
	__le32	word2;
	__le32	word3;
};

#define MTS_DESC_DONE		BIT(31)
#define MTS_DESC_WRAP		BIT(30)
#define MTS_DESC_SOP		BIT(29)
#define MTS_DESC_EOP		BIT(28)
#define MTS_DESC_LEN_MASK	0xffff
#define MTS_DESC_RX_LEN		0x600
#define MTS_DESC_RX_LEN_MASK	0x7ff
#define MTS_DESC_W2_FREE	0xffff0000
#define MTS_DESC_W2_TX		0x4

#define MTS_RING_ALIGN		0x4000
#define MTS_TX_RING_SIZE	256
#define MTS_TX_RING_BYTES	MTS_RING_ALIGN
#define MTS_RX_RING_SIZE	1024
#define MTS_RX_RING_BYTES	(MTS_RX_RING_SIZE * sizeof(struct mts_desc))
#define MTS_TX_MAX_LEN		1536
#define MTS_TX_SCRATCH_SIZE	0xa0000
#define MTS_RX_POOL_SIZE	(MTS_RX_RING_SIZE * MTS_DESC_RX_LEN)

struct mts_priv {
	struct net_device	*netdev;
	struct pci_dev		*pdev;
	void __iomem		*base;
	void __iomem		*fuse;
	struct pci_dev		*glue_pdev;

	struct mts_desc		*tx_ring;
	dma_addr_t		tx_ring_dma;
	void			*tx_scratch;
	dma_addr_t		tx_scratch_dma;
	unsigned int		tx_scratch_off;
	unsigned int		tx_head;
	unsigned int		tx_tail;

	struct mts_desc		*rx_ring;
	dma_addr_t		rx_ring_dma;
	void			*rx_pool;
	dma_addr_t		rx_pool_dma;
	unsigned int		rx_head;

	struct napi_struct	napi;
	struct delayed_work	link_poll;
	spinlock_t		lock;
	unsigned long		state;
	int			irq;
	bool			link_up;
	bool			coal_on;
	u32			irq_count;
	u32			irq_seen;
};

static inline u32 mts_rd(struct mts_priv *p, u32 reg)
{
	return readl(p->base + reg);
}

static inline void mts_wr(struct mts_priv *p, u32 reg, u32 val)
{
	writel(val, p->base + reg);
}

static inline void mts_set(struct mts_priv *p, u32 reg, u32 bits)
{
	mts_wr(p, reg, mts_rd(p, reg) | bits);
}

static inline void mts_clr(struct mts_priv *p, u32 reg, u32 bits)
{
	mts_wr(p, reg, mts_rd(p, reg) & ~bits);
}

void mts_phy_init(struct mts_priv *p);
void mts_phy_autoneg(struct mts_priv *p);
void mts_phy_resolve(struct mts_priv *p, int *speed, bool *full);

#endif
