// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/mii.h>
#include <linux/minmax.h>
#include <linux/netdevice.h>

#include "mts.h"

#define MTS_SMI_DONE		BIT(15)
#define MTS_SMI_ARM		BIT(15)
#define MTS_SMI_CL22_WRITE	0x2000
#define MTS_SMI_CL22_READ	0x4000
#define MTS_SMI_CL45_ADDR	0x20
#define MTS_SMI_CL45_WRITE	0x60
#define MTS_SMI_CL45_READ	0xe0
#define MTS_SMI_TIMEOUT		10000

#define MTS_MMD(devad, reg)	(((u32)(reg) << 16) | (devad))
#define MTS_DEV_PMA		0x1e
#define MTS_DEV_VEND2		0x1f
#define MTS_DEV_AN		0x07

#define MTS_PHY_PAGE		0x1f
#define MTS_PHY_PAGE_TR		0x52b5
#define MTS_PHY_PAGE_3		0x0003
#define MTS_PHY_TR_ADDR		0x10
#define MTS_PHY_TR_LOW		0x11
#define MTS_PHY_TR_HIGH		0x12

#define MTS_FUSE_LOCK		0x80800000

static const u8 mts_cal_lut[64] = {
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
	0x7b, 0x7a, 0x75, 0x73, 0x70, 0x67, 0x64, 0x62,
	0x57, 0x55, 0x53, 0x51, 0x48, 0x46, 0x44, 0x42,
	0x40, 0x37, 0x35, 0x34, 0x32, 0x31, 0x30, 0x26,
	0x24, 0x23, 0x22, 0x21, 0x20, 0x16, 0x15, 0x14,
	0x13, 0x12, 0x11, 0x10, 0x07, 0x06, 0x05, 0x04,
	0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static u32 mts_fuse(struct mts_priv *p, u32 off)
{
	return readl(p->fuse + off);
}

static int mts_smi_cmd(struct mts_priv *p, u32 cmd)
{
	int i;

	mts_wr(p, MTS_REG_SMI, MTS_SMI_ARM);
	mts_wr(p, MTS_REG_SMI, cmd);
	for (i = 0; i < MTS_SMI_TIMEOUT; i++) {
		if (mts_rd(p, MTS_REG_SMI) & MTS_SMI_DONE)
			return 0;
		udelay(1);
	}
	netdev_warn_once(p->netdev, "SMI timeout, cmd %#x\n", cmd);
	return -ETIMEDOUT;
}

static u16 mts_mdio_read(struct mts_priv *p, u8 reg)
{
	if (mts_smi_cmd(p, ((reg & 0x1f) << 8) | MTS_SMI_CL22_READ))
		return 0;
	return mts_rd(p, MTS_REG_SMI) >> 16;
}

static void mts_mdio_write(struct mts_priv *p, u8 reg, u16 val)
{
	mts_smi_cmd(p, ((u32)val << 16) | ((reg & 0x1f) << 8) |
		    MTS_SMI_CL22_WRITE);
}

static int mts_mmd_addr(struct mts_priv *p, u32 sel)
{
	return mts_smi_cmd(p, (sel & 0xffff0000) | ((sel & 0x1f) << 8) |
			   MTS_SMI_CL45_ADDR);
}

static u16 mts_mmd_read(struct mts_priv *p, u32 sel)
{
	if (mts_mmd_addr(p, sel))
		return 0;
	if (mts_smi_cmd(p, ((sel & 0x1f) << 8) | MTS_SMI_CL45_READ))
		return 0;
	return mts_rd(p, MTS_REG_SMI) >> 16;
}

static void mts_mmd_write(struct mts_priv *p, u32 sel, u16 val)
{
	if (mts_mmd_addr(p, sel))
		return;
	mts_smi_cmd(p, ((u32)val << 16) | ((sel & 0x1f) << 8) |
		    MTS_SMI_CL45_WRITE);
}

static void mts_mmd_modify(struct mts_priv *p, u32 sel, u16 mask, u16 set)
{
	mts_mmd_write(p, sel, (mts_mmd_read(p, sel) & mask) | set);
}

static void mts_phy_tr_write(struct mts_priv *p, u16 low, u16 high, u16 addr)
{
	u16 page = mts_mdio_read(p, MTS_PHY_PAGE);

	mts_mdio_write(p, MTS_PHY_PAGE, MTS_PHY_PAGE_TR);
	mts_mdio_write(p, MTS_PHY_TR_LOW, low);
	mts_mdio_write(p, MTS_PHY_TR_HIGH, high);
	mts_mdio_write(p, MTS_PHY_TR_ADDR, addr);
	mts_mdio_write(p, MTS_PHY_PAGE, page);
}

static u8 mts_cal_value(u32 field)
{
	u32 b = field & 0x7f;

	if (b < 2)
		b = 1;
	if (b > 0x3f)
		return mts_cal_lut[0x3f];
	return mts_cal_lut[b - 1];
}

static void mts_cal_high(struct mts_priv *p, u32 sel, u32 field)
{
	mts_mmd_modify(p, sel, 0x00ff, (mts_cal_value(field) << 8) | 0x8000);
}

static void mts_cal_low(struct mts_priv *p, u32 sel, u32 field)
{
	mts_mmd_modify(p, sel, 0xff00, mts_cal_value(field) | 0x0080);
}

static void mts_cal_pair(struct mts_priv *p, u32 reg_a, u32 reg_b, u32 v, int shift)
{
	v &= 0x3f;
	mts_mmd_write(p, MTS_MMD(MTS_DEV_PMA, reg_a), (v << shift) | v);
	mts_mmd_write(p, MTS_MMD(MTS_DEV_PMA, reg_b),
		      ((min_t(u32, v, 0x3c) + 3) << shift) | v);
}

static void mts_phy_calibrate(struct mts_priv *p)
{
	u32 f5c = mts_fuse(p, 0x5c);
	u32 f60 = mts_fuse(p, 0x60);
	u32 f64 = mts_fuse(p, 0x64);
	u32 f68 = mts_fuse(p, 0x68);

	mts_mmd_write(p, MTS_MMD(MTS_DEV_PMA, 0x00e0), (f68 & 0x3f) << 8);
	mts_mmd_write(p, MTS_MMD(MTS_DEV_VEND2, 0x0115), (f68 >> 6) & 7);

	mts_cal_high(p, MTS_MMD(MTS_DEV_PMA, 0x0174), f60);
	mts_cal_low(p, MTS_MMD(MTS_DEV_PMA, 0x0174), f60 >> 19);
	mts_cal_high(p, MTS_MMD(MTS_DEV_PMA, 0x0175), f64 >> 6);
	mts_cal_low(p, MTS_MMD(MTS_DEV_PMA, 0x0175), f64 >> 25);

	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0172), 0xc0ff, (f5c >> 12) & 0x3f00);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0172), 0xffc0, (f60 >> 7) & 0x3f);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0173), 0xc0ff, (f60 >> 18) & 0x3f00);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0173), 0xffc0, (f64 >> 13) & 0x3f);

	mts_cal_pair(p, 0x0012, 0x0016, f5c >> 26, 10);
	mts_cal_pair(p, 0x0017, 0x0018, f60 >> 13, 8);
	mts_cal_pair(p, 0x0019, 0x0020, f64, 8);
	mts_cal_pair(p, 0x0021, 0x0022, f64 >> 19, 8);

	mts_mmd_write(p, MTS_MMD(MTS_DEV_PMA, 0x0096), 0x8000);
	mts_mmd_write(p, MTS_MMD(MTS_DEV_PMA, 0x0037), 0x0033);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0039), 0xb7ff, 0);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_VEND2, 0x0107), 0xefff, 0);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0171), 0xffff, 0x0180);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0039), 0xffff, 0x2000);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0039), 0xdfff, 0);
	udelay(50);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0171), 0xfe7f, 0);
}

