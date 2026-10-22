// SPDX-License-Identifier: GPL-2.0
/*
 * y700_dsi v4 - Lenovo Legion Y700 Gen4 (SM8750): add a NEW 170Hz mode.
 *
 * Verified on this device (from v2/v3 dumps):
 *  - dsi_display_get_modes() returns the driver's master mode list
 *    (display->modes, 200-byte entries = 50 u32):
 *      w3 hfp  w7 vbp  w9 vfp  w11 refresh  w12 clk  w40 pixel_clk_khz
 *      w46 mode index  w48/49 priv_info pointer
 *    list order: 0=120Hz 1=165Hz 2=144Hz 3=90Hz 4=60Hz 5=30Hz
 *  - all modes share one pixel clock; refresh is set purely by the porches:
 *      fps = 172.992e6 / ((254 + hbp + hsw + hfp) * vtotal)   (per link,
 *    every horizontal word in the list is 2x the per-link value)
 *
 * The mode COUNT is stored elsewhere (not mapped yet), so a 7th entry cannot
 * be appended. Instead the entry in <slot> (default 5 = the 30Hz mode) is
 * overwritten with a CLONE of the 165Hz entry whose hfp/refresh are changed.
 * The 165Hz mode stays. Same pixel clock, porch-only, like the vendor's own
 * 165/144Hz modes. Nothing happens until the connector is probed:
 *     echo detect > /sys/class/drm/card0-DSI-1/status
 *
 * patch_list=1 apply, patch_list=0 restore (also restored on rmmod).
 * Be on 120Hz when applying / restoring / unloading.
 *
 * dump=1 additionally prints the per-mode priv_info blocks of entries 0 and 1
 * (read-only) so the clock fields can be located later.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/moduleparam.h>

#define ENTRY_WORDS	50
#define MAX_ENTRIES	8
#define SRC_HFP2	100	/* stock 165Hz hfp, both links (2 x 50) */
#define SRC_FPS		165
#define PRIV_DUMP_WORDS	64

static int patch_list;
module_param(patch_list, int, 0644);
MODULE_PARM_DESC(patch_list, "1 = write the new mode on next connector probe, 0 = restore");

static int slot = 5;
module_param(slot, int, 0644);
MODULE_PARM_DESC(slot, "list index overwritten by the new mode (1..5, default 5 = 30Hz)");

static int hfp = 40;
module_param(hfp, int, 0644);
MODULE_PARM_DESC(hfp, "per-link hfp of the new mode (8..50); 40=170Hz 29=176Hz 22=180Hz 14=185Hz");

static int new_fps = 170;
module_param(new_fps, int, 0644);
MODULE_PARM_DESC(new_fps, "refresh rate label of the new mode (166..190)");

static int dump;
module_param(dump, int, 0644);
MODULE_PARM_DESC(dump, "1 = dump list summary and priv_info blocks on each probe");

static u32 *last_modes;			/* master list, for restore on unload */
static int applied;
static int applied_slot;
static u32 applied_hfp2, applied_fps;
static u32 saved[ENTRY_WORDS];

static inline bool kptr_ok(unsigned long p)
{
	return (s64)p < 0 && !(p & 3);
}

static bool entry_ok(const u32 *m)
{
	unsigned long priv = ((unsigned long)m[49] << 32) | m[48];

	return m[0] == 1904 && m[1] == 32 && m[2] == 32 && m[6] == 3040 &&
	       kptr_ok(priv);
}

static bool stock_fps(u32 f)
{
	return f == 30 || f == 60 || f == 90 || f == 120 || f == 144 || f == 165;
}

static int count_entries(const u32 *b)
{
	int n = 0;

	while (n < MAX_ENTRIES && entry_ok(b + n * ENTRY_WORDS))
		n++;
	return n;
}

static int find_src(const u32 *b, int n)
{
	int e;

	for (e = 0; e < n; e++) {
		const u32 *m = b + e * ENTRY_WORDS;

		if (m[3] == SRC_HFP2 && m[7] == 50 && m[9] == 26 && m[11] == SRC_FPS)
			return e;
	}
	return -1;
}

static void restore_list(u32 *b)
{
	u32 *d;
	int i;

	if (!applied)
		return;
	d = b + applied_slot * ENTRY_WORDS;
	if (!entry_ok(d) || d[11] != applied_fps || d[3] != applied_hfp2) {
		pr_warn("y700_dsi: slot %d no longer looks patched, not restoring\n", applied_slot);
		return;
	}
	for (i = 0; i < ENTRY_WORDS; i++)
		d[i] = saved[i];
	applied = 0;
	pr_info("y700_dsi: RESTORED slot %d (hfp=%u fps=%u)\n", applied_slot, d[3], d[11]);
}

