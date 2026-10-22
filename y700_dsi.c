// SPDX-License-Identifier: GPL-2.0
/*
 * y700_dsi v3 - Lenovo Legion Y700 Gen4 (SM8750) DSI mode-list tool.
 *
 * Facts verified on this device (from v2 dumps):
 *  - dsi_display_get_modes(display, &out) returns the driver's MASTER mode
 *    list (display->modes, at display+0x318). Entries are 200 bytes (50 u32):
 *      w0 h_active(1904)  w1 hbp(32)  w2 hsw(32)  w3 hfp   w6 v_active(3040)
 *      w7 vbp  w8 vsw  w9 vfp  w11 refresh  w12 clk  w40 pixel_clk_khz
 *      w46 mode index  w48/49 priv_info pointer
 *  - all modes share one pixel clock (345984 kHz, both links); refresh rate
 *    is set purely by the porches: fps = 172.992e6 / ((254+hbp+hsw+hfp)*vtotal)
 *    per link, with every horizontal word being 2x the per-link value.
 *
 * What this module does:
 *  - always: logs set_mode() calls.
 *  - patch_list=1 : the next time the connector is probed
 *        (echo detect > /sys/class/drm/card0-DSI-1/status)
 *    the stock 165Hz entry (hfp 50, vbp 50, vfp 26) in the master list is
 *    rewritten to hfp=<hfp>, refresh=<new_fps>, so the DRM modes AND the DSI
 *    host are generated from the same, consistent timing.
 *  - patch_list=0 : the next probe restores the original entry.
 *  - unloading the module restores the entry too.
 *
 * Be on 120Hz or 60Hz (NOT 165Hz) when you apply or restore.
 *
 *  /sys/module/y700_dsi/parameters/{patch_list,hfp,new_fps,dump}
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/moduleparam.h>

#define ENTRY_WORDS	50
#define MAX_ENTRIES	8
#define SRC_HFP2	100	/* stock 165Hz hfp, both links (2 x 50) */
#define SRC_FPS		165

static int patch_list;
module_param(patch_list, int, 0644);
MODULE_PARM_DESC(patch_list, "1 = rewrite the 165Hz list entry on next connector probe, 0 = restore");

static int hfp = 40;
module_param(hfp, int, 0644);
MODULE_PARM_DESC(hfp, "per-link horizontal front porch for the rewritten entry (8..50); 40=170Hz 29=176Hz 22=180Hz 14=185Hz");

static int new_fps = 170;
module_param(new_fps, int, 0644);
MODULE_PARM_DESC(new_fps, "refresh rate label written into the entry (166..190)");

static int dump;
module_param(dump, int, 0644);
MODULE_PARM_DESC(dump, "1 = also dump the whole list on each probe");

static u32 *last_modes;		/* master list, remembered for restore on unload */
static int applied;		/* 1 while the list entry is currently patched */
static u32 applied_hfp2, applied_fps;

static inline bool kptr_ok(unsigned long p)
{
	return (s64)p < 0 && !(p & 3);
}

/* A list entry is valid only if it passes all of these (stride/layout check). */
static bool entry_ok(const u32 *m, int idx)
{
	unsigned long priv = ((unsigned long)m[49] << 32) | m[48];

	return m[0] == 1904 && m[1] == 32 && m[2] == 32 && m[6] == 3040 &&
	       m[46] == (u32)idx && kptr_ok(priv);
}

static void restore_list(u32 *b)
{
	int e;

	for (e = 0; e < MAX_ENTRIES; e++) {
		u32 *m = b + e * ENTRY_WORDS;

		if (!entry_ok(m, e))
			break;
		if (applied && m[3] == applied_hfp2 && m[11] == applied_fps &&
		    m[7] == 50 && m[9] == 26) {
			m[3] = SRC_HFP2;
			m[11] = SRC_FPS;
			applied = 0;
			pr_info("y700_dsi: RESTORED list entry %d -> hfp=%u fps=%u\n", e, m[3], m[11]);
		}
	}
}

static void apply_list(u32 *b)
{
	int e;

	if (hfp < 8 || hfp > 50 || new_fps < 166 || new_fps > 190) {
		pr_warn("y700_dsi: parameters out of range (hfp=%d new_fps=%d), not patching\n", hfp, new_fps);
		return;
	}
	for (e = 0; e < MAX_ENTRIES; e++) {
		u32 *m = b + e * ENTRY_WORDS;

		if (!entry_ok(m, e))
			break;
		if (!applied && m[3] == SRC_HFP2 && m[7] == 50 && m[9] == 26 &&
		    m[11] == SRC_FPS) {
			m[3] = 2 * hfp;
			m[11] = new_fps;
			applied = 1;
			applied_hfp2 = m[3];
			applied_fps = m[11];
			pr_info("y700_dsi: PATCHED list entry %d -> hfp=%u fps=%u\n", e, m[3], m[11]);
		}
	}
}

static void dump_list(const u32 *b)
{
	int e;

	for (e = 0; e < MAX_ENTRIES; e++) {
		const u32 *m = b + e * ENTRY_WORDS;

		if (!entry_ok(m, e))
			break;
		pr_info("y700_dsi: entry %d: hfp=%u vbp=%u vfp=%u fps=%u pclk_khz=%u idx=%u\n",
			e, m[3], m[7], m[9], m[11], m[40], m[46]);
	}
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
	if (!entry_ok(b, 0)) {
		pr_warn("y700_dsi: get_modes list at %lx failed layout check, ignoring\n", mp);
		return 0;
	}
	last_modes = b;

	if (patch_list)
		apply_list(b);
	else if (applied)
		restore_list(b);

	pr_info("y700_dsi: get_modes list=%lx patch_list=%d applied=%d\n", mp, patch_list, applied);
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
	pr_info("y700_dsi: v3 loaded (patch_list=%d, nothing changed until a connector probe)\n", patch_list);
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
MODULE_DESCRIPTION("Y700 Gen4 DSI master mode list tool (v3)");