void mts_phy_init(struct mts_priv *p)
{
	u32 link;

	mts_mdio_read(p, MII_PHYSID1);
	mts_mdio_read(p, MII_PHYSID2);

	mts_wr(p, MTS_REG_RESET, MTS_RESET_VAL);
	mts_mmd_write(p, MTS_MMD(MTS_DEV_PMA, 0x0147), 0);

	if ((mts_fuse(p, 0x6c) & MTS_FUSE_LOCK) == MTS_FUSE_LOCK)
		mts_phy_calibrate(p);
	else
		netdev_info(p->netdev, "fuse calibration not valid, skipped\n");

	link = mts_rd(p, MTS_REG_LINK);

	mts_mmd_write(p, MTS_MMD(MTS_DEV_PMA, 0x0189), 0x0110);
	mts_phy_tr_write(p, 0xb90a, 0x006f, 0x8f82);
	mts_phy_tr_write(p, 0xbaef, 0x002e, 0x968c);

	mts_mdio_write(p, MTS_PHY_PAGE, MTS_PHY_PAGE_3);
	mts_mdio_write(p, 0x1c, 0x0c92);
	mts_mdio_write(p, MTS_PHY_PAGE, 0);

	mts_wr(p, MTS_REG_CLK_FREQ, MTS_CLK_FREQ_HZ);
	mts_mmd_write(p, MTS_MMD(MTS_DEV_PMA, 0x0122), 0xffff);
	mts_phy_tr_write(p, 0x704d, 0x0000, 0x9698);
	mts_phy_tr_write(p, 0x344f, 0x0002, 0x969a);
	mts_mmd_write(p, MTS_MMD(MTS_DEV_VEND2, 0x0268), 0x07f4);
	mts_phy_tr_write(p, 0x0004, 0x0000, 0x9686);
	mts_phy_tr_write(p, 0x0671, 0x0006, 0x8fae);

	mts_mdio_write(p, MII_ADVERTISE, mts_mdio_read(p, MII_ADVERTISE) & 0xf3ff);
	mts_wr(p, MTS_REG_LINK, link & MTS_LINK_KEEP);
	mts_clr(p, MTS_REG_RX_PCODE, BIT(0));
	mts_mmd_write(p, MTS_MMD(MTS_DEV_AN, 0x003c), 0);
	mts_mmd_modify(p, MTS_MMD(MTS_DEV_PMA, 0x0033), 0xefff, 0);
	mts_mdio_write(p, MII_BMCR, mts_mdio_read(p, MII_BMCR) |
		       BMCR_ANENABLE | BMCR_ANRESTART);
}

