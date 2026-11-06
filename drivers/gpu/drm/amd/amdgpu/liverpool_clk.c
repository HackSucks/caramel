// SPDX-License-Identifier: MIT

#include <asm/ps4.h>
#include <linux/delay.h>
#include "amdgpu.h"
#include "liverpool_clk.h"
#include "liverpool_smu.h"

#define GCK_SCLK_FUSES                  0xc0500004
#define   STARTUP_SCLK_DID_SHIFT          0
#define   STARTUP_SCLK_DID_MASK           (0x7f << 0)

#define GCK_SPLL_FUSES                  0xc050002c
#define   SPLL_FREQ_ID_STARTUP_SHIFT      1
#define   SPLL_FREQ_ID_STARTUP_MASK       (0x3f << 1)
#define   SPLL_FREQ_ID_MAX_SHIFT          7
#define   SPLL_FREQ_ID_MAX_MASK           (0x3f << 7)

#define CG_SCLK_CNTL                    0xc050008c
#define CG_SCLK_CNTL_80                 0xc0500080
#define CG_SCLK_STATUS_84               0xc0500084
#define   SCLK_DIVIDER_SHIFT              0
#define   SCLK_DIVIDER_MASK               (0x7f << 0)
#define   SCLK_DIRCNTL_EN                 BIT(8)
#define   SCLK_DIRCNTL_TOG                BIT(9)
#define   SCLK_DIRCNTL_DIV_SHIFT          10
#define   SCLK_DIRCNTL_DIV_MASK           (0x7f << 10)

#define CG_SCLK_STATUS                  0xc0500090
#define   SCLK_STATUS_DONE                BIT(0)
#define   SCLK_FORCE_STATUS_DONE          BIT(1)
#define   SCLK_DIRCNTL_DONE_TOG           BIT(3)

#define CG_SPLL_FUNC_CNTL               0xc0500100
#define   SPLL_PDIVA_SHIFT                20
#define   SPLL_PDIVA_MASK                 (0x7f << 20)

#define CG_SPLL_FUNC_CNTL_3             0xc0500108
#define   SPLL_FB_DIV_SHIFT               0
#define   SPLL_FB_DIV_MASK                (0x3ffffff << 0)

#define CG_SPLL_STATUS                  0xc0500114
#define   SPLL_UNLOCK_STICKY              BIT(7)

#define SCLK_STARTUP_DID                0xe000304c
#define   SCLKSTARTUPDID_MASK             0x7f

#define VDDNB_REQUEST                   0xc0200090
#define VDDNB_STATUS                    0xc0200094
#define LIVERPOOL_FUSE_BASE             0xc0100000
#define LIVERPOOL_SCLK_LEVELS           5
#define LIVERPOOL_SCLK_POLL             100001
#define LIVERPOOL_VDDNB_POLL            10000
#define LIVERPOOL_VID_MIN               0x58
#define LIVERPOOL_VID_MAX               0x74
#define LIVERPOOL_DENTIST_DISPCLK_CNTL  (0x490 / 4)

#define GLADIUS_GCK_SCLK_HI		0x400000

