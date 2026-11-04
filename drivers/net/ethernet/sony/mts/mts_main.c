// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/bitrev.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <asm/ps4.h>

#include "mts.h"

#define MTS_GLUE_FUNC		4
#define MTS_GLUE_BAR		4
#define MTS_READRQ_SONY		2048

static void mts_write_mac(struct mts_priv *p, const u8 *addr)
{
	mts_wr(p, MTS_REG_MAC_HI, (addr[0] << 8) | addr[1]);
	mts_wr(p, MTS_REG_MAC_LO, (addr[2] << 24) | (addr[3] << 16) |
	       (addr[4] << 8) | addr[5]);
}

static void mts_read_mac(struct mts_priv *p, u8 *addr)
{
	u32 hi = mts_rd(p, MTS_REG_MAC_HI);
	u32 lo = mts_rd(p, MTS_REG_MAC_LO);

	addr[0] = hi >> 8;
	addr[1] = hi;
	addr[2] = lo >> 24;
	addr[3] = lo >> 16;
	addr[4] = lo >> 8;
	addr[5] = lo;
}

static int mts_glue_map(struct mts_priv *p)
{
	struct pci_dev *glue;
	resource_size_t start;

	glue = pci_get_slot(p->pdev->bus,
			    PCI_DEVFN(PCI_SLOT(p->pdev->devfn), MTS_GLUE_FUNC));
	if (!glue)
		return -ENODEV;

	if (glue->vendor != PCI_VENDOR_ID_SONY ||
	    glue->device != PCI_DEVICE_ID_SONY_BAIKAL_PCIE ||
	    !(pci_resource_flags(glue, MTS_GLUE_BAR) & IORESOURCE_MEM) ||
	    pci_resource_len(glue, MTS_GLUE_BAR) <
	    MTS_GLUE_FUSE_BASE + MTS_GLUE_FUSE_SIZE) {
		pci_dev_put(glue);
		return -ENODEV;
	}

	start = pci_resource_start(glue, MTS_GLUE_BAR) + MTS_GLUE_FUSE_BASE;
	p->fuse = ioremap(start, MTS_GLUE_FUSE_SIZE);
	if (!p->fuse) {
		pci_dev_put(glue);
		return -ENOMEM;
	}

	p->glue_pdev = glue;
	return 0;
}

static void mts_glue_unmap(struct mts_priv *p)
{
	if (p->fuse)
		iounmap(p->fuse);
	p->fuse = NULL;
	pci_dev_put(p->glue_pdev);
	p->glue_pdev = NULL;
}

static void mts_init_tx_ring(struct mts_priv *p)
{
	int i;

	for (i = 0; i < MTS_TX_RING_SIZE; i++) {
		p->tx_ring[i].flags = cpu_to_le32(MTS_DESC_DONE |
			(i == MTS_TX_RING_SIZE - 1 ? MTS_DESC_WRAP : 0));
		p->tx_ring[i].buf_addr = 0;
		p->tx_ring[i].word2 = cpu_to_le32(MTS_DESC_W2_FREE);
		p->tx_ring[i].word3 = 0;
	}
	p->tx_head = 0;
	p->tx_tail = 0;
	p->tx_scratch_off = 0;
}

static void mts_rx_give(struct mts_priv *p, unsigned int i)
{
	p->rx_ring[i].word2 = 0;
	p->rx_ring[i].word3 = 0;
	dma_wmb();
	p->rx_ring[i].flags = cpu_to_le32(MTS_DESC_RX_LEN |
		(i == MTS_RX_RING_SIZE - 1 ? MTS_DESC_WRAP : 0));
}

static void mts_init_rx_ring(struct mts_priv *p)
{
	int i;

	for (i = 0; i < MTS_RX_RING_SIZE; i++) {
		p->rx_ring[i].buf_addr = cpu_to_le32(p->rx_pool_dma +
						     i * MTS_DESC_RX_LEN);
		mts_rx_give(p, i);
	}
	p->rx_head = 0;
}

