// SPDX-License-Identifier: MIT

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

#define LIVERPOOL_TARGET_SCLK_DID       1
#define LIVERPOOL_CLK_TIMEOUT_US        10
#define LIVERPOOL_CLK_TIMEOUT_ITER      1000

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

int liverpool_clk_force_max(struct amdgpu_device *adev)
{
	u32 spll_fuses, sclk_fuses, spll_cntl, spll_fb;
	u32 spll_freq_id_startup, spll_freq_id_max;
	u32 startup_did, current_did;
	u32 spll_pdiva, spll_fbdiv;
	u32 cntl, status;
	bool expected_done_tog;
	int i;

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

	if (adev->asic_type == CHIP_GLADIUS)
		return gladius_gck_mailbox(adev);

	cntl = RREG32_SMC(CG_SCLK_CNTL);
	dev_info(adev->dev,
		 "Liverpool CLK: CG_SCLK_CNTL=0x%08x DIVIDER=%u DIRCNTLEN=%u DIRCNTLDIV=%u\n",
		 cntl,
		 (cntl & SCLK_DIVIDER_MASK)     >> SCLK_DIVIDER_SHIFT,
		 !!(cntl & SCLK_DIRCNTL_EN),
		 (cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT);

	if ((cntl & SCLK_DIRCNTL_EN) &&
	    ((cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT) == LIVERPOOL_TARGET_SCLK_DID) {
		dev_info(adev->dev, "Liverpool CLK: already at DID=1, no change.\n");
		return 0;
	}

	for (i = 0; i < LIVERPOOL_CLK_TIMEOUT_ITER; i++) {
		if (RREG32_SMC(CG_SCLK_STATUS) & SCLK_STATUS_DONE)
			break;
		udelay(LIVERPOOL_CLK_TIMEOUT_US);
	}
	if (i == LIVERPOOL_CLK_TIMEOUT_ITER) {
		dev_err(adev->dev,
			"Liverpool CLK: timed out waiting for SCLK idle (0x%08x)\n",
			RREG32_SMC(CG_SCLK_STATUS));
		return -ETIMEDOUT;
	}

	cntl &= ~SCLK_DIRCNTL_DIV_MASK;
	cntl |= (LIVERPOOL_TARGET_SCLK_DID << SCLK_DIRCNTL_DIV_SHIFT) & SCLK_DIRCNTL_DIV_MASK;
	cntl |= SCLK_DIRCNTL_EN;
	cntl ^= SCLK_DIRCNTL_TOG;
	expected_done_tog = !!(cntl & SCLK_DIRCNTL_TOG);
	WREG32_SMC(CG_SCLK_CNTL, cntl);

	for (i = 0; i < LIVERPOOL_CLK_TIMEOUT_ITER; i++) {
		status = RREG32_SMC(CG_SCLK_STATUS);
		if (!!(status & SCLK_DIRCNTL_DONE_TOG) == expected_done_tog)
			break;
		udelay(LIVERPOOL_CLK_TIMEOUT_US);
	}
	if (i == LIVERPOOL_CLK_TIMEOUT_ITER) {
		dev_err(adev->dev,
			"Liverpool CLK: timed out waiting for force done (0x%08x)\n",
			RREG32_SMC(CG_SCLK_STATUS));
		return -ETIMEDOUT;
	}

	cntl   = RREG32_SMC(CG_SCLK_CNTL);
	status = RREG32_SMC(CG_SCLK_STATUS);
	dev_info(adev->dev,
		 "Liverpool CLK: force done. CG_SCLK_CNTL=0x%08x DIRCNTLDIV=%u status=0x%08x\n",
		 cntl,
		 (cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT,
		 status);

	if (((cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT) != LIVERPOOL_TARGET_SCLK_DID)
		dev_warn(adev->dev,
			 "Liverpool CLK: readback DID mismatch â€” expected %u got %u\n",
			 LIVERPOOL_TARGET_SCLK_DID,
			 (cntl & SCLK_DIRCNTL_DIV_MASK) >> SCLK_DIRCNTL_DIV_SHIFT);

	return 0;
}