static void gladius_log_sclk_pair(struct amdgpu_device *adev, const char *when)
{
	u32 r80 = RREG32_SMC(CG_SCLK_CNTL_80);
	u32 r84 = RREG32_SMC(CG_SCLK_STATUS_84);
	u32 r8c = RREG32_SMC(CG_SCLK_CNTL);
	u32 r90 = RREG32_SMC(CG_SCLK_STATUS);

	dev_info(adev->dev,
		 "Gladius SCLK 0x80 %s: 0x80=0x%08x (DID=%u EN=%u TOG=%u DIV=%u) 0x84=0x%08x 0x8c=0x%08x 0x90=0x%08x\n",
		 when, r80,
		 (r80 & SCLK_DIVIDER_MASK) >> SCLK_DIVIDER_SHIFT,
		 !!(r80 & SCLK_DIRCNTL_EN),
		 !!(r80 & SCLK_DIRCNTL_TOG),
		 (r80 & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT,
		 r84, r8c, r90);
}

static void gladius_spll_911(struct amdgpu_device *adev)
{
	u32 fb = (911u * 0x28f5c3u) >> 10;
	u32 st = 0;
	int i, r;

	r = liverpool_smu_samu_read(adev, 0xc05002e4, &st);
	dev_info(adev->dev,
		 "Gladius SPLL before 2e4 r=%d val=0x%08x want=0x%08x\n",
		 r, st, fb);

	r = liverpool_smu_samu_write(adev, 0xc05002e4, fb);
	dev_info(adev->dev, "Gladius SPLL wr 2e4=0x%08x r=%d\n", fb, r);
	liverpool_smu_samu_write(adev, 0xc05002b4, 0x100);
	liverpool_smu_samu_write(adev, 0xc05002dc, 0x14009);
	liverpool_smu_samu_write(adev, 0xc05002b0, 0x19);
	liverpool_smu_samu_write(adev, 0xc05002c0, 0x8000082);
	liverpool_smu_samu_write(adev, 0xc05002c4, 0x64000000);
	liverpool_smu_samu_write(adev, 0xc05002ac, 0x60840000);

	for (i = 0; i < 1000; i++) {
		st = 0;
		liverpool_smu_samu_read(adev, 0xc05002e0, &st);
		if (!(st & 4))
			break;
		udelay(10);
	}
	dev_info(adev->dev, "Gladius SPLL 2e0 wait-clr i=%d val=0x%08x\n", i, st);

	liverpool_smu_samu_write(adev, 0xc05002ac, 0x40840000);

	for (i = 0; i < 1000; i++) {
		st = 0;
		liverpool_smu_samu_read(adev, 0xc05002e0, &st);
		if (st & 4)
			break;
		udelay(10);
	}
	dev_info(adev->dev, "Gladius SPLL 2e0 wait-set i=%d val=0x%08x\n", i, st);
}

static int gladius_gck_mailbox(struct amdgpu_device *adev)
{
	static const u32 addrs[] = {
		0xc0500080, 0xc050008c,
		0xc050009c, 0xc05000a0,
		0xc05000a4, 0xc05000a8,
		0xc05000dc,
	};
	static const char *names[] = {
		"80", "8c", "9c", "a0", "a4", "a8", "dc"
	};
	u32 v[7], smc80;
	int i, r;

	gladius_log_sclk_pair(adev, "mb before");
	gladius_spll_911(adev);
	smc80 = RREG32_SMC(CG_SCLK_CNTL_80);

	for (i = 0; i < 7; i++) {
		v[i] = 0;
		r = liverpool_smu_samu_read(adev, addrs[i], &v[i]);
		dev_info(adev->dev,
			 "Gladius GCK mb rd %s: r=%d val=0x%08x\n",
			 names[i], r, v[i]);
	}

	dev_info(adev->dev,
		 "Gladius GCK mb: SMC_IND 0x80=0x%08x mb 0x80=0x%08x DIV=%u (leave strap)\n",
		 smc80, v[0],
		 (v[0] & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT);
	gladius_log_sclk_pair(adev, "mb after");
	return 0;
}

int liverpool_clk_gladius_set_div(struct amdgpu_device *adev, unsigned int div)
{
	u32 cur = 0, want, after = 0;
	int r, wr;

	if (adev->asic_type != CHIP_GLADIUS)
		return -EINVAL;

	r = liverpool_smu_samu_read(adev, CG_SCLK_CNTL_80, &cur);
	want = cur | GLADIUS_GCK_SCLK_HI;
	want &= ~SCLK_DIRCNTL_DIV_MASK;
	want |= (div << SCLK_DIRCNTL_DIV_SHIFT) & SCLK_DIRCNTL_DIV_MASK;
	want |= SCLK_DIRCNTL_EN;
	want ^= SCLK_DIRCNTL_TOG;
	wr = liverpool_smu_samu_write(adev, CG_SCLK_CNTL_80, want);
	liverpool_smu_samu_read(adev, CG_SCLK_CNTL_80, &after);
	dev_info(adev->dev,
		 "Gladius GCK set DIV=%u rd=%d wr=%d cur=0x%08x want=0x%08x after=0x%08x\n",
		 div, r, wr, cur, want, after);
	return wr;
}

static u32 liverpool_fuse(struct amdgpu_device *adev, u32 block, u32 bit, u32 width)
{
	u32 reg = LIVERPOOL_FUSE_BASE | (block << 12) | ((bit >> 5) * 4);
	u32 shift = bit & 31;
	u64 value = RREG32_SMC(reg);

	if (shift + width > 32)
		value |= (u64)RREG32_SMC(reg + 4) << 32;
	return (value >> shift) & (BIT_ULL(width) - 1);
}

static int liverpool_poll_bit0(struct amdgpu_device *adev, u32 reg, u32 tries)
{
	u32 i;

	for (i = 0; i < tries; i++)
		if (RREG32_SMC(reg) & 1)
			return 0;
	return -ETIMEDOUT;
}

static u32 liverpool_did_clock(u32 did)
{
	if (did < 8)
		return 80000;
	if (did < 0x40)
		return 1280000 / did;
	if (did < 0x60)
		return 320000 / (16 + (did - 0x40) / 2);
	if (did < 0x7f)
		return 320000 / (did - 0x40);
	return 2500;
}

static u32 liverpool_clock_level(struct amdgpu_device *adev, u32 block, u32 bit,
				 u32 levels, u32 did)
{
	u32 clock = liverpool_did_clock(did);
	u32 k;

	for (k = 0; k < levels - 1; k++)
		if (clock <= liverpool_did_clock(liverpool_fuse(adev, block, bit + 7 * k, 7)))
			break;
	return k;
}

static u32 liverpool_vid_for_clocks(struct amdgpu_device *adev, u32 sclk_did)
{
	static const struct {
		u32 reg, block, bit, bit_early;
	} clocks[] = {
		{ 0xc05000dc, 4, 0x3e3, 0x3c2 },
		{ 0xc050009c, 7, 0x3b8, 0x3b8 },
		{ 0xc05000ac, 4, 0x3c7, 0x3a6 },
		{ 0xc0500094, 7, 0x398, 0x398 },
		{ 0xc05000e4, 4, 0x3ff, 0x3de },
		{ 0xc05000a4, 7, 0x3d4, 0x3d4 },
	};
	bool slim = adev->pdev->device == 0x9923;
	u32 level, k, i, sel;

	level = liverpool_clock_level(adev, 7, 0x349, LIVERPOOL_SCLK_LEVELS, sclk_did);
	for (i = 0; i < ARRAY_SIZE(clocks); i++) {
		k = liverpool_clock_level(adev, clocks[i].block,
					  slim ? clocks[i].bit : clocks[i].bit_early, 4,
					  RREG32_SMC(clocks[i].reg) & SCLK_DIVIDER_MASK);
		level = max(level, k);
	}
	k = liverpool_clock_level(adev, 7, 0x37c, 4,
				  RREG32(LIVERPOOL_DENTIST_DISPCLK_CNTL) & SCLK_DIVIDER_MASK);
	level = max(level, k);

	sel = liverpool_fuse(adev, 7, 0x33f + 2 * level, 2);
	return liverpool_fuse(adev, 4, 0x33 + 8 * sel, 8);
}

static int liverpool_vid_set(struct amdgpu_device *adev, u32 vid)
{
	int r = liverpool_smu_samu_write(adev, VDDNB_REQUEST, vid * 2 + 1);

	if (!r)
		r = liverpool_poll_bit0(adev, VDDNB_STATUS, LIVERPOOL_VDDNB_POLL);
	if (r)
		dev_err(adev->dev, "Liverpool SCLK: VID 0x%02x failed (%d)\n", vid, r);
	return r;
}

static int liverpool_sclk_set_top(struct amdgpu_device *adev)
{
	u32 cntl, current_did, top, target, vid, current_vid;
	int r;

	top = liverpool_fuse(adev, 7, 0x349 + 7 * (LIVERPOOL_SCLK_LEVELS - 1), 7);
	cntl = RREG32_SMC(CG_SCLK_CNTL);
	current_did = cntl & SCLK_DIVIDER_MASK;

	if (top < 8 || top > 0x3f) {
		dev_warn(adev->dev, "Liverpool SCLK: invalid fused divider 0x%02x\n", top);
		return -EINVAL;
	}

	target = min(current_did, top);
	vid = liverpool_vid_for_clocks(adev, target);
	current_vid = (RREG32_SMC(VDDNB_STATUS) >> 1) & 0xff;

	dev_info(adev->dev,
		 "Liverpool SCLK: divider 0x%02x, fused top 0x%02x (%u MHz), VID 0x%02x, fused VID 0x%02x\n",
		 current_did, top, 12800 / top, current_vid, vid);

	if (vid < LIVERPOOL_VID_MIN || vid > LIVERPOOL_VID_MAX) {
		dev_warn(adev->dev, "Liverpool SCLK: fused VID 0x%02x out of range\n", vid);
		return -EINVAL;
	}

	if (vid < current_vid) {
		r = liverpool_vid_set(adev, vid);
		if (r)
			return r;
	}

	if (current_did > top) {
		r = liverpool_smu_samu_write(adev, CG_SCLK_CNTL, cntl & ~SCLK_DIRCNTL_EN);
		if (!r)
			r = liverpool_poll_bit0(adev, CG_SCLK_STATUS, LIVERPOOL_SCLK_POLL);
		if (!r)
			r = liverpool_smu_samu_write(adev, CG_SCLK_CNTL,
						     (RREG32_SMC(CG_SCLK_CNTL) & ~SCLK_DIVIDER_MASK) | top);
		if (!r)
			r = liverpool_poll_bit0(adev, CG_SCLK_STATUS, LIVERPOOL_SCLK_POLL);
		if (r) {
			dev_err(adev->dev, "Liverpool SCLK: divider write failed (%d)\n", r);
			return r;
		}
	}

	if (vid > current_vid) {
		r = liverpool_vid_set(adev, vid);
		if (r)
			return r;
	}

	cntl = RREG32_SMC(CG_SCLK_CNTL);
	current_vid = (RREG32_SMC(VDDNB_STATUS) >> 1) & 0xff;
	if (current_did != target || current_did != (cntl & SCLK_DIVIDER_MASK) || current_vid != vid)
		dev_info(adev->dev, "Liverpool SCLK: now divider 0x%02x, VID 0x%02x\n",
			 cntl & SCLK_DIVIDER_MASK, current_vid);
	return ((cntl & SCLK_DIVIDER_MASK) == target && current_vid == vid) ? 0 : -EIO;
}

int liverpool_clk_force_max(struct amdgpu_device *adev)
{
	u32 spll_fuses, sclk_fuses, spll_cntl, spll_fb;
	u32 spll_freq_id_startup, spll_freq_id_max;
	u32 startup_did, current_did;
	u32 spll_pdiva, spll_fbdiv;
	u32 status;

	if (!ps4_is_baikal())
		return 0;

	if (adev->asic_type != CHIP_GLADIUS)
		return liverpool_sclk_set_top(adev);

	spll_fuses = RREG32_SMC(GCK_SPLL_FUSES);
	sclk_fuses = RREG32_SMC(GCK_SCLK_FUSES);
	spll_cntl  = RREG32_SMC(CG_SPLL_FUNC_CNTL);
	spll_fb    = RREG32_SMC(CG_SPLL_FUNC_CNTL_3);

	spll_freq_id_startup = (spll_fuses & SPLL_FREQ_ID_STARTUP_MASK) >> SPLL_FREQ_ID_STARTUP_SHIFT;
	spll_freq_id_max     = (spll_fuses & SPLL_FREQ_ID_MAX_MASK)     >> SPLL_FREQ_ID_MAX_SHIFT;
	startup_did          = (sclk_fuses & STARTUP_SCLK_DID_MASK)     >> STARTUP_SCLK_DID_SHIFT;
	current_did          = RREG32_SMC(SCLK_STARTUP_DID) & SCLKSTARTUPDID_MASK;
	spll_pdiva           = (spll_cntl & SPLL_PDIVA_MASK)            >> SPLL_PDIVA_SHIFT;
	spll_fbdiv           = (spll_fb  & SPLL_FB_DIV_MASK)            >> SPLL_FB_DIV_SHIFT;

	dev_info(adev->dev,
		 "Liverpool CLK: SPLL startup_freq_id=%u max_freq_id=%u "
		 "SCLK startup_DID=%u current_DID=%u\n",
		 spll_freq_id_startup, spll_freq_id_max, startup_did, current_did);
	dev_info(adev->dev,
		 "Liverpool CLK: SPLL PDIVA=%u FBDIV=0x%x\n",
		 spll_pdiva, spll_fbdiv);

	if (spll_pdiva > 4)
		dev_warn(adev->dev,
			 "Liverpool CLK: SPLL PDIVA=%u > 4 â€” SPLL may have been "
			 "reconfigured by soft-reset. Report PDIVA+FBDIV.\n",
			 spll_pdiva);

	status = RREG32_SMC(CG_SPLL_STATUS);
	if (status & SPLL_UNLOCK_STICKY)
		dev_warn(adev->dev,
			 "Liverpool CLK: SPLL unlock sticky set (0x%08x)\n",
			 status);

	return gladius_gck_mailbox(adev);
}