static void mts_start_dma(struct mts_priv *p)
{
	wmb();

	mts_wr(p, MTS_REG_TX_RING_BASE, p->tx_ring_dma);
	mts_wr(p, MTS_REG_TX_RING_CUR, p->tx_ring_dma);
	mts_wr(p, MTS_REG_RX_RING_BASE, p->rx_ring_dma);
	mts_wr(p, MTS_REG_RX_RING_CUR, p->rx_ring_dma);
	mts_set(p, MTS_REG_TX_DMA_CTRL, MTS_DMA_START);
	mts_set(p, MTS_REG_RX_DMA_CTRL, MTS_DMA_START);
	mts_wr(p, MTS_REG_ISR, mts_rd(p, MTS_REG_ISR));
	mts_wr(p, MTS_REG_IMR, MTS_IMR_NORMAL);
}

static void mts_stop_dma(struct mts_priv *p)
{
	u32 val;

	mts_wr(p, MTS_REG_IMR, MTS_IMR_ALL);
	mts_wr(p, MTS_REG_COAL, 0);
	p->coal_on = false;

	mts_set(p, MTS_REG_TX_DMA_CTRL, MTS_DMA_STOP);
	if (readl_poll_timeout(p->base + MTS_REG_TX_DMA_CTRL, val,
			       !(val & MTS_DMA_STOP), 10, 1000000))
		netdev_warn(p->netdev, "TX DMA stop timeout\n");

	mts_set(p, MTS_REG_RX_DMA_CTRL, MTS_DMA_STOP);
	if (readl_poll_timeout(p->base + MTS_REG_RX_DMA_CTRL, val,
			       !(val & MTS_DMA_STOP), 10, 1000000))
		netdev_warn(p->netdev, "RX DMA stop timeout\n");
}

static void mts_init_hw(struct mts_priv *p)
{
	mts_wr(p, MTS_REG_WOL_CTRL, 0);
	mts_wr(p, MTS_REG_ISR, mts_rd(p, MTS_REG_ISR));
	usleep_range(680, 1000);

	mts_phy_init(p);

	mts_write_mac(p, p->netdev->dev_addr);
	mts_clr(p, MTS_REG_CTRL, BIT(7));
	mts_wr(p, MTS_REG_SERDES, MTS_SERDES_VAL);
	mts_set(p, MTS_REG_CLK_CTRL, MTS_CLK_ENABLE);
	mts_wr(p, MTS_REG_RX_EN, 1);
	mts_wr(p, MTS_REG_MODE, (mts_rd(p, MTS_REG_MODE) & 0xffffff6e) | 0x81);
	mts_wr(p, MTS_REG_RXBUF, MTS_RXBUF_VAL);

	mts_wr(p, MTS_REG_COAL, 0);
	p->coal_on = false;
	mts_rd(p, MTS_REG_MIB_RX_DROP);
	mts_rd(p, MTS_REG_MIB_RX_CRC);
}

static int mts_alloc_rings(struct mts_priv *p)
{
	struct device *dev = &p->pdev->dev;

	p->tx_ring = dmam_alloc_coherent(dev, MTS_TX_RING_BYTES,
					 &p->tx_ring_dma, GFP_KERNEL);
	p->rx_ring = dmam_alloc_coherent(dev, MTS_RX_RING_BYTES,
					 &p->rx_ring_dma, GFP_KERNEL);
	p->tx_scratch = dmam_alloc_coherent(dev, MTS_TX_SCRATCH_SIZE,
					    &p->tx_scratch_dma, GFP_KERNEL);
	p->rx_pool = dmam_alloc_coherent(dev, MTS_RX_POOL_SIZE,
					 &p->rx_pool_dma, GFP_KERNEL);

	if (!p->tx_ring || !p->rx_ring || !p->tx_scratch || !p->rx_pool)
		return -ENOMEM;
	return 0;
}

static void mts_tx_complete(struct mts_priv *p)
{
	unsigned int tail = p->tx_tail;

	while (tail != p->tx_head) {
		struct mts_desc *d = &p->tx_ring[tail];
		u32 w2 = le32_to_cpu(d->word2);

		if (!(le32_to_cpu(d->flags) & MTS_DESC_DONE))
			break;
		if (w2 >= MTS_DESC_W2_FREE)
			break;

		d->word2 = cpu_to_le32(w2 | MTS_DESC_W2_FREE);
		tail = (tail + 1) % MTS_TX_RING_SIZE;
	}
	p->tx_tail = tail;

	if (netif_queue_stopped(p->netdev))
		netif_wake_queue(p->netdev);
}

