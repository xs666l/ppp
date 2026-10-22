// SPDX-License-Identifier: GPL-2.0
/*
 * y700_dsi v2 - READ-ONLY investigation module for Lenovo Legion Y700 Gen4.
 *
 * It never modifies anything. It only logs:
 *  1. every dsi_display_set_mode() call (as in v1)
 *  2. what dsi_display_get_modes() returns: the master list of display modes
 *     (full raw dump of each entry) and the offset of that list inside the
 *     dsi_display object. Trigger it with:
 *        echo detect > /sys/class/drm/card0-DSI-1/status
 *
 * Parameter: /sys/module/y700_dsi/parameters/dump (1 = log get_modes, default)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/moduleparam.h>

static int dump = 1;
module_param(dump, int, 0644);
MODULE_PARM_DESC(dump, "1 = log dsi_display_get_modes results (default 1)");

static inline bool kptr_ok(unsigned long p)
{
	return (s64)p < 0 && !(p & 3);
}

/* ---- v1 behaviour: log set_mode ---- */
static int sm_pre(struct kprobe *p, struct pt_regs *regs)
{
	u32 *m = (u32 *)regs->regs[1];

	if (!kptr_ok((unsigned long)m))
		return 0;
	pr_info("y700_dsi: set_mode %ux%u hbp=%u hsw=%u hfp=%u vbp=%u vsw=%u vfp=%u fps=%u clk=%u\n",
		m[0], m[6], m[1], m[2], m[3], m[7], m[8], m[9], m[11], m[12]);
	return 0;
}

static struct kprobe kp_sm = {
	.symbol_name = "dsi_display_set_mode",
	.pre_handler = sm_pre,
};

/* ---- v2: observe dsi_display_get_modes(display, &out_modes) ---- */
struct gm_ctx {
	unsigned long disp;
	unsigned long out;
};

static int gm_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct gm_ctx *c = (struct gm_ctx *)ri->data;

	c->disp = regs->regs[0];
	c->out = regs->regs[1];
	return 0;
}

#define SCAN_WORDS	256
#define MAX_ENTRIES	8
#define MAX_STRIDE	160
#define DISP_SCAN_U64	768	/* 6 KB of the display object */

static void dump_words(const char *tag, const u32 *w, int n)
{
	int j;

	for (j = 0; j < n; j += 8)
		pr_info("y700_dsi: %s+%d: %u %u %u %u %u %u %u %u\n", tag, j,
			w[j], w[j + 1], w[j + 2], w[j + 3],
			w[j + 4], w[j + 5], w[j + 6], w[j + 7]);
}

static int gm_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct gm_ctx *c = (struct gm_ctx *)ri->data;
	unsigned long out = c->out, mp;
	const u32 *b;
	int idx[MAX_ENTRIES], n = 0, i, e, stride;
	char tag[16];

	if (!dump)
		return 0;

	pr_info("y700_dsi: get_modes ret=%ld display=%lx out=%lx\n",
		(long)regs_return_value(regs), c->disp, out);
	if (!kptr_ok(out) || (out & 7))
		return 0;

	mp = *(unsigned long *)out;
	pr_info("y700_dsi: *out (modes array) = %lx\n", mp);
	if (!kptr_ok(mp))
		return 0;
	b = (const u32 *)mp;

	for (i = 0; i + 6 < SCAN_WORDS && n < MAX_ENTRIES; i++)
		if (b[i] == 1904 && b[i + 1] == 32 && b[i + 2] == 32 && b[i + 6] == 3040)
			idx[n++] = i;

	pr_info("y700_dsi: found %d mode entries at word offsets: %d %d %d %d %d %d %d %d\n",
		n, n > 0 ? idx[0] : -1, n > 1 ? idx[1] : -1, n > 2 ? idx[2] : -1,
		n > 3 ? idx[3] : -1, n > 4 ? idx[4] : -1, n > 5 ? idx[5] : -1,
		n > 6 ? idx[6] : -1, n > 7 ? idx[7] : -1);

	stride = (n >= 2) ? idx[1] - idx[0] : 0;
	if (stride <= 0 || stride > MAX_STRIDE)
		stride = 64;
	pr_info("y700_dsi: entry stride = %d words (%d bytes)\n", stride, stride * 4);

	for (e = 0; e < n; e++) {
		snprintf(tag, sizeof(tag), "mode%d", e);
		dump_words(tag, b + idx[e], (stride + 7) & ~7);
	}

	/* where does the display object keep the modes pointer / count? */
	if (kptr_ok(c->disp) && !(c->disp & 7)) {
		const unsigned long *d = (const unsigned long *)c->disp;
		const u32 *d32 = (const u32 *)c->disp;

		for (i = 0; i < DISP_SCAN_U64; i++) {
			if (d[i] == mp) {
				int k;

				pr_info("y700_dsi: display->modes pointer at byte offset 0x%x\n", i * 8);
				for (k = 0; k < 16; k++) {
					int w = i * 2 + k - 8;

					if (w >= 0 && w < DISP_SCAN_U64 * 2 && d32[w] >= 1 && d32[w] <= 16)
						pr_info("y700_dsi:   small u32 %u at byte offset 0x%x\n", d32[w], w * 4);
				}
			}
		}
	}
	return 0;
}

static struct kretprobe krp_gm = {
	.kp.symbol_name = "dsi_display_get_modes",
	.entry_handler = gm_entry,
	.handler = gm_ret,
	.data_size = sizeof(struct gm_ctx),
	.maxactive = 4,
};

static int __init y700_dsi_init(void)
{
	int ret = register_kprobe(&kp_sm);

	if (ret < 0) {
		pr_err("y700_dsi: register set_mode probe failed: %d\n", ret);
		return ret;
	}
	ret = register_kretprobe(&krp_gm);
	if (ret < 0) {
		pr_err("y700_dsi: register get_modes probe failed: %d\n", ret);
		unregister_kprobe(&kp_sm);
		return ret;
	}
	pr_info("y700_dsi: v2 loaded (read-only)\n");
	return 0;
}

static void __exit y700_dsi_exit(void)
{
	unregister_kretprobe(&krp_gm);
	unregister_kprobe(&kp_sm);
	pr_info("y700_dsi: unloaded\n");
}

module_init(y700_dsi_init);
module_exit(y700_dsi_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Y700 Gen4 DSI read-only investigation module (v2)");
