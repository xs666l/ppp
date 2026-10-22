// SPDX-License-Identifier: GPL-2.0
/*
 * y700_dsi - test module for Lenovo Legion Y700 Gen4 (SM8750, GKI 6.6)
 *
 * Hooks msm_drm's dsi_display_set_mode() with a kprobe.
 *
 *   patch=0 (default)  read-only: just logs the mode that is being set.
 *   patch=1            when the 165Hz mode is being set, rewrite its
 *                      h_front_porch and refresh rate before the driver uses it.
 *
 * Word layout of the mode struct (verified on this device by tracing):
 *   m[0]=h_active  m[1]=h_back_porch  m[2]=h_sync_width  m[3]=h_front_porch
 *   m[6]=v_active  m[7]=v_back_porch  m[8]=v_sync_width  m[9]=v_front_porch
 *   m[11]=refresh_rate  m[12]=clk_rate_hz
 * Both DSI links are driven together, so every horizontal value is 2x the
 * per-link value in the device tree (hfp 50 in dtbo -> m[3]=100).
 *
 * Parameters can be changed at runtime:
 *   /sys/module/y700_dsi/parameters/{patch,hfp,new_fps,src_fps}
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/moduleparam.h>

static int patch;
module_param(patch, int, 0644);
MODULE_PARM_DESC(patch, "0=log only, 1=rewrite the source mode (default 0)");

static int src_fps = 165;
module_param(src_fps, int, 0644);
MODULE_PARM_DESC(src_fps, "refresh rate of the mode to rewrite (default 165)");

static int new_fps = 170;
module_param(new_fps, int, 0644);
MODULE_PARM_DESC(new_fps, "refresh rate value written into the mode (166..190)");

static int hfp = 40;
module_param(hfp, int, 0644);
MODULE_PARM_DESC(hfp, "per-link horizontal front porch, dtbo units (8..50); m[3]=2*hfp");

static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	u32 *m = (u32 *)regs->regs[1];

	/* x1 must look like a kernel pointer (negative as signed on arm64) */
	if (!m || (s64)(unsigned long)m >= 0 || ((unsigned long)m & 3))
		return 0;

	pr_info("y700_dsi: set_mode %ux%u hbp=%u hsw=%u hfp=%u vbp=%u vsw=%u vfp=%u fps=%u clk=%u\n",
		m[0], m[6], m[1], m[2], m[3], m[7], m[8], m[9], m[11], m[12]);

	if (!patch)
		return 0;

	/* only touch the exact stock mode we expect: 1904x3040, hfp 50(x2), vbp 50, vfp 26 */
	if (m[0] != 1904 || m[6] != 3040 || m[11] != (u32)src_fps ||
	    m[3] != 100 || m[7] != 50 || m[9] != 26)
		return 0;

	if (hfp < 8 || hfp > 50 || new_fps < 166 || new_fps > 190) {
		pr_warn("y700_dsi: parameters out of range, not patching (hfp=%d new_fps=%d)\n",
			hfp, new_fps);
		return 0;
	}

	m[3] = 2 * hfp;
	m[11] = new_fps;
	pr_info("y700_dsi: PATCHED mode -> hfp=%u fps=%u\n", m[3], m[11]);
	return 0;
}

static struct kprobe kp = {
	.symbol_name = "dsi_display_set_mode",
	.pre_handler = handler_pre,
};

static int __init y700_dsi_init(void)
{
	int ret = register_kprobe(&kp);

	if (ret < 0) {
		pr_err("y700_dsi: register_kprobe failed: %d\n", ret);
		return ret;
	}
	pr_info("y700_dsi: loaded, probe at %p, patch=%d\n", kp.addr, patch);
	return 0;
}

static void __exit y700_dsi_exit(void)
{
	unregister_kprobe(&kp);
	pr_info("y700_dsi: unloaded\n");
}

module_init(y700_dsi_init);
module_exit(y700_dsi_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Y700 Gen4 DSI set_mode probe/patch test module");