static int mts_rx_poll(struct mts_priv *p, int budget)
{
	struct net_device *dev = p->netdev;
	unsigned int i = p->rx_head;
	int done = 0;

	while (done < budget &&
	       (le32_to_cpu(p->rx_ring[i].flags) & MTS_DESC_DONE)) {
		u32 len = le32_to_cpu(p->rx_ring[i].flags) & MTS_DESC_RX_LEN_MASK;
		const u8 *src = (const u8 *)p->rx_pool + i * MTS_DESC_RX_LEN;
		struct sk_buff *skb;

		dma_rmb();

		if (len < ETH_ZLEN || len > MTS_DESC_RX_LEN) {
			dev->stats.rx_length_errors++;
			dev->stats.rx_errors++;
		} else {
			skb = napi_alloc_skb(&p->napi, len);
			if (skb) {
				skb_put_data(skb, src, len);
				skb->protocol = eth_type_trans(skb, dev);
				napi_gro_receive(&p->napi, skb);
				dev->stats.rx_packets++;
				dev->stats.rx_bytes += len;
			} else {
				dev->stats.rx_dropped++;
			}
		}

		mts_rx_give(p, i);
		done++;
		i = (i + 1) % MTS_RX_RING_SIZE;
	}
	p->rx_head = i;

	if (done)
		mts_set(p, MTS_REG_RX_DMA_CTRL, MTS_DMA_KICK);

	return done;
}

static netdev_tx_t mts_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mts_priv *p = netdev_priv(dev);
	unsigned int len = max_t(unsigned int, skb->len, ETH_ZLEN);
	unsigned int head, off, size;
	unsigned long flags;

	if (len > MTS_TX_MAX_LEN) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&p->lock, flags);

	mts_tx_complete(p);

	head = p->tx_head;
	if ((head + 1) % MTS_TX_RING_SIZE == p->tx_tail) {
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&p->lock, flags);
		return NETDEV_TX_BUSY;
	}

	size = ALIGN(len, 128);
	off = p->tx_scratch_off;
	if (off + size > MTS_TX_SCRATCH_SIZE)
		off = 0;
	p->tx_scratch_off = (off + size) % MTS_TX_SCRATCH_SIZE;

	skb_copy_bits(skb, 0, (u8 *)p->tx_scratch + off, skb->len);
	if (len > skb->len)
		memset((u8 *)p->tx_scratch + off + skb->len, 0, len - skb->len);

	p->tx_ring[head].buf_addr = cpu_to_le32(p->tx_scratch_dma + off);
	p->tx_ring[head].word2 = cpu_to_le32(MTS_DESC_W2_TX);
	p->tx_ring[head].word3 = 0;
	dma_wmb();
	p->tx_ring[head].flags = cpu_to_le32((len & MTS_DESC_LEN_MASK) |
		MTS_DESC_SOP | MTS_DESC_EOP |
		(head == MTS_TX_RING_SIZE - 1 ? MTS_DESC_WRAP : 0));

	p->tx_head = (head + 1) % MTS_TX_RING_SIZE;

	mts_set(p, MTS_REG_TX_DMA_CTRL, MTS_DMA_KICK);

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;

	spin_unlock_irqrestore(&p->lock, flags);
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

static int mts_set_mac_address(struct net_device *dev, void *addr)
{
	struct mts_priv *p = netdev_priv(dev);
	struct sockaddr *sa = addr;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;

	eth_hw_addr_set(dev, sa->sa_data);
	mts_write_mac(p, dev->dev_addr);
	return 0;
}

static u32 mts_mc_hash(const u8 *addr)
{
	u8 rev[ETH_ALEN];
	u32 crc;
	int i;

	for (i = 0; i < ETH_ALEN; i++)
		rev[i] = bitrev8(addr[i]);

	crc = crc32_be(~0, rev, ETH_ALEN);
	return (crc & 7) + (crc >> 27) * 8;
}