static void apply_list(u32 *b)
{
	int n, s, i;
	u32 *d;
	const u32 *src;

	if (applied)
		return;
	if (hfp < 8 || hfp > 50 || new_fps < 166 || new_fps > 190) {
		pr_warn("y700_dsi: parameters out of range (hfp=%d new_fps=%d), not patching\n", hfp, new_fps);
		return;
	}
	n = count_entries(b);
	if (slot < 1 || slot >= n) {
		pr_warn("y700_dsi: slot %d invalid (list has %d entries), not patching\n", slot, n);
		return;
	}
	s = find_src(b, n);
	if (s < 0 || s == slot) {
		pr_warn("y700_dsi: 165Hz source entry not found or equals slot (src=%d slot=%d)\n", s, slot);
		return;
	}
	d = b + slot * ENTRY_WORDS;
	src = b + s * ENTRY_WORDS;
	if (!stock_fps(d[11])) {
		pr_warn("y700_dsi: slot %d has unexpected fps %u, not patching\n", slot, d[11]);
		return;
	}
	for (i = 0; i < ENTRY_WORDS; i++) {
		saved[i] = d[i];
		d[i] = src[i];
	}
	d[3] = 2 * hfp;
	d[11] = new_fps;
	applied = 1;
	applied_slot = slot;
	applied_hfp2 = d[3];
	applied_fps = d[11];
	pr_info("y700_dsi: NEW MODE in slot %d: clone of entry %d with hfp=%u fps=%u (old slot fps was %u)\n",
		slot, s, d[3], d[11], saved[11]);
}

static void dump_priv(const u32 *m, const char *tag)
{
	unsigned long priv = ((unsigned long)m[49] << 32) | m[48];
	const u32 *p;
	int j;

	if (!kptr_ok(priv))
		return;
	p = (const u32 *)priv;
	pr_info("y700_dsi: %s priv_info at %lx\n", tag, priv);
	for (j = 0; j < PRIV_DUMP_WORDS; j += 8)
		pr_info("y700_dsi: %s+%d: %u %u %u %u %u %u %u %u\n", tag, j,
			p[j], p[j + 1], p[j + 2], p[j + 3],
			p[j + 4], p[j + 5], p[j + 6], p[j + 7]);
}

static void dump_list(const u32 *b)
{
	int e, n = count_entries(b);

	for (e = 0; e < n; e++) {
		const u32 *m = b + e * ENTRY_WORDS;

		pr_info("y700_dsi: entry %d: hfp=%u vbp=%u vfp=%u fps=%u pclk_khz=%u clk=%u idx=%u\n",
			e, m[3], m[7], m[9], m[11], m[40], m[12], m[46]);
	}
	dump_priv(b, "priv0");
	dump_priv(b + ENTRY_WORDS, "priv1");
}

/* ---- set_mode logging ---- */
static int sm_pre(struct kprobe *p, struct pt_regs *regs)
{
	u32 *m = (u32 *)regs->regs[1];

	if (!kptr_ok((unsigned long)m))
		return 0;
	pr_info("y700_dsi: set_mode %ux%u hfp=%u vbp=%u vfp=%u fps=%u clk=%u\n",
		m[0], m[6], m[3], m[7], m[9], m[11], m[12]);
	return 0;
}

static struct kprobe kp_sm = {
	.symbol_name = "dsi_display_set_mode",
	.pre_handler = sm_pre,
};

/* ---- dsi_display_get_modes(display, &out_modes) ---- */
struct gm_ctx {
	unsigned long out;
};

static int gm_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	((struct gm_ctx *)ri->data)->out = regs->regs[1];
	return 0;
}

static int gm_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	unsigned long out = ((struct gm_ctx *)ri->data)->out, mp;
	u32 *b;

	if (!kptr_ok(out) || (out & 7))
		return 0;
	mp = *(unsigned long *)out;
	if (!kptr_ok(mp))
		return 0;
	b = (u32 *)mp;
	if (!entry_ok(b)) {
		pr_warn("y700_dsi: get_modes list at %lx failed layout check, ignoring\n", mp);
		return 0;
	}
	last_modes = b;

	if (patch_list)
		apply_list(b);
	else if (applied)
		restore_list(b);

	pr_info("y700_dsi: get_modes list=%lx entries=%d patch_list=%d applied=%d\n",
		mp, count_entries(b), patch_list, applied);
	if (dump)
		dump_list(b);
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
	pr_info("y700_dsi: v4 loaded (patch_list=%d slot=%d hfp=%d fps=%d; nothing changes until a connector probe)\n",
		patch_list, slot, hfp, new_fps);
	return 0;
}

static void __exit y700_dsi_exit(void)
{
	unregister_kretprobe(&krp_gm);
	unregister_kprobe(&kp_sm);
	if (applied && last_modes)
		restore_list(last_modes);
	pr_info("y700_dsi: unloaded (applied=%d)\n", applied);
}

module_init(y700_dsi_init);
module_exit(y700_dsi_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Y700 Gen4 DSI: add a new 170Hz mode via the master mode list (v4)");
