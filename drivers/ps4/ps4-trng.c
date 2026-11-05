// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "ps4-trng: " fmt

#include <linux/hw_random.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/ps4.h>

#define PS4_TRNG_CHUNK	16

static int ps4_trng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	size_t len = min_t(size_t, max, PS4_TRNG_CHUNK);
	int ret;

	ret = ps4_samu_rng(data, len);
	return ret ? ret : len;
}

static struct hwrng ps4_trng = {
	.name = "ps4-trng",
	.read = ps4_trng_read,
	.quality = 900,
};

static int __init ps4_trng_init(void)
{
	u8 probe[PS4_TRNG_CHUNK];
	int ret;

	ret = ps4_samu_rng(probe, sizeof(probe));
	if (ret) {
		pr_err("samu rng request failed: %d\n", ret);
		return ret;
	}

	ret = hwrng_register(&ps4_trng);
	if (ret)
		pr_err("hwrng_register failed: %d\n", ret);
	else
		pr_info("registered with the hwrng core\n");
	return ret;
}
late_initcall(ps4_trng_init);

MODULE_AUTHOR("Armandas Kvietkus <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("PS4 SAMU hardware random number generator");
MODULE_LICENSE("GPL");