static void mts_set_rx_mode(struct net_device *dev)
{
	struct mts_priv *p = netdev_priv(dev);
	struct netdev_hw_addr *ha;
	u32 val;
	int i;

	if (dev->flags & IFF_PROMISC) {
		mts_set(p, MTS_REG_MODE, MTS_MODE_PROMISC);
		return;
	}

	mts_clr(p, MTS_REG_MODE, MTS_MODE_PROMISC);

	if (dev->flags & IFF_ALLMULTI) {
		for (i = 0; i < MTS_MC_HASH_BITS; i++)
			mts_wr(p, MTS_REG_MC_HASH, MTS_MC_HASH_SET | i);
		return;
	}

	mts_wr(p, MTS_REG_MC_HASH, MTS_MC_HASH_CLEAR);
	if (readl_poll_timeout_atomic(p->base + MTS_REG_MC_HASH, val,
				      val & MTS_MC_HASH_CLEARED, 1, 10000))
		netdev_warn(dev, "multicast hash clear timeout\n");

	netdev_for_each_mc_addr(ha, dev) {
		mts_wr(p, MTS_REG_MC_HASH, MTS_MC_HASH_SET | mts_mc_hash(ha->addr));
		if (readl_poll_timeout_atomic(p->base + MTS_REG_MC_HASH, val,
					      !(val & MTS_MC_HASH_BUSY), 1, 10000))
			netdev_warn(dev, "multicast hash set timeout\n");
	}
}

static void mts_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	struct mts_priv *p = netdev_priv(dev);

	strscpy(info->driver, KBUILD_MODNAME, sizeof(info->driver));
	strscpy(info->bus_info, pci_name(p->pdev), sizeof(info->bus_info));
}

static const struct ethtool_ops mts_ethtool_ops = {
	.get_drvinfo	= mts_get_drvinfo,
	.get_link	= ethtool_op_get_link,
};

static void mts_link_change(struct mts_priv *p)
{
	u32 link = mts_rd(p, MTS_REG_LINK);
	bool up = link & MTS_LINK_UP;
	int speed;
	bool full;

	if (up == p->link_up)
		return;

	p->link_up = up;
	if (!up) {
		netif_carrier_off(p->netdev);
		netdev_info(p->netdev, "link down\n");
		return;
	}

	mts_phy_resolve(p, &speed, &full);
	link &= ~(MTS_LINK_SPEED | MTS_LINK_FULL_DPX);
	link |= FIELD_PREP(MTS_LINK_SPEED, speed == SPEED_1000 ? MTS_LINK_SPEED_1000 :
			   speed == SPEED_100 ? MTS_LINK_SPEED_100 : MTS_LINK_SPEED_10);
	if (full)
		link |= MTS_LINK_FULL_DPX;
	mts_wr(p, MTS_REG_LINK, link);

	netif_carrier_on(p->netdev);
	netdev_info(p->netdev, "link up %d Mbps %s duplex (%#x)\n", speed,
		    full ? "full" : "half", mts_rd(p, MTS_REG_LINK));
}

static void mts_link_poll_work(struct work_struct *work)
{
	struct mts_priv *p = container_of(to_delayed_work(work),
					  struct mts_priv, link_poll);

	mts_link_change(p);

	p->netdev->stats.rx_missed_errors += mts_rd(p, MTS_REG_MIB_RX_DROP);
	p->netdev->stats.rx_crc_errors += mts_rd(p, MTS_REG_MIB_RX_CRC);

	if (p->irq_count == p->irq_seen)
		napi_schedule(&p->napi);
	p->irq_seen = p->irq_count;

	schedule_delayed_work(&p->link_poll, HZ);
}

static void mts_tx_rebuild(struct mts_priv *p)
{
	mts_init_tx_ring(p);
	dma_wmb();
	mts_wr(p, MTS_REG_TX_RING_BASE, p->tx_ring_dma);
	mts_wr(p, MTS_REG_TX_RING_CUR, p->tx_ring_dma);
	mts_set(p, MTS_REG_TX_DMA_CTRL, MTS_DMA_START);
}