void mts_phy_resolve(struct mts_priv *p, int *speed, bool *full)
{
	u16 ctrl1000 = mts_mdio_read(p, MII_CTRL1000);
	u16 stat1000 = mts_mdio_read(p, MII_STAT1000);
	u16 common = mts_mdio_read(p, MII_ADVERTISE) & mts_mdio_read(p, MII_LPA);

	if ((ctrl1000 & ADVERTISE_1000FULL) && (stat1000 & LPA_1000FULL)) {
		*speed = SPEED_1000;
		*full = true;
	} else if (common & (ADVERTISE_100FULL | ADVERTISE_100HALF)) {
		*speed = SPEED_100;
		*full = common & ADVERTISE_100FULL;
	} else {
		*speed = SPEED_10;
		*full = common & ADVERTISE_10FULL;
	}
}

void mts_phy_autoneg(struct mts_priv *p)
{
	u16 adv = mts_mdio_read(p, MII_ADVERTISE);
	u16 ctrl1000 = mts_mdio_read(p, MII_CTRL1000);

	mts_mdio_write(p, MII_ADVERTISE, adv | ADVERTISE_ALL);
	mts_mdio_write(p, MII_CTRL1000, (ctrl1000 & 0xfcff) | ADVERTISE_1000FULL);
	mts_mdio_write(p, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART |
		       BMCR_FULLDPLX | BMCR_SPEED1000);

	netdev_info(p->netdev,
		    "phy %04x:%04x bmcr %04x bmsr %04x adv %04x ctrl1000 %04x\n",
		    mts_mdio_read(p, MII_PHYSID1), mts_mdio_read(p, MII_PHYSID2),
		    mts_mdio_read(p, MII_BMCR), mts_mdio_read(p, MII_BMSR),
		    mts_mdio_read(p, MII_ADVERTISE), mts_mdio_read(p, MII_CTRL1000));
}