static void mts_handle_status(struct mts_priv *p, u32 status)
{
	p->irq_count++;

	if (status & MTS_ISR_LINK)
		mts_link_change(p);

	if (status & MTS_ISR_ERRORS) {
		net_err_ratelimited("%s: hardware error %#x\n",
				    netdev_name(p->netdev),
				    status & MTS_ISR_ERRORS);
		p->netdev->stats.rx_errors++;
	}

	if (status & MTS_ISR_LSO_RECOVER)
		set_bit(MTS_ST_LSO_RECOVER, &p->state);
	if (status & MTS_ISR_RX_EMPTY)
		set_bit(MTS_ST_RX_RESTART, &p->state);
}

static int mts_poll(struct napi_struct *napi, int budget)
{
	struct mts_priv *p = container_of(napi, struct mts_priv, napi);
	unsigned int tail;
	bool tx_busy;
	int rx_done;
	u32 isr;

	spin_lock(&p->lock);
	if (test_and_clear_bit(MTS_ST_LSO_RECOVER, &p->state)) {
		netdev_warn(p->netdev, "LSO error, rebuilding TX ring\n");
		mts_tx_rebuild(p);
		netif_wake_queue(p->netdev);
	}
	tail = p->tx_tail;
	mts_tx_complete(p);
	tx_busy = tail != p->tx_tail;
	if (p->tx_tail != p->tx_head)
		mts_set(p, MTS_REG_TX_DMA_CTRL, MTS_DMA_KICK);
	spin_unlock(&p->lock);

	rx_done = mts_rx_poll(p, budget);

	if (test_and_clear_bit(MTS_ST_RX_RESTART, &p->state))
		mts_set(p, MTS_REG_RX_DMA_CTRL, MTS_DMA_KICK);

	if (rx_done >= budget)
		return budget;

	if (!napi_complete_done(napi, rx_done))
		return rx_done;

	if (tx_busy || rx_done) {
		if (!p->coal_on) {
			mts_wr(p, MTS_REG_COAL, MTS_COAL_EN | MTS_COAL_TICK);
			p->coal_on = true;
		}
		mts_wr(p, MTS_REG_IMR, MTS_IMR_TIMER);
		return rx_done;
	}

	if (p->coal_on) {
		mts_wr(p, MTS_REG_COAL, 0);
		p->coal_on = false;
		mts_wr(p, MTS_REG_ISR, MTS_ISR_TIMER);
	}
	mts_wr(p, MTS_REG_IMR, MTS_IMR_NORMAL);

	isr = mts_rd(p, MTS_REG_ISR) & ~(MTS_IMR_NORMAL | MTS_ISR_TIMER);
	if (isr) {
		mts_wr(p, MTS_REG_ISR, isr);
		mts_handle_status(p, isr);
		if ((isr & MTS_ISR_WORK) && napi_schedule_prep(napi)) {
			mts_wr(p, MTS_REG_IMR, MTS_IMR_ALL);
			__napi_schedule(napi);
		}
	}

	return rx_done;
}

static irqreturn_t mts_isr(int irq, void *data)
{
	struct mts_priv *p = data;
	u32 status = mts_rd(p, MTS_REG_ISR);

	if (!(status & ~MTS_IMR_NORMAL))
		return IRQ_NONE;

	mts_wr(p, MTS_REG_ISR, status);
	mts_handle_status(p, status);

	if (napi_schedule_prep(&p->napi)) {
		mts_wr(p, MTS_REG_IMR, MTS_IMR_ALL);
		__napi_schedule(&p->napi);
	}

	return IRQ_HANDLED;
}

static int mts_open(struct net_device *dev)
{
	struct mts_priv *p = netdev_priv(dev);
	int ret;

	ret = request_irq(p->irq, mts_isr, IRQF_SHARED, dev->name, p);
	if (ret)
		return ret;

	p->link_up = false;
	p->state = 0;
	netif_carrier_off(dev);

	mts_init_hw(p);
	mts_init_tx_ring(p);
	mts_init_rx_ring(p);
	mts_start_dma(p);
	mts_phy_autoneg(p);

	napi_enable(&p->napi);
	netif_start_queue(dev);

	mts_link_change(p);
	schedule_delayed_work(&p->link_poll, HZ);
	return 0;
}

static int mts_stop(struct net_device *dev)
{
	struct mts_priv *p = netdev_priv(dev);

	netif_stop_queue(dev);
	netif_carrier_off(dev);
	cancel_delayed_work_sync(&p->link_poll);
	napi_disable(&p->napi);
	mts_stop_dma(p);
	free_irq(p->irq, p);
	return 0;
}

static void mts_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct mts_priv *p = netdev_priv(dev);
	u32 val;

	netdev_warn(dev, "TX timeout, head %u tail %u\n", p->tx_head, p->tx_tail);

	spin_lock_bh(&p->lock);
	mts_set(p, MTS_REG_TX_DMA_CTRL, MTS_DMA_STOP);
	readl_poll_timeout_atomic(p->base + MTS_REG_TX_DMA_CTRL, val,
				  !(val & MTS_DMA_STOP), 1, 1000);
	mts_tx_rebuild(p);
	netif_wake_queue(dev);
	spin_unlock_bh(&p->lock);
}

static const struct net_device_ops mts_netdev_ops = {
	.ndo_open		= mts_open,
	.ndo_stop		= mts_stop,
	.ndo_start_xmit		= mts_xmit,
	.ndo_tx_timeout		= mts_tx_timeout,
	.ndo_set_mac_address	= mts_set_mac_address,
	.ndo_set_rx_mode	= mts_set_rx_mode,
	.ndo_validate_addr	= eth_validate_addr,
};

static int mts_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct net_device *netdev;
	struct mts_priv *p;
	u8 mac[ETH_ALEN];
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	netdev = devm_alloc_etherdev(&pdev->dev, sizeof(*p));
	if (!netdev)
		return -ENOMEM;
	SET_NETDEV_DEV(netdev, &pdev->dev);

	p = netdev_priv(netdev);
	p->netdev = netdev;
	p->pdev = pdev;
	spin_lock_init(&p->lock);

	p->base = pcim_iomap_region(pdev, 0, KBUILD_MODNAME);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	if (pcie_get_readrq(pdev) == 512)
		pcie_set_readrq(pdev, MTS_READRQ_SONY);

	ret = mts_alloc_rings(p);
	if (ret)
		return ret;

	ret = mts_glue_map(p);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Baikal glue BAR4 not found\n");

	mts_read_mac(p, mac);
	if (is_valid_ether_addr(mac)) {
		eth_hw_addr_set(netdev, mac);
	} else {
		eth_hw_addr_random(netdev);
		dev_warn(&pdev->dev, "no MAC address from firmware, using %pM\n",
			 netdev->dev_addr);
	}

	ret = bpcie_assign_irqs(pdev, 1);
	if (ret < 0) {
		mts_glue_unmap(p);
		return ret == -ENODEV ? -EPROBE_DEFER : ret;
	}
	p->irq = pci_irq_vector(pdev, 0);

	netdev->netdev_ops = &mts_netdev_ops;
	netdev->ethtool_ops = &mts_ethtool_ops;
	netdev->watchdog_timeo = 5 * HZ;
	netdev->min_mtu = ETH_MIN_MTU;
	netdev->max_mtu = ETH_DATA_LEN;

	netif_napi_add(netdev, &p->napi, mts_poll);
	INIT_DELAYED_WORK(&p->link_poll, mts_link_poll_work);
	pci_set_drvdata(pdev, netdev);

	ret = register_netdev(netdev);
	if (ret) {
		netif_napi_del(&p->napi);
		pci_free_irq_vectors(pdev);
		mts_glue_unmap(p);
		return ret;
	}

	netif_carrier_off(netdev);
	netdev_info(netdev, "Baikal GbE at %s, irq %d, MAC %pM\n",
		    pci_name(pdev), p->irq, netdev->dev_addr);
	return 0;
}

static void mts_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct mts_priv *p = netdev_priv(netdev);

	unregister_netdev(netdev);
	netif_napi_del(&p->napi);
	pci_free_irq_vectors(pdev);
	mts_glue_unmap(p);
}

static const struct pci_device_id mts_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_SONY_BAIKAL_GBE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, mts_pci_tbl);

static struct pci_driver mts_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= mts_pci_tbl,
	.probe		= mts_probe,
	.remove		= mts_remove,
};
module_pci_driver(mts_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Armandas Kvietkus");
MODULE_DESCRIPTION("PlayStation 4 Baikal Gigabit Ethernet driver");
