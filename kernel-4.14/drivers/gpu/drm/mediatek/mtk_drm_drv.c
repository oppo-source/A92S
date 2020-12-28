/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: YT SHEN <yt.shen@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_cma_helper.h>
#include <linux/component.h>
#include <linux/iommu.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>

#include "drm_internal.h"
#include "mtk_drm_crtc.h"
#include "mtk_drm_ddp.h"
#include "mtk_drm_ddp_comp.h"
#include "mtk_drm_debugfs.h"
#include "mtk_drm_drv.h"
#include "mtk_drm_fb.h"
#include "mtk_drm_fbdev.h"
#include "mtk_drm_gem.h"
#include "mtk_drm_session.h"
#include "mtk_fence.h"
#include "mtk_debug.h"
#include "mtk_layering_rule.h"
#include "mtk_rect.h"
#include "mtk_drm_helper.h"
#include "mtk_drm_assert.h"
#include "mtk_drm_graphics_base.h"
#include "mtk_panel_ext.h"
#include <linux/clk.h>
#include "mtk_disp_pmqos.h"
#include "mtk_disp_recovery.h"
#include "mtk_drm_arr.h"
#include "mtk_disp_ccorr.h"
#include "mtk_disp_color.h"
#include "mtk_disp_gamma.h"
#include "mtk_disp_aal.h"
#include "mtk_drm_mmp.h"
#ifdef VENDOR_EDIT
/* Yongpeng.Yi@PSW.MultiMedia.Display.LCD.Feature, 2018/09/10, Add for sau and silence close backlight */
#include <mt-plat/mtk_boot_common.h>
extern unsigned long silence_mode;
#endif


#define DRIVER_NAME "mediatek"
#define DRIVER_DESC "Mediatek SoC DRM"
#define DRIVER_DATE "20150513"
#define DRIVER_MAJOR 1
#define DRIVER_MINOR 0

atomic_t _mtk_fence_idx = ATOMIC_INIT(-1);
atomic_t _mtk_fence_update_event = ATOMIC_INIT(0);
wait_queue_head_t _mtk_fence_wq;

static atomic_t top_isr_ref; /* irq power status protection */
static atomic_t top_clk_ref; /* top clk status protection*/
static spinlock_t top_clk_lock; /* power status protection*/

unsigned long long mutex_time_start;
unsigned long long mutex_time_end;
long long mutex_time_period;
const char *mutex_locker;

unsigned long long mutex_nested_time_start;
unsigned long long mutex_nested_time_end;
long long mutex_nested_time_period;
const char *mutex_nested_locker;

struct lcm_fps_ctx_t lcm_fps_ctx[MAX_CRTC];

static inline struct mtk_atomic_state *to_mtk_state(struct drm_atomic_state *s)
{
	return container_of(s, struct mtk_atomic_state, base);
}

static void mtk_atomic_state_free(struct kref *k)
{
	struct mtk_atomic_state *mtk_state =
		container_of(k, struct mtk_atomic_state, kref);
	struct drm_atomic_state *state = &mtk_state->base;

	drm_atomic_state_clear(state);
	drm_atomic_state_default_release(state);
	kfree(mtk_state);
}

void mtk_atomic_state_get(struct drm_atomic_state *state)
{
	struct mtk_atomic_state *mtk_state = to_mtk_state(state);

	kref_get(&mtk_state->kref);
	kref_get(&state->ref);
}

void mtk_atomic_state_put(struct drm_atomic_state *state)
{
	struct mtk_atomic_state *mtk_state = to_mtk_state(state);

	drm_atomic_state_put(state);
	kref_put(&mtk_state->kref, mtk_atomic_state_free);
}

void mtk_atomic_state_put_queue(struct drm_atomic_state *state)
{
	struct drm_device *drm = state->dev;
	struct mtk_drm_private *mtk_drm = drm->dev_private;
	struct mtk_atomic_state *mtk_state = to_mtk_state(state);
	unsigned long flags;

	spin_lock_irqsave(&mtk_drm->unreference.lock, flags);
	list_add_tail(&mtk_state->list, &mtk_drm->unreference.list);
	spin_unlock_irqrestore(&mtk_drm->unreference.lock, flags);

	schedule_work(&mtk_drm->unreference.work);
}

static uint32_t mtk_atomic_crtc_mask(struct drm_device *drm,
				     struct drm_atomic_state *state)
{
	uint32_t crtc_mask = 0;
	int i;

	for (i = 0, crtc_mask = 0; i < drm->mode_config.num_crtc; i++) {
		struct __drm_crtcs_state *crtc = &(state->crtcs[i]);

		if (!crtc->ptr)
			continue;

		crtc_mask |= (1 << drm_crtc_index(crtc->ptr));
	}

	return crtc_mask;
}

static void mtk_unreference_work(struct work_struct *work)
{
	struct mtk_drm_private *mtk_drm =
		container_of(work, struct mtk_drm_private, unreference.work);
	unsigned long flags;
	struct mtk_atomic_state *state, *tmp;

	/*
	 * framebuffers cannot be unreferenced in atomic context.
	 * Therefore, only hold the spinlock when iterating unreference_list,
	 * and drop it when doing the unreference.
	 */
	spin_lock_irqsave(&mtk_drm->unreference.lock, flags);
	list_for_each_entry_safe(state, tmp, &mtk_drm->unreference.list, list) {
		list_del(&state->list);
		spin_unlock_irqrestore(&mtk_drm->unreference.lock, flags);
		mtk_atomic_state_put(&state->base);
		spin_lock_irqsave(&mtk_drm->unreference.lock, flags);
	}
	spin_unlock_irqrestore(&mtk_drm->unreference.lock, flags);
}

static void mtk_atomic_schedule(struct mtk_drm_private *private,
				struct drm_atomic_state *state)
{
	private->commit.state = state;
	schedule_work(&private->commit.work);
}

static void mtk_atomic_wait_for_fences(struct drm_atomic_state *state)
{
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	int i;

	for_each_plane_in_state(state, plane, plane_state, i)
		mtk_fb_wait(plane->state->fb);
}

static void mtk_atomic_disp_rsz_roi(struct drm_device *dev,
				    struct drm_atomic_state *old_state)
{
	struct drm_plane *plane;
	struct drm_plane_state *old_plane_state;
	struct drm_crtc *crtc = NULL;
	struct drm_crtc_state *old_crtc_state;
	int i;
	struct mtk_rect dst_layer_roi = {0};
	struct mtk_rect dst_total_roi[MAX_CRTC] = {0};
	struct mtk_rect src_layer_roi = {0};
	struct mtk_rect src_total_roi[MAX_CRTC] = {0};
	bool rsz_enable[MAX_CRTC] = {false};
	struct mtk_plane_comp_state comp_state[MAX_CRTC][OVL_LAYER_NR];

	for_each_crtc_in_state(old_state, crtc, old_crtc_state, i) {
		struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);

		for (i = 0 ; i < mtk_crtc->layer_nr; i++) {
			struct drm_plane *plane = &mtk_crtc->planes[i].base;

			mtk_plane_get_comp_state(
				plane, &comp_state[drm_crtc_index(crtc)][i],
				crtc, 1);
		}
	}

	for_each_plane_in_state(old_state, plane, old_plane_state, i) {
		struct drm_plane_state *plane_state = plane->state;
		int src_w = drm_rect_width(&plane_state->src) >> 16;
		int src_h = drm_rect_height(&plane_state->src) >> 16;
		int dst_x = plane_state->dst.x1;
		int dst_y = plane_state->dst.y1;
		int dst_w = drm_rect_width(&plane_state->dst);
		int dst_h = drm_rect_height(&plane_state->dst);
		int idx;

		if (!plane_state->crtc)
			continue;

		if (i < OVL_LAYER_NR)
			idx = i;
		else if (i < (OVL_LAYER_NR + EXTERNAL_INPUT_LAYER_NR))
			idx = i - OVL_LAYER_NR;
		else
			idx = i - (OVL_LAYER_NR + EXTERNAL_INPUT_LAYER_NR);

		if (comp_state[drm_crtc_index(crtc)][idx].layer_caps
			& MTK_DISP_RSZ_LAYER) {
			mtk_rect_make(&src_layer_roi,
				((dst_x * src_w * 10) / dst_w + 5) / 10,
				((dst_y * src_h * 10) / dst_h + 5) / 10,
				src_w, src_h);
			mtk_rect_make(&dst_layer_roi,
				dst_x, dst_y,
				dst_w, dst_h);
			mtk_rect_join(&src_layer_roi,
				      &src_total_roi[drm_crtc_index(
					      plane_state->crtc)],
				      &src_total_roi[drm_crtc_index(
					      plane_state->crtc)]);
			mtk_rect_join(&dst_layer_roi,
				      &dst_total_roi[drm_crtc_index(
					      plane_state->crtc)],
				      &dst_total_roi[drm_crtc_index(
					      plane_state->crtc)]);
			rsz_enable[drm_crtc_index(plane_state->crtc)] = true;
		}
	}

	for_each_crtc_in_state(old_state, crtc, old_crtc_state, i) {
		struct mtk_crtc_state *state = to_mtk_crtc_state(crtc->state);
		struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);

		if (!rsz_enable[i] || mtk_crtc->fake_layer.fake_layer_mask) {
			src_total_roi[i].x = 0;
			src_total_roi[i].y = 0;
			src_total_roi[i].width =
				crtc->state->adjusted_mode.hdisplay;
			src_total_roi[i].height =
					crtc->state->adjusted_mode.vdisplay;
			dst_total_roi[i].x = 0;
			dst_total_roi[i].y = 0;
			dst_total_roi[i].width =
				crtc->state->adjusted_mode.hdisplay;
			dst_total_roi[i].height =
				crtc->state->adjusted_mode.vdisplay;
		}
		state->rsz_src_roi = src_total_roi[i];
		state->rsz_dst_roi = dst_total_roi[i];
		DDPINFO("[RPO] crtc[%d] (%d,%d,%d,%d)->(%d,%d,%d,%d)\n",
			drm_crtc_index(crtc), src_total_roi[i].x,
			src_total_roi[i].y, src_total_roi[i].width,
			src_total_roi[i].height, dst_total_roi[i].x,
			dst_total_roi[i].y, dst_total_roi[i].width,
			dst_total_roi[i].height);
	}
}

static void
mtk_atomic_calculate_plane_enabled_number(struct drm_device *dev,
					  struct drm_atomic_state *old_state)
{
	int i;
	struct drm_plane *plane;
	struct drm_plane_state *old_plane_state;
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	unsigned int cnt[MAX_CRTC] = {0};

	for_each_plane_in_state(old_state, plane, old_plane_state, i) {
		if (plane->state->crtc) {
			struct mtk_drm_crtc *mtk_crtc =
					to_mtk_crtc(plane->state->crtc);
			cnt[drm_crtc_index(&mtk_crtc->base)]++;
		}
	}

	for_each_crtc_in_state(old_state, crtc, old_crtc_state, i) {
		struct mtk_crtc_state *state = to_mtk_crtc_state(crtc->state);

		atomic_set(&state->plane_enabled_num, cnt[i]);
	}
}

static void mtk_atomic_check_plane_sec_state(struct drm_device *dev,
				     struct drm_atomic_state *new_state)
{
	int i;
	int sec_on[MAX_CRTC] = {0};
	struct drm_plane *plane;
	struct drm_plane_state *new_plane_state;
#if defined(CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)
	struct drm_crtc *crtc;
	struct drm_crtc_state *new_crtc_state;
#endif
	for_each_plane_in_state(new_state, plane, new_plane_state, i) {
		if (plane->state->crtc) {
			struct mtk_drm_crtc *mtk_crtc =
					to_mtk_crtc(plane->state->crtc);

			if (plane->state->fb
				&& plane->state->fb->pixel_format
					!= DRM_FORMAT_C8
				&& mtk_drm_fb_is_secure(plane->state->fb))
				sec_on[drm_crtc_index(&mtk_crtc->base)] = true;
		}
	}

#if defined(CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)
	for_each_crtc_in_state(new_state, crtc, new_crtc_state, i) {
		struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);

		/* Leave secure sequence */
		if (mtk_crtc->sec_on && !sec_on[i])
			mtk_crtc_disable_secure_state(&mtk_crtc->base);

		/* When the engine switch to secure, we don't let system
		 * enter LP idle mode. Because it may make more secure risks
		 * with tiny power benefit.
		 */
		if (!mtk_crtc->sec_on && sec_on[i])
			mtk_drm_idlemgr_kick(__func__, crtc, false);

		mtk_crtc->sec_on = sec_on[i];
	}
#endif
}

static bool mtk_atomic_need_force_doze_switch(struct drm_crtc *crtc)
{
	struct mtk_crtc_state *mtk_state;

	mtk_state = to_mtk_crtc_state(crtc->state);
	if (!mtk_state->doze_changed ||
	    drm_atomic_crtc_needs_modeset(crtc->state))
		return false;

	DDPINFO("%s crtc%d, active:%d, doze_active:%d\n", __func__,
		drm_crtc_index(crtc), crtc->state->active,
		mtk_state->prop_val[CRTC_PROP_DOZE_ACTIVE]);
	return true;
}

#ifndef VENDOR_EDIT
/* liwei.a@PSW.MM.DisplayDriver.AOD, 2019/11/15, modify for AOD*/
static void mtk_atomic_force_doze_switch(struct drm_device *dev,
					 struct drm_atomic_state *old_state,
					 struct drm_connector *connector,
					 struct drm_crtc *crtc)
{
	const struct drm_encoder_helper_funcs *funcs;
	struct drm_encoder *encoder;
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_panel_funcs *panel_funcs;
	struct cmdq_pkt *handle;
	struct cmdq_client *client = mtk_crtc->gce_obj.client[CLIENT_CFG];

	/*
	 * If CRTC doze_active state change but the active state
	 * keep with the same value, the enable/disable cb of the
	 * encorder would not be executed. So we have to force
	 * run those cbs to change the doze directly.
	 */
	if (!mtk_atomic_need_force_doze_switch(crtc))
		return;

	DDPINFO("%s\n", __func__);

	encoder = connector->state->best_encoder;
	funcs = encoder->helper_private;

	panel_funcs = mtk_drm_get_lcm_ext_funcs(crtc);
	if (panel_funcs && panel_funcs->doze_get_mode_flags) {
		/* blocking flush before stop trigger loop */
		mtk_crtc_pkt_create(&handle, &mtk_crtc->base,
			mtk_crtc->gce_obj.client[CLIENT_CFG]);
		if (mtk_crtc_is_frame_trigger_mode(crtc))
			cmdq_pkt_wait_no_clear(handle,
				mtk_crtc->gce_obj.event[EVENT_STREAM_EOF]);
		else
			cmdq_pkt_wait_no_clear(handle,
				mtk_crtc->gce_obj.event[EVENT_VDO_EOF]);
		cmdq_pkt_flush(handle);
		cmdq_pkt_destroy(handle);

		cmdq_mbox_enable(client->chan); /* GCE clk refcnt + 1 */
		mtk_crtc_stop_trig_loop(crtc);
#if defined(CONFIG_MACH_MT6873)
		if (!mtk_crtc_is_frame_trigger_mode(crtc))
			mtk_crtc_stop_sodi_loop(crtc);
#endif
		if (mtk_crtc_is_frame_trigger_mode(crtc)) {
			mtk_disp_mutex_disable(mtk_crtc->mutex[0]);
			mtk_disp_mutex_src_set(mtk_crtc, false);
			mtk_disp_mutex_enable(mtk_crtc->mutex[0]);
		}
	}
	/*
	 * No matter what the target crct power state it is,
	 * the encorder should be enabled for register controlling
	 * purpose.
	 */
	if (!funcs)
		return;
	if (funcs->enable)
		funcs->enable(encoder);
	else if (funcs->commit)
		funcs->commit(encoder);

	/*
	 * If the target crtc power state is POWER_OFF or
	 * DOZE_SUSPEND, call the encorder disable cb here.
	 */
	if (!crtc->state->active) {
		if (connector->state->crtc && funcs->prepare)
			funcs->prepare(encoder);
		else if (funcs->disable)
			funcs->disable(encoder);
		else if (funcs->dpms)
			funcs->dpms(encoder, DRM_MODE_DPMS_OFF);
	}

	if (panel_funcs && panel_funcs->doze_get_mode_flags) {
		if (mtk_crtc_is_frame_trigger_mode(crtc)) {
			mtk_disp_mutex_disable(mtk_crtc->mutex[0]);
			mtk_disp_mutex_src_set(mtk_crtc, true);
		}
#if defined(CONFIG_MACH_MT6873)
		if (!mtk_crtc_is_frame_trigger_mode(crtc))
			mtk_crtc_start_sodi_loop(crtc);
#endif
		mtk_crtc_start_trig_loop(crtc);
		cmdq_mbox_disable(client->chan); /* GCE clk refcnt - 1 */

		mtk_crtc_hw_block_ready(crtc);
	}
}
#else
static void mtk_atomic_force_doze_switch(struct drm_device *dev,
					 struct drm_atomic_state *old_state,
					 struct drm_connector *connector,
					 struct drm_crtc *crtc)
{
	const struct drm_encoder_helper_funcs *funcs;
	struct drm_encoder *encoder;
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_panel_funcs *panel_funcs;
	#ifdef VENDOR_EDIT
	/* liwei.a@PSW.MM.DisplayDriver.Stability, 2019/12/05, add for cmdq timeout issue*/
	struct cmdq_pkt *cmdq_handle, *cmdq_handle2, *cmdq_handle3;
	#endif

	mtk_drm_idlemgr_kick(__func__, crtc, false);
	/*
	 * If CRTC doze_active state change but the active state
	 * keep with the same value, the enable/disable cb of the
	 * encorder would not be executed. So we have to force
	 * run those cbs to change the doze directly.
	 */
	if (!mtk_atomic_need_force_doze_switch(crtc))
		return;

	DDPINFO("%s\n", __func__);

	encoder = connector->state->best_encoder;
	funcs = encoder->helper_private;

	#ifdef VENDOR_EDIT
	/* liwei.a@PSW.MM.DisplayDriver.Stability, 2019/12/05, add for cmdq timeout issue*/
	/* add blocking flush */
	mtk_crtc_pkt_create(&cmdq_handle, &mtk_crtc->base,
			mtk_crtc->gce_obj.client[CLIENT_CFG]);
	cmdq_pkt_flush(cmdq_handle);
	cmdq_pkt_destroy(cmdq_handle);

	mtk_crtc_pkt_create(&cmdq_handle2, &mtk_crtc->base,
			mtk_crtc->gce_obj.client[CLIENT_DSI_CFG]);
	cmdq_pkt_flush(cmdq_handle2);
	cmdq_pkt_destroy(cmdq_handle2);
	#endif
	#ifdef VENDOR_EDIT
	/* zhangjianbin@PSW.MM.DisplayDriver.AOD, 2020/1/22, add for cmd aod*/
	panel_funcs = mtk_drm_get_lcm_ext_funcs(crtc);
	if (panel_funcs && panel_funcs->doze_get_mode_flags) {
		DDPINFO("%s 1 trigger mode %d\n", __func__,
			mtk_crtc_is_frame_trigger_mode(crtc));
		if (mtk_crtc_is_frame_trigger_mode(crtc)) {
			mtk_crtc_pkt_create(&cmdq_handle3, &mtk_crtc->base,
				mtk_crtc->gce_obj.client[CLIENT_CFG]);
			cmdq_pkt_wait_no_clear(cmdq_handle3,
				mtk_crtc->gce_obj.event[EVENT_STREAM_EOF]);
			cmdq_pkt_clear_event(cmdq_handle3,
				mtk_crtc->gce_obj.event[EVENT_STREAM_BLOCK]);
			cmdq_pkt_flush(cmdq_handle3);
			cmdq_pkt_destroy(cmdq_handle3);
		}
		mtk_crtc_stop_trig_loop(crtc);
		if (mtk_crtc_is_frame_trigger_mode(crtc)) {
			mtk_disp_mutex_disable(mtk_crtc->mutex[0]);
			mtk_disp_mutex_src_set(mtk_crtc, false);
			mtk_disp_mutex_enable(mtk_crtc->mutex[0]);
		}
	} else {
		if(panel_funcs && panel_funcs->panel_no_cv_switch){
			if (mtk_crtc_is_frame_trigger_mode(crtc)) {
				mtk_crtc_pkt_create(&cmdq_handle3, &mtk_crtc->base,
					mtk_crtc->gce_obj.client[CLIENT_CFG]);
				cmdq_pkt_wait_no_clear(cmdq_handle3,
					mtk_crtc->gce_obj.event[EVENT_STREAM_EOF]);
				cmdq_pkt_clear_event(cmdq_handle3,
					mtk_crtc->gce_obj.event[EVENT_STREAM_BLOCK]);
				cmdq_pkt_flush(cmdq_handle3);
				cmdq_pkt_destroy(cmdq_handle3);
			}
			mtk_crtc_stop_trig_loop(crtc);
			if (mtk_crtc_is_frame_trigger_mode(crtc)) {
				mtk_disp_mutex_disable(mtk_crtc->mutex[0]);
				mtk_disp_mutex_src_set(mtk_crtc, false);
				mtk_disp_mutex_enable(mtk_crtc->mutex[0]);
			}
		}
	}
  	#endif
	/*
	 * No matter what the target crct power state it is,
	 * the encorder should be enabled for register controlling
	 * purpose.
	 */
	if (!funcs)
		return;
	if (funcs->enable)
		funcs->enable(encoder);
	else if (funcs->commit)
		funcs->commit(encoder);

	/*
	 * If the target crtc power state is POWER_OFF or
	 * DOZE_SUSPEND, call the encorder disable cb here.
	 */
	if (!crtc->state->active) {
		if (connector->state->crtc && funcs->prepare)
			funcs->prepare(encoder);
		else if (funcs->disable)
			funcs->disable(encoder);
		else if (funcs->dpms)
			funcs->dpms(encoder, DRM_MODE_DPMS_OFF);
	}
	#ifdef VENDOR_EDIT
	/* zhangjianbin@PSW.MM.DisplayDriver.AOD, 2020/1/22, add for cmd aod*/
	if (panel_funcs && panel_funcs->doze_get_mode_flags) {
		DDPINFO("%s 2 trigger mode %d\n", __func__,
			mtk_crtc_is_frame_trigger_mode(crtc));
		if (mtk_crtc_is_frame_trigger_mode(crtc)) {
			mtk_disp_mutex_rst(mtk_crtc->mutex[0]);
			mtk_disp_mutex_disable(mtk_crtc->mutex[0]);
			mtk_disp_mutex_src_set(mtk_crtc, true);
		}
		mtk_crtc_start_trig_loop(crtc);
		mtk_crtc_hw_block_ready(crtc);
	} else {
		if(panel_funcs && panel_funcs->panel_no_cv_switch){
			if (mtk_crtc_is_frame_trigger_mode(crtc)) {
				mtk_disp_mutex_rst(mtk_crtc->mutex[0]);
				mtk_disp_mutex_disable(mtk_crtc->mutex[0]);
				mtk_disp_mutex_src_set(mtk_crtc, true);
			}
				mtk_crtc_start_trig_loop(crtc);
				mtk_crtc_hw_block_ready(crtc);
		}
	 }
	#endif
	mtk_drm_idlemgr_kick(__func__, crtc, false);
}
#endif

static void mtk_atomic_doze_update_dsi_state(struct drm_device *dev,
					 struct drm_crtc *crtc, bool prepare)
{
	struct mtk_crtc_state *mtk_state;
	#ifdef VENDOR_EDIT
	/* zhangjianbin@PSW.MultiMedia.Display.LCD, 2020/1/15, Add for cmd aod */
	struct mtk_panel_funcs *panel_funcs;
	panel_funcs = mtk_drm_get_lcm_ext_funcs(crtc);
	if((panel_funcs&&panel_funcs->panel_no_cv_switch)){
		return ;
	}
	#endif
	mtk_state = to_mtk_crtc_state(crtc->state);
	DDPINFO("%s doze_changed:%d, needs_modeset:%d, doze_active:%d\n",
		__func__, mtk_state->doze_changed,
		drm_atomic_crtc_needs_modeset(crtc->state),
		mtk_state->prop_val[CRTC_PROP_DOZE_ACTIVE]);

	if (!mtk_state->doze_changed ||
		!drm_atomic_crtc_needs_modeset(crtc->state))
		return;

	#ifdef VENDOR_EDIT
	/* liwei.a@PSW.MM.DisplayDriver.Stability, 2019/12/07, modify to fix cmdq timeout from doze to off*/
	/* do not have to change output mode when enter suspend*/
	if((!crtc->state->active) &&
		(!mtk_state->prop_val[CRTC_PROP_DOZE_ACTIVE])) {
		return;
	}
	#endif

	/* consider suspend->doze, doze_suspend->resume when doze prepare
	 * consider doze->suspend, resume->doze_supend when doze finish
	 */
	if ((crtc->state->active && prepare) ||
		(!crtc->state->active && !prepare))
		mtk_crtc_change_output_mode(crtc,
			mtk_state->prop_val[CRTC_PROP_DOZE_ACTIVE]);
}

static void mtk_atomic_doze_preparation(struct drm_device *dev,
					 struct drm_atomic_state *old_state)
{
	struct drm_crtc *crtc;
	struct drm_connector *connector;
	struct drm_connector_state *old_conn_state;
	int i;

	for_each_new_connector_in_state(old_state, connector,
		old_conn_state, i) {

		crtc = connector->state->crtc;
		if (!crtc) {
			DDPPR_ERR("%s connector has no crtc\n", __func__);
			continue;
		}

		mtk_atomic_doze_update_dsi_state(dev, crtc, 1);

		mtk_atomic_force_doze_switch(dev, old_state, connector, crtc);
	}

}

static void mtk_atomic_doze_finish(struct drm_device *dev,
					 struct drm_atomic_state *old_state)
{
	struct drm_crtc *crtc;
	struct drm_connector *connector;
	struct drm_connector_state *old_conn_state;
	int i;

	for_each_new_connector_in_state(old_state, connector,
		old_conn_state, i) {

		crtc = connector->state->crtc;
		if (!crtc) {
			DDPPR_ERR("%s connector has no crtc\n", __func__);
			continue;
		}

		mtk_atomic_doze_update_dsi_state(dev, crtc, 0);
	}

}

static bool mtk_drm_is_enable_from_lk(struct drm_crtc *crtc)
{
	/* TODO: check if target CRTC has been turn on in LK */
	if (drm_crtc_index(crtc) == 0)
		return true;
	return false;
}

void mtk_drm_set_lcm_disp(struct drm_crtc *crtc,enum mtk_ddp_io_cmd io_cmd)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *comp;

	comp = mtk_ddp_comp_request_output(mtk_crtc);
	if (comp)
		mtk_ddp_comp_io_cmd(comp, NULL, io_cmd, NULL);
}

static bool mtk_atomic_skip_plane_update(struct mtk_drm_private *private,
					 struct drm_atomic_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	int i;

	/* If the power state goes to or leave to doze mode, the display
	 * engine should not be self-refresh it again to avoid updating the
	 * unexpected content. The middleware is only change one CRTC
	 * power state at a time and there has no new frame updating
	 * when power state change. So here we can traverse all the
	 * CRTC state and skip the whole frame update. If the behavior
	 * above changed, the statement below should be modified.
	 */
	for_each_crtc_in_state(state, crtc, old_crtc_state, i) {
		struct mtk_crtc_state *mtk_state =
			to_mtk_crtc_state(crtc->state);
		if (mtk_state->doze_changed ||
			(drm_atomic_crtc_needs_modeset(crtc->state) &&
			mtk_state->prop_val[CRTC_PROP_DOZE_ACTIVE])) {
			DDPINFO("%s doze changed, skip self-update\n",
				__func__);
			return true;
		}
	}

	/* The CRTC would be enabled in LK stage and the content of
	 * corresponding display
	 *  may modified unexpected when first set crtc. If the case occur, we
	 * skip the plane
	 * update to make sure the display correctly.
	 */
	if (!state->legacy_set_config)
		return false;

	/* In general case, the set crtc stage control one crtc only. If the
	 * multiple CRTCs set
	 * at the same time, we have to make sure all the CRTC has been enabled
	 * from LK, then
	 * skip the plane update.
	 */
	for_each_crtc_in_state(state, crtc, old_crtc_state, i) {
		if (!mtk_drm_is_enable_from_lk(crtc))
			return false;
	}

	return true;
}

#ifdef MTK_DRM_ESD_SUPPORT
static void drm_atomic_esd_chk_first_enable(struct drm_device *dev,
				     struct drm_atomic_state *old_state)
{
	static bool is_first = true;
	int i;
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;


	if (is_first) {
		for_each_crtc_in_state(old_state, crtc, old_crtc_state, i) {
			if (drm_crtc_index(crtc) == 0) {
				if  (mtk_drm_lcm_is_connect())
					mtk_disp_esd_check_switch(crtc, true);
				break;
			}
		}

		is_first = false;
	}
}
#endif

static void mtk_drm_enable_trig(struct drm_device *drm,
		struct drm_atomic_state *old_state)
{
	struct drm_connector *connector;
	struct drm_connector_state *new_conn_state;
	int i;

	for_each_new_connector_in_state(old_state, connector,
				new_conn_state, i) {
		if (!new_conn_state->best_encoder)
			continue;

		if (!new_conn_state->crtc->state->active ||
		    !drm_atomic_crtc_needs_modeset(new_conn_state->crtc->state))
			continue;

		mtk_crtc_hw_block_ready(new_conn_state->crtc);
	}
}

static void mtk_atomic_complete(struct mtk_drm_private *private,
				struct drm_atomic_state *state)
{
	struct drm_device *drm = private->drm;

	mtk_atomic_wait_for_fences(state);

	/*
	 * Mediatek drm supports runtime PM, so plane registers cannot be
	 * written when their crtc is disabled.
	 *
	 * The comment for drm_atomic_helper_commit states:
	 *     For drivers supporting runtime PM the recommended sequence is
	 *
	 *     drm_atomic_helper_commit_modeset_disables(dev, state);
	 *     drm_atomic_helper_commit_modeset_enables(dev, state);
	 *     drm_atomic_helper_commit_planes(dev, state,
	 *                                     DRM_PLANE_COMMIT_ACTIVE_ONLY);
	 *
	 * See the kerneldoc entries for these three functions for more details.
	 */
	/*
	 * To change the CRTC doze state, call the encorder enable/disable
	 * directly if the actvie state doesn't change.
	 */
	mtk_atomic_doze_preparation(drm, state);

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_modeset_enables(drm, state);

	mtk_drm_enable_trig(drm, state);


	mtk_atomic_disp_rsz_roi(drm, state);

	mtk_atomic_calculate_plane_enabled_number(drm, state);

	mtk_atomic_check_plane_sec_state(drm, state);

	if (!mtk_atomic_skip_plane_update(private, state)) {
		drm_atomic_helper_commit_planes(drm, state,
						DRM_PLANE_COMMIT_ACTIVE_ONLY);
#ifdef MTK_DRM_ESD_SUPPORT
		drm_atomic_esd_chk_first_enable(drm, state);
#endif
	}

	mtk_atomic_doze_finish(drm, state);

	if (!mtk_drm_helper_get_opt(private->helper_opt,
				    MTK_DRM_OPT_COMMIT_NO_WAIT_VBLANK))
		drm_atomic_helper_wait_for_vblanks(drm, state);

	drm_atomic_helper_cleanup_planes(drm, state);
	mtk_atomic_state_put(state);
}

static void mtk_atomic_work(struct work_struct *work)
{
	struct mtk_drm_private *private =
		container_of(work, struct mtk_drm_private, commit.work);

	mtk_atomic_complete(private, private->commit.state);
}

static int mtk_atomic_check(struct drm_device *dev,
			    struct drm_atomic_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct mtk_crtc_state *old_state, *new_state;
	int i, ret = 0;

	ret = drm_atomic_helper_check(dev, state);
	if (ret)
		return ret;

	for_each_crtc_in_state(state, crtc, crtc_state, i) {
		old_state = to_mtk_crtc_state(crtc->state);
		new_state = to_mtk_crtc_state(crtc_state);

		if (old_state->prop_val[CRTC_PROP_DOZE_ACTIVE] ==
		    new_state->prop_val[CRTC_PROP_DOZE_ACTIVE])
			continue;
		DDPINFO("[CRTC:%d:%s] doze active changed\n", crtc->base.id,
				crtc->name);
		new_state->doze_changed = true;
		ret = drm_atomic_add_affected_connectors(state, crtc);
		if (ret) {
			DDPINFO("DRM add conn failed! state:%p, ret:%d\n",
					state, ret);
			return ret;
		}
	}

	return ret;
}

static int mtk_atomic_commit(struct drm_device *drm,
			     struct drm_atomic_state *state, bool async)
{
	struct mtk_drm_private *private = drm->dev_private;
	uint32_t crtc_mask;
	struct drm_crtc *crtc;
	struct mtk_drm_crtc *mtk_crtc;
	int ret, i = 0;
	int index;

	ret = drm_atomic_helper_prepare_planes(drm, state);
	if (ret)
		return ret;

	mutex_lock(&private->commit.lock);
	flush_work(&private->commit.work);

	crtc_mask = mtk_atomic_crtc_mask(drm, state);

	DRM_MMP_EVENT_START(mutex_lock, 0, 0);
	for (i = 0; i < MAX_CRTC; i++) {
		if (!(crtc_mask >> i & 0x1))
			continue;

		crtc = private->crtc[i];
		mtk_crtc = to_mtk_crtc(crtc);
		index = drm_crtc_index(crtc);
		DRM_MMP_MARK(mutex_lock, (unsigned long)&mtk_crtc->lock, i);

		DDP_MUTEX_LOCK_NESTED(&mtk_crtc->lock, i, __func__, __LINE__);
		CRTC_MMP_EVENT_START(index, atomic_commit, 0, 0);
	}
	mutex_nested_time_start = sched_clock();

	ret = drm_atomic_helper_swap_state(state, 0);
	if (ret) {
		DDPPR_ERR("DRM swap state failed! state:%p, ret:%d\n",
				state, ret);
		goto err_mutex_unlock;
	}

	mtk_atomic_state_get(state);
	if (async)
		mtk_atomic_schedule(private, state);
	else
		mtk_atomic_complete(private, state);

	mutex_nested_time_end = sched_clock();
	mutex_nested_time_period =
			mutex_nested_time_end - mutex_nested_time_start;
	if (mutex_nested_time_period > 1000000000) {
		DDPPR_ERR("M_ULOCK_NESTED:%s[%d] timeout:<%lld ns>!\n",
			__func__, __LINE__, mutex_nested_time_period);
		DRM_MMP_MARK(mutex_lock,
			(unsigned long)mutex_time_period, 0);
		dump_stack();
	}

err_mutex_unlock:
	for (i = MAX_CRTC - 1; i >= 0; i--) {
		if (!(crtc_mask >> i & 0x1))
			continue;

		crtc = private->crtc[i];
		mtk_crtc = to_mtk_crtc(crtc);
		index = drm_crtc_index(crtc);

		CRTC_MMP_EVENT_END(index, atomic_commit, 0, 0);
		DDP_MUTEX_UNLOCK_NESTED(&mtk_crtc->lock, i, __func__, __LINE__);
		DRM_MMP_MARK(mutex_lock, (unsigned long)&mtk_crtc->lock,
				i + (1 << 8));
	}
	DRM_MMP_EVENT_END(mutex_lock, 0, 0);

	mutex_unlock(&private->commit.lock);

	return 0;
}

static struct drm_atomic_state *
mtk_drm_atomic_state_alloc(struct drm_device *dev)
{
	struct mtk_atomic_state *mtk_state;

	mtk_state = kzalloc(sizeof(*mtk_state), GFP_KERNEL);
	if (!mtk_state)
		return NULL;

	if (drm_atomic_state_init(dev, &mtk_state->base) < 0) {
		kfree(mtk_state);
		return NULL;
	}

	INIT_LIST_HEAD(&mtk_state->list);
	kref_init(&mtk_state->kref);

	return &mtk_state->base;
}

static void mtk_drm_atomic_state_free(struct drm_atomic_state *state)
{
	mtk_atomic_state_put(state);
}

static const struct drm_mode_config_funcs mtk_drm_mode_config_funcs = {
	.fb_create = mtk_drm_mode_fb_create,
	.atomic_check = mtk_atomic_check,
	.atomic_commit = mtk_atomic_commit,
	.atomic_state_alloc = mtk_drm_atomic_state_alloc,
	.atomic_state_free = mtk_drm_atomic_state_free};

static const enum mtk_ddp_comp_id mt2701_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0, DDP_COMPONENT_RDMA0, DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_BLS,  DDP_COMPONENT_DSI0,
};

static const enum mtk_ddp_comp_id mt2701_mtk_ddp_ext[] = {
	DDP_COMPONENT_RDMA1, DDP_COMPONENT_DPI0,
};

static const enum mtk_ddp_comp_id mt2712_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,  DDP_COMPONENT_COLOR0, DDP_COMPONENT_AAL0,
	DDP_COMPONENT_OD,    DDP_COMPONENT_RDMA0,  DDP_COMPONENT_DPI0,
	DDP_COMPONENT_WDMA0, DDP_COMPONENT_PWM0,
};

static const enum mtk_ddp_comp_id mt2712_mtk_ddp_ext[] = {
	DDP_COMPONENT_OVL1,  DDP_COMPONENT_COLOR1, DDP_COMPONENT_AAL1,
	DDP_COMPONENT_OD1,   DDP_COMPONENT_RDMA1,  DDP_COMPONENT_DPI1,
	DDP_COMPONENT_WDMA1, DDP_COMPONENT_PWM1,
};

static enum mtk_ddp_comp_id mt8173_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0, DDP_COMPONENT_COLOR0, DDP_COMPONENT_AAL0,
	DDP_COMPONENT_OD,   DDP_COMPONENT_RDMA0,  DDP_COMPONENT_UFOE,
	DDP_COMPONENT_DSI0, DDP_COMPONENT_PWM0,
};

static const enum mtk_ddp_comp_id mt8173_mtk_ddp_ext[] = {
	DDP_COMPONENT_OVL1,  DDP_COMPONENT_COLOR1, DDP_COMPONENT_GAMMA0,
	DDP_COMPONENT_RDMA1, DDP_COMPONENT_DPI0,
};

static const enum mtk_ddp_comp_id mt6779_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0_2L,  DDP_COMPONENT_OVL0,
	DDP_COMPONENT_RDMA0,    DDP_COMPONENT_RDMA0_VIRTUAL0,
	DDP_COMPONENT_COLOR0,   DDP_COMPONENT_CCORR0,
	DDP_COMPONENT_AAL0,      DDP_COMPONENT_GAMMA0,
	DDP_COMPONENT_POSTMASK0, DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSI0,     DDP_COMPONENT_PWM0,
};

static const enum mtk_ddp_comp_id mt6779_mtk_ddp_ext[] = {
// Todo: Just for create a simple external session
// and Can create crtc1 successfully use this path.
#if 0
	DDP_COMPONENT_OVL1_2L,
	DDP_COMPONENT_WDMA0,
#else
	DDP_COMPONENT_OVL1_2L, DDP_COMPONENT_RDMA0,
/*DDP_COMPONENT_DPI0,*/
#endif
};

static const enum mtk_ddp_comp_id mt6779_mtk_ddp_third[] = {
	DDP_COMPONENT_OVL1_2L, DDP_COMPONENT_WDMA_VIRTUAL0,
	DDP_COMPONENT_WDMA_VIRTUAL1, DDP_COMPONENT_WDMA0,
};

static const enum mtk_ddp_comp_id mt6779_mtk_ddp_main_minor[] = {
	DDP_COMPONENT_OVL0_2L,       DDP_COMPONENT_OVL0,
	DDP_COMPONENT_WDMA_VIRTUAL0, DDP_COMPONENT_WDMA_VIRTUAL1,
	DDP_COMPONENT_WDMA0,
};

static const enum mtk_ddp_comp_id mt6779_mtk_ddp_main_minor_sub[] = {
	DDP_COMPONENT_RDMA0,    DDP_COMPONENT_RDMA0_VIRTUAL0,
	DDP_COMPONENT_COLOR0,   DDP_COMPONENT_CCORR0,
	DDP_COMPONENT_AAL0,      DDP_COMPONENT_GAMMA0,
	DDP_COMPONENT_POSTMASK0, DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSI0,     DDP_COMPONENT_PWM0,
};

static const enum mtk_ddp_comp_id mt6779_mtk_ddp_main_wb_path[] = {
	DDP_COMPONENT_OVL0, DDP_COMPONENT_WDMA_VIRTUAL0,
	DDP_COMPONENT_WDMA_VIRTUAL1, DDP_COMPONENT_WDMA0,
};


static const enum mtk_ddp_comp_id mt6885_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0_2L,		DDP_COMPONENT_OVL0,
	DDP_COMPONENT_OVL0_VIRTUAL0,	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_RDMA0_VIRTUAL0,	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_CCORR0,
#ifdef CONFIG_MTK_DRE30_SUPPORT
	DDP_COMPONENT_DMDP_AAL0,
#endif
	DDP_COMPONENT_AAL0,		DDP_COMPONENT_GAMMA0,
	DDP_COMPONENT_POSTMASK0,	DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSI0,		DDP_COMPONENT_PWM0,
};

static const enum mtk_ddp_comp_id mt6885_mtk_ddp_main_wb_path[] = {
	DDP_COMPONENT_OVL0,	DDP_COMPONENT_OVL0_VIRTUAL0,
	DDP_COMPONENT_WDMA0,
};


static const enum mtk_ddp_comp_id mt6885_mtk_ddp_main_minor[] = {
	DDP_COMPONENT_OVL0_2L,		DDP_COMPONENT_OVL0,
	DDP_COMPONENT_OVL0_VIRTUAL0,	DDP_COMPONENT_WDMA0,
};


static const enum mtk_ddp_comp_id mt6885_mtk_ddp_main_minor_sub[] = {
	DDP_COMPONENT_RDMA0,		DDP_COMPONENT_RDMA0_VIRTUAL0,
	DDP_COMPONENT_COLOR0,		DDP_COMPONENT_CCORR0,
#ifdef CONFIG_MTK_DRE30_SUPPORT
	DDP_COMPONENT_DMDP_AAL0,
#endif
	DDP_COMPONENT_AAL0,		DDP_COMPONENT_GAMMA0,
	DDP_COMPONENT_POSTMASK0,	DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSI0,		DDP_COMPONENT_PWM0,
};

static const enum mtk_ddp_comp_id mt6885_mtk_ddp_ext[] = {
	/* TODO: DP interface */
	DDP_COMPONENT_OVL2_2L, DDP_COMPONENT_RDMA4,
};

static const enum mtk_ddp_comp_id mt6885_mtk_ddp_third[] = {
	DDP_COMPONENT_OVL2_2L, DDP_COMPONENT_WDMA0,
};

static const struct mtk_addon_module_data addon_rsz_data[] = {
	{DISP_RSZ, ADDON_BETWEEN, DDP_COMPONENT_OVL0_2L},
};

static const struct mtk_addon_module_data addon_rsz_data_v2[] = {
	{DISP_RSZ_v2, ADDON_BETWEEN, DDP_COMPONENT_OVL0_2L},
};

static const struct mtk_addon_scenario_data mt6779_addon_main[ADDON_SCN_NR] = {
		[NONE] = {

				.module_num = 0,
				.hrt_type = HRT_TB_TYPE_GENERAL1,
			},
		[ONE_SCALING] = {

				.module_num = ARRAY_SIZE(addon_rsz_data),
				.module_data = addon_rsz_data,
				.hrt_type = HRT_TB_TYPE_RPO_L0,
			},
		[TWO_SCALING] = {

				.module_num = ARRAY_SIZE(addon_rsz_data),
				.module_data = addon_rsz_data,
				.hrt_type = HRT_TB_TYPE_GENERAL1,
			},
};

static const struct mtk_addon_scenario_data mt6779_addon_ext[ADDON_SCN_NR] = {
	[NONE] = {
		.module_num = 0,
		.hrt_type = HRT_TB_TYPE_GENERAL0,
	},
	[TRIPLE_DISP] = {
		.module_num = 0,
		.hrt_type = HRT_TB_TYPE_GENERAL0,
	},
};

static const struct mtk_addon_scenario_data mt6885_addon_main[ADDON_SCN_NR] = {
		[NONE] = {
				.module_num = 0,
				.hrt_type = HRT_TB_TYPE_GENERAL1,
			},
		[ONE_SCALING] = {
				.module_num = ARRAY_SIZE(addon_rsz_data_v2),
				.module_data = addon_rsz_data_v2,
				.hrt_type = HRT_TB_TYPE_RPO_L0,
			},
		[TWO_SCALING] = {
				.module_num = ARRAY_SIZE(addon_rsz_data_v2),
				.module_data = addon_rsz_data_v2,
				.hrt_type = HRT_TB_TYPE_GENERAL1,
			},
};

static const struct mtk_addon_scenario_data mt6885_addon_ext[ADDON_SCN_NR] = {
	[NONE] = {
		.module_num = 0,
		.hrt_type = HRT_TB_TYPE_GENERAL0,
	},
	[TRIPLE_DISP] = {
		.module_num = 0,
		.hrt_type = HRT_TB_TYPE_GENERAL0,
	},
};

static const enum mtk_ddp_comp_id mt6873_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0_2L,
	DDP_COMPONENT_OVL0, DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_COLOR0,   DDP_COMPONENT_CCORR0,
#ifdef CONFIG_MTK_DRE30_SUPPORT
	DDP_COMPONENT_DMDP_AAL0,
#endif
	DDP_COMPONENT_AAL0,      DDP_COMPONENT_GAMMA0,
	DDP_COMPONENT_POSTMASK0, DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSI0,     DDP_COMPONENT_PWM0,
};

static const enum mtk_ddp_comp_id mt6873_mtk_ddp_ext[] = {
#ifdef CONFIG_DRM_MEDIATEK_HDMI
	DDP_COMPONENT_OVL2_2L, DDP_COMPONENT_RDMA4,
	DDP_COMPONENT_DPI0,
#else
	DDP_COMPONENT_OVL2_2L, DDP_COMPONENT_RDMA4,
#endif
};

static const enum mtk_ddp_comp_id mt6873_mtk_ddp_third[] = {
	DDP_COMPONENT_OVL2_2L, DDP_COMPONENT_WDMA0,
};

static const enum mtk_ddp_comp_id mt6873_mtk_ddp_main_minor[] = {
	DDP_COMPONENT_OVL0_2L,       DDP_COMPONENT_OVL0,
	DDP_COMPONENT_WDMA0,
};

static const enum mtk_ddp_comp_id mt6873_mtk_ddp_main_minor_sub[] = {
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_COLOR0,   DDP_COMPONENT_CCORR0,
#ifdef CONFIG_MTK_DRE30_SUPPORT
	DDP_COMPONENT_DMDP_AAL0,
#endif
	DDP_COMPONENT_AAL0,      DDP_COMPONENT_GAMMA0,
	DDP_COMPONENT_POSTMASK0, DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSI0,     DDP_COMPONENT_PWM0,
};

static const enum mtk_ddp_comp_id mt6873_mtk_ddp_main_wb_path[] = {
	DDP_COMPONENT_OVL0, DDP_COMPONENT_WDMA0,
};

static const struct mtk_addon_scenario_data mt6873_addon_main[ADDON_SCN_NR] = {
		[NONE] = {
				.module_num = 0,
				.hrt_type = HRT_TB_TYPE_GENERAL1,
			},
		[ONE_SCALING] = {
				.module_num = ARRAY_SIZE(addon_rsz_data),
				.module_data = addon_rsz_data,
				.hrt_type = HRT_TB_TYPE_RPO_L0,
			},
		[TWO_SCALING] = {
				.module_num = ARRAY_SIZE(addon_rsz_data),
				.module_data = addon_rsz_data,
				.hrt_type = HRT_TB_TYPE_GENERAL1,
			},
};

static const struct mtk_addon_scenario_data mt6873_addon_ext[ADDON_SCN_NR] = {
	[NONE] = {
		.module_num = 0,
		.hrt_type = HRT_TB_TYPE_GENERAL0,
	},
	[TRIPLE_DISP] = {
		.module_num = 0,
		.hrt_type = HRT_TB_TYPE_GENERAL0,
	},
};

static const struct mtk_crtc_path_data mt6779_mtk_main_path_data = {
	.path[DDP_MAJOR][0] = mt6779_mtk_ddp_main,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt6779_mtk_ddp_main),
	.path_req_hrt[DDP_MAJOR][0] = true,
	.wb_path[DDP_MAJOR] = mt6779_mtk_ddp_main_wb_path,
	.wb_path_len[DDP_MAJOR] = ARRAY_SIZE(mt6779_mtk_ddp_main_wb_path),
	.path[DDP_MINOR][0] = mt6779_mtk_ddp_main_minor,
	.path_len[DDP_MINOR][0] = ARRAY_SIZE(mt6779_mtk_ddp_main_minor),
	.path_req_hrt[DDP_MINOR][0] = false,
	.path[DDP_MINOR][1] = mt6779_mtk_ddp_main_minor_sub,
	.path_len[DDP_MINOR][1] = ARRAY_SIZE(mt6779_mtk_ddp_main_minor_sub),
	.path_req_hrt[DDP_MINOR][1] = true,
	.addon_data = mt6779_addon_main,
};

static const struct mtk_crtc_path_data mt6779_mtk_ext_path_data = {
	.path[DDP_MAJOR][0] = mt6779_mtk_ddp_ext,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt6779_mtk_ddp_ext),
	.path_req_hrt[DDP_MAJOR][0] = true,
	.addon_data = mt6779_addon_ext,
};

static const struct mtk_crtc_path_data mt6779_mtk_third_path_data = {
	.path[DDP_MAJOR][0] = mt6779_mtk_ddp_third,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt6779_mtk_ddp_third),
	.addon_data = mt6779_addon_ext,
};

static const struct mtk_crtc_path_data mt6885_mtk_main_path_data = {
	.path[DDP_MAJOR][0] = mt6885_mtk_ddp_main,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt6885_mtk_ddp_main),
	.path_req_hrt[DDP_MAJOR][0] = true,
	.wb_path[DDP_MAJOR] = mt6885_mtk_ddp_main_wb_path,
	.wb_path_len[DDP_MAJOR] = ARRAY_SIZE(mt6885_mtk_ddp_main_wb_path),
	.path[DDP_MINOR][0] = mt6885_mtk_ddp_main_minor,
	.path_len[DDP_MINOR][0] = ARRAY_SIZE(mt6885_mtk_ddp_main_minor),
	.path_req_hrt[DDP_MINOR][0] = false,
	.path[DDP_MINOR][1] = mt6885_mtk_ddp_main_minor_sub,
	.path_len[DDP_MINOR][1] = ARRAY_SIZE(mt6885_mtk_ddp_main_minor_sub),
	.path_req_hrt[DDP_MINOR][1] = true,
	.addon_data = mt6885_addon_main,
};

static const struct mtk_crtc_path_data mt6885_mtk_ext_path_data = {
	.path[DDP_MAJOR][0] = mt6885_mtk_ddp_ext,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt6885_mtk_ddp_ext),
	.path_req_hrt[DDP_MAJOR][0] = true,
	.addon_data = mt6885_addon_ext,
};

static const struct mtk_crtc_path_data mt6885_mtk_third_path_data = {
	.path[DDP_MAJOR][0] = mt6885_mtk_ddp_third,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt6885_mtk_ddp_third),
	.addon_data = mt6885_addon_ext,
};

static const struct mtk_crtc_path_data mt2701_mtk_main_path_data = {
	.path[DDP_MAJOR][0] = mt2701_mtk_ddp_main,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt2701_mtk_ddp_main),
	.path_req_hrt[DDP_MAJOR][0] = true,
};

static const struct mtk_crtc_path_data mt2701_mtk_ext_path_data = {
	.path[DDP_MAJOR][0] = mt2701_mtk_ddp_ext,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt2701_mtk_ddp_ext),
	.path_req_hrt[DDP_MAJOR][0] = true,
};

static const struct mtk_crtc_path_data mt2712_mtk_main_path_data = {
	.path[DDP_MAJOR][0] = mt2712_mtk_ddp_main,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt2712_mtk_ddp_main),
	.path_req_hrt[DDP_MAJOR][0] = true,
};

static const struct mtk_crtc_path_data mt2712_mtk_ext_path_data = {
	.path[DDP_MAJOR][0] = mt2712_mtk_ddp_ext,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt2712_mtk_ddp_ext),
	.path_req_hrt[DDP_MAJOR][0] = true,
};

static const struct mtk_crtc_path_data mt8173_mtk_main_path_data = {
	.path[DDP_MAJOR][0] = mt8173_mtk_ddp_main,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt8173_mtk_ddp_main),
	.path_req_hrt[DDP_MAJOR][0] = true,
};

static const struct mtk_crtc_path_data mt8173_mtk_ext_path_data = {
	.path[DDP_MAJOR][0] = mt8173_mtk_ddp_ext,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt8173_mtk_ddp_ext),
	.path_req_hrt[DDP_MAJOR][0] = true,
};

static const struct mtk_crtc_path_data mt6873_mtk_main_path_data = {
	.path[DDP_MAJOR][0] = mt6873_mtk_ddp_main,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt6873_mtk_ddp_main),
	.path_req_hrt[DDP_MAJOR][0] = true,
	.wb_path[DDP_MAJOR] = mt6873_mtk_ddp_main_wb_path,
	.wb_path_len[DDP_MAJOR] = ARRAY_SIZE(mt6873_mtk_ddp_main_wb_path),
	.path[DDP_MINOR][0] = mt6873_mtk_ddp_main_minor,
	.path_len[DDP_MINOR][0] = ARRAY_SIZE(mt6873_mtk_ddp_main_minor),
	.path_req_hrt[DDP_MINOR][0] = false,
	.path[DDP_MINOR][1] = mt6873_mtk_ddp_main_minor_sub,
	.path_len[DDP_MINOR][1] = ARRAY_SIZE(mt6873_mtk_ddp_main_minor_sub),
	.path_req_hrt[DDP_MINOR][1] = true,
	.addon_data = mt6873_addon_main,
};

static const struct mtk_crtc_path_data mt6873_mtk_ext_path_data = {
	.path[DDP_MAJOR][0] = mt6873_mtk_ddp_ext,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt6873_mtk_ddp_ext),
	.path_req_hrt[DDP_MAJOR][0] = true,
	.addon_data = mt6873_addon_ext,
};

static const struct mtk_crtc_path_data mt6873_mtk_third_path_data = {
	.path[DDP_MAJOR][0] = mt6873_mtk_ddp_third,
	.path_len[DDP_MAJOR][0] = ARRAY_SIZE(mt6873_mtk_ddp_third),
	.addon_data = mt6873_addon_ext,
};

const struct mtk_session_mode_tb mt6779_mode_tb[MTK_DRM_SESSION_NUM] = {
		[MTK_DRM_SESSION_DL] = {

				.en = 1,
				.ddp_mode = {DDP_MAJOR, DDP_MAJOR, DDP_MAJOR},
			},
		[MTK_DRM_SESSION_DOUBLE_DL] = {

				.en = 1,
				.ddp_mode = {DDP_MAJOR, DDP_MAJOR, DDP_MAJOR},
			},
		[MTK_DRM_SESSION_DC_MIRROR] = {

				.en = 1,
				.ddp_mode = {DDP_MINOR, DDP_MAJOR, DDP_NO_USE},
			},
		[MTK_DRM_SESSION_TRIPLE_DL] = {

				.en = 1,
				.ddp_mode = {DDP_MAJOR, DDP_MINOR, DDP_MAJOR},
			},
};

static const struct mtk_fake_eng_reg mt6779_fake_eng_reg[] = {
		{.CG_idx = 0, .CG_bit = 19, .share_port = false},
		{.CG_idx = 1, .CG_bit = 4, .share_port = false},
};

static const struct mtk_fake_eng_data mt6779_fake_eng_data = {
	.fake_eng_num =  ARRAY_SIZE(mt6779_fake_eng_reg),
	.fake_eng_reg = mt6779_fake_eng_reg,
};

const struct mtk_session_mode_tb mt6885_mode_tb[MTK_DRM_SESSION_NUM] = {
		[MTK_DRM_SESSION_DL] = {

				.en = 1,
				.ddp_mode = {DDP_MAJOR, DDP_MAJOR, DDP_MAJOR},
			},
		[MTK_DRM_SESSION_DOUBLE_DL] = {

				.en = 1,
				.ddp_mode = {DDP_MAJOR, DDP_MAJOR, DDP_MAJOR},
			},
		[MTK_DRM_SESSION_DC_MIRROR] = {

				.en = 1,
				.ddp_mode = {DDP_MINOR, DDP_MAJOR, DDP_NO_USE},
			},
		[MTK_DRM_SESSION_TRIPLE_DL] = {

				.en = 1,
				.ddp_mode = {DDP_MAJOR, DDP_MINOR, DDP_MAJOR},
			},
};

const struct mtk_session_mode_tb mt6873_mode_tb[MTK_DRM_SESSION_NUM] = {
		[MTK_DRM_SESSION_DL] = {

				.en = 1,
				.ddp_mode = {DDP_MAJOR, DDP_MAJOR, DDP_MAJOR},
			},
		[MTK_DRM_SESSION_DOUBLE_DL] = {

				.en = 1,
				.ddp_mode = {DDP_MAJOR, DDP_MAJOR, DDP_MAJOR},
			},
		[MTK_DRM_SESSION_DC_MIRROR] = {

				.en = 1,
				.ddp_mode = {DDP_MINOR, DDP_MAJOR, DDP_NO_USE},
			},
		[MTK_DRM_SESSION_TRIPLE_DL] = {

				.en = 1,
				.ddp_mode = {DDP_MAJOR, DDP_MINOR, DDP_MAJOR},
			},
};
static const struct mtk_fake_eng_reg mt6885_fake_eng_reg[] = {
		{.CG_idx = 0, .CG_bit = 14, .share_port = true},
		{.CG_idx = 0, .CG_bit = 15, .share_port = true},
};

static const struct mtk_fake_eng_data mt6885_fake_eng_data = {
	.fake_eng_num =  ARRAY_SIZE(mt6885_fake_eng_reg),
	.fake_eng_reg = mt6885_fake_eng_reg,
};

static const struct mtk_fake_eng_reg mt6873_fake_eng_reg[] = {
		{.CG_idx = 0, .CG_bit = 18, .share_port = true},
		{.CG_idx = 0, .CG_bit = 19, .share_port = true},
};

static const struct mtk_fake_eng_data mt6873_fake_eng_data = {
	.fake_eng_num =  ARRAY_SIZE(mt6873_fake_eng_reg),
	.fake_eng_reg = mt6873_fake_eng_reg,
};

static const struct mtk_mmsys_driver_data mt2701_mmsys_driver_data = {
	.main_path_data = &mt2701_mtk_main_path_data,
	.ext_path_data = &mt2701_mtk_ext_path_data,
	.mmsys_id = MMSYS_MT2701,
	.shadow_register = true,
};

static const struct mtk_mmsys_driver_data mt2712_mmsys_driver_data = {
	.main_path_data = &mt2712_mtk_main_path_data,
	.ext_path_data = &mt2712_mtk_ext_path_data,
	.mmsys_id = MMSYS_MT2712,
};

static const struct mtk_mmsys_driver_data mt8173_mmsys_driver_data = {
	.main_path_data = &mt8173_mtk_main_path_data,
	.ext_path_data = &mt8173_mtk_ext_path_data,
	.mmsys_id = MMSYS_MT8173,
};

static const struct mtk_mmsys_driver_data mt6779_mmsys_driver_data = {
	.main_path_data = &mt6779_mtk_main_path_data,
	.ext_path_data = &mt6779_mtk_ext_path_data,
	.third_path_data = &mt6779_mtk_third_path_data,
	.mmsys_id = MMSYS_MT6779,
	.mode_tb = mt6779_mode_tb,
	.sodi_config = mt6779_mtk_sodi_config,
	.fake_eng_data = &mt6779_fake_eng_data,
};

static const struct mtk_mmsys_driver_data mt6885_mmsys_driver_data = {
	.main_path_data = &mt6885_mtk_main_path_data,
	.ext_path_data = &mt6885_mtk_ext_path_data,
	.third_path_data = &mt6885_mtk_third_path_data,
	.fake_eng_data = &mt6885_fake_eng_data,
	.mmsys_id = MMSYS_MT6885,
	.mode_tb = mt6885_mode_tb,
	.sodi_config = mt6885_mtk_sodi_config,
};

static const struct mtk_mmsys_driver_data mt6873_mmsys_driver_data = {
	.main_path_data = &mt6873_mtk_main_path_data,
	.ext_path_data = &mt6873_mtk_ext_path_data,
	.third_path_data = &mt6873_mtk_third_path_data,
	.fake_eng_data = &mt6873_fake_eng_data,
	.mmsys_id = MMSYS_MT6873,
	.mode_tb = mt6873_mode_tb,
	.sodi_config = mt6873_mtk_sodi_config,
};

#ifdef MTK_DRM_FENCE_SUPPORT
static int mtk_drm_fence_release_thread(void *data)
{
	struct sched_param param = {.sched_priority = 87};
	struct mtk_drm_private *private = (struct mtk_drm_private *)data;

	sched_setscheduler(current, SCHED_RR, &param);

	while (!kthread_should_stop()) {
		wait_event_interruptible(_mtk_fence_wq,
					 atomic_read(&_mtk_fence_update_event));
		atomic_set(&_mtk_fence_update_event, 0);

		DDPINFO("%s:%d wait vblank+\n", __func__, __LINE__);
		drm_wait_one_vblank(private->drm, 0);
		DDPINFO("%s:%d wait vblank-\n", __func__, __LINE__);

		mutex_lock(&private->commit.lock);
		mtk_release_present_fence(private->session_id[0],
					  atomic_read(&_mtk_fence_idx));
		mutex_unlock(&private->commit.lock);
	}

	return 0;
}

void mtk_drm_fence_update(unsigned int fence_idx)
{
	int pf = atomic_read(&_mtk_fence_idx);

	if (pf != -1 && fence_idx < (unsigned int)pf)
		return;

	atomic_set(&_mtk_fence_idx, fence_idx);
	atomic_set(&_mtk_fence_update_event, 1);
	wake_up_interruptible(&_mtk_fence_wq);

	CRTC_MMP_MARK(0, update_present_fence, 0, fence_idx);
}

int mtk_drm_suspend_release_fence(struct device *dev)
{
	unsigned int i = 0;
	struct mtk_drm_private *private = dev_get_drvdata(dev);

	for (i = 0; i < MTK_TIMELINE_OUTPUT_TIMELINE_ID; i++) {
		DDPINFO("%s layerid=%d\n", __func__, i);
		mtk_release_layer_fence(private->session_id[0], i);
	}
	/* release present fence */
	mtk_release_present_fence(private->session_id[0],
				  atomic_read(&_mtk_fence_idx));
	return 0;
}

void mtk_drm_suspend_release_present_fence(struct device *dev)
{
	struct mtk_drm_private *private = dev_get_drvdata(dev);

	mtk_release_present_fence(private->session_id[0],
				  atomic_read(&_mtk_fence_idx));
}
#endif

/*---------------- function for repaint start ------------------*/
void drm_trigger_repaint(enum DRM_REPAINT_TYPE type, struct drm_device *drm_dev)
{
	if (type > DRM_WAIT_FOR_REPAINT && type < DRM_REPAINT_TYPE_NUM) {
		struct mtk_drm_private *pvd = drm_dev->dev_private;
		struct repaint_job_t *repaint_job;

		/* get a repaint_job_t from pool */
		spin_lock(&pvd->repaint_data.wq.lock);
		if (!list_empty(&pvd->repaint_data.job_pool)) {
			repaint_job =
				list_first_entry(&pvd->repaint_data.job_pool,
						 struct repaint_job_t, link);
			list_del_init(&repaint_job->link);
		} else { /* create repaint_job_t if pool is empty */
			repaint_job = kzalloc(sizeof(struct repaint_job_t),
					      GFP_KERNEL);
			if (IS_ERR_OR_NULL(repaint_job)) {
				DDPPR_ERR("allocate repaint_job_t fail\n");
				spin_unlock(&pvd->repaint_data.wq.lock);
				return;
			}
			INIT_LIST_HEAD(&repaint_job->link);
			DDPINFO("[REPAINT] allocate a new repaint_job_t\n");
		}

		/* init & insert repaint_job_t into queue */
		repaint_job->type = (unsigned int)type;
		list_add_tail(&repaint_job->link, &pvd->repaint_data.job_queue);

		DDPINFO("[REPAINT] insert repaint_job in queue, type:%u\n",
			type);
		spin_unlock(&pvd->repaint_data.wq.lock);
		wake_up_interruptible(&pvd->repaint_data.wq);
	}
}

int mtk_drm_wait_repaint_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	int ret = 0;
	unsigned int *type = data;
	struct repaint_job_t *repaint_job;
	struct mtk_drm_private *pvd = dev->dev_private;

	/* reset status & wake-up threads which wait for repainting */
	DDPINFO("[REPAINT] HWC waits for repaint\n");

	/*  wait for repaint */
	ret = wait_event_interruptible(
		pvd->repaint_data.wq,
		!list_empty(&pvd->repaint_data.job_queue));

	if (ret >= 0) {
		/* retrieve first repaint_job_t from queue */
		spin_lock(&pvd->repaint_data.wq.lock);
		repaint_job = list_first_entry(&pvd->repaint_data.job_queue,
					       struct repaint_job_t, link);

		*type = repaint_job->type;

		/* remove from queue & add repaint_job_t in pool */
		list_del_init(&repaint_job->link);
		list_add_tail(&repaint_job->link, &pvd->repaint_data.job_pool);
		spin_unlock(&pvd->repaint_data.wq.lock);
		DDPINFO("[REPAINT] trigger repaint, type: %u\n", *type);
	} else {
		*type = DRM_WAIT_FOR_REPAINT;
		DDPINFO("[REPAINT] interrupted unexpected\n");
	}

	return 0;
}
/*---------------- function for repaint end ------------------*/
static void mtk_drm_get_top_clk(struct mtk_drm_private *priv)
{
	struct device *dev = priv->mmsys_dev;
	struct device_node *node = dev->of_node;
	struct clk *clk;
	int ret, i;

	if (of_property_read_u32(node, "clock-num", &priv->top_clk_num)) {
		priv->top_clk_num = -1;
		priv->top_clk = NULL;
		return;
	}

	priv->top_clk = devm_kmalloc_array(dev, priv->top_clk_num,
					   sizeof(*priv->top_clk), GFP_KERNEL);

	for (i = 0; i < priv->top_clk_num; i++) {
		clk = of_clk_get(node, i);

		if (IS_ERR(clk)) {
			DDPPR_ERR("%s get %d clk failed\n", __func__, i);
			priv->top_clk_num = -1;
			return;
		}

		priv->top_clk[i] = clk;
		/* TODO: check display enable from lk */
		/* Because of align lk hw power status,
		 * we power on mtcmos at the beginning of
		 * the display initialization.
		 * We will power off mtcmos at the end of
		 * the display initialization.
		 */
		ret = clk_prepare_enable(priv->top_clk[i]);
		if (ret)
			DDPPR_ERR("top clk prepare enable failed:%d\n", i);
	}

	spin_lock_init(&top_clk_lock);
	/* TODO: check display enable from lk */
	atomic_set(&top_isr_ref, 0);
	atomic_set(&top_clk_ref, 1);
	priv->power_state = true;
}

void mtk_drm_top_clk_prepare_enable(struct drm_device *drm)
{
	struct mtk_drm_private *priv = drm->dev_private;
	int i;
	bool en = 1;
	int ret;
	unsigned long flags = 0;

	if (priv->top_clk_num <= 0)
		return;

	for (i = 0; i < priv->top_clk_num; i++) {
		if (IS_ERR(priv->top_clk[i])) {
			DDPPR_ERR("%s invalid %d clk\n", __func__, i);
			return;
		}
		ret = clk_prepare_enable(priv->top_clk[i]);
		if (ret)
			DDPPR_ERR("top clk prepare enable failed:%d\n", i);
	}

	spin_lock_irqsave(&top_clk_lock, flags);
	atomic_inc(&top_clk_ref);
	if (atomic_read(&top_clk_ref) == 1)
		priv->power_state = true;
	spin_unlock_irqrestore(&top_clk_lock, flags);

	if (priv->data->sodi_config)
		priv->data->sodi_config(drm, DDP_COMPONENT_ID_MAX, NULL, &en);
}

void mtk_drm_top_clk_disable_unprepare(struct drm_device *drm)
{
	struct mtk_drm_private *priv = drm->dev_private;
	int i = 0, cnt = 0;
	unsigned long flags = 0;

	if (priv->top_clk_num <= 0)
		return;

	spin_lock_irqsave(&top_clk_lock, flags);
	atomic_dec(&top_clk_ref);
	if (atomic_read(&top_clk_ref) == 0) {
		while (atomic_read(&top_isr_ref) > 0 &&
		       cnt++ < 10) {
			spin_unlock_irqrestore(&top_clk_lock, flags);
			pr_notice("%s waiting for isr job, %d\n",
				  __func__, cnt);
			usleep_range(20, 40);
			spin_lock_irqsave(&top_clk_lock, flags);
		}
		priv->power_state = false;
	}
	spin_unlock_irqrestore(&top_clk_lock, flags);

	for (i = priv->top_clk_num - 1; i >= 0; i--) {
		if (IS_ERR(priv->top_clk[i])) {
			DDPPR_ERR("%s invalid %d clk\n", __func__, i);
			return;
		}
		clk_disable_unprepare(priv->top_clk[i]);
	}
}

bool mtk_drm_top_clk_isr_get(char *master)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&top_clk_lock, flags);
	if (atomic_read(&top_clk_ref) <= 0) {
		DDPPR_ERR("%s, top clk off at %s\n",
			  __func__, master ? master : "NULL");
		spin_unlock_irqrestore(&top_clk_lock, flags);
		return false;
	}
	atomic_inc(&top_isr_ref);
	spin_unlock_irqrestore(&top_clk_lock, flags);

	return true;
}

void mtk_drm_top_clk_isr_put(char *master)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&top_clk_lock, flags);

	/* when timeout of polling isr ref in unpreare top clk*/
	if (atomic_read(&top_clk_ref) <= 0)
		DDPPR_ERR("%s, top clk off at %s\n",
			  __func__, master ? master : "NULL");

	atomic_dec(&top_isr_ref);
	spin_unlock_irqrestore(&top_clk_lock, flags);
}

static void mtk_drm_first_enable(struct drm_device *drm)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, drm) {
		if (mtk_drm_is_enable_from_lk(crtc))
			mtk_drm_crtc_first_enable(crtc);
		lcm_fps_ctx_init(crtc);
	}
}

bool mtk_drm_session_mode_is_decouple_mode(struct drm_device *dev)
{
	struct mtk_drm_private *priv = dev->dev_private;

	if (priv->session_mode == MTK_DRM_SESSION_DC_MIRROR)
		return true;

	return false;
}

bool mtk_drm_session_mode_is_mirror_mode(struct drm_device *dev)
{
	struct mtk_drm_private *priv = dev->dev_private;

	if (priv->session_mode == MTK_DRM_SESSION_DC_MIRROR)
		return true;

	return false;
}

struct mtk_panel_params *mtk_drm_get_lcm_ext_params(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_panel_ext *panel_ext = NULL;
	struct mtk_ddp_comp *comp;

	comp = mtk_ddp_comp_request_output(mtk_crtc);
	if (comp)
		mtk_ddp_comp_io_cmd(comp, NULL, REQ_PANEL_EXT, &panel_ext);

	if (!panel_ext || !panel_ext->params)
		return NULL;

	return panel_ext->params;
}

struct mtk_panel_funcs *mtk_drm_get_lcm_ext_funcs(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_panel_ext *panel_ext = NULL;
	struct mtk_ddp_comp *comp;

	comp = mtk_ddp_comp_request_output(mtk_crtc);
	if (comp)
		mtk_ddp_comp_io_cmd(comp, NULL, REQ_PANEL_EXT, &panel_ext);

	if (!panel_ext || !panel_ext->funcs)
		return NULL;

	return panel_ext->funcs;
}

int mtk_drm_get_display_caps_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *file_priv)
{
	int ret = 0;
	struct mtk_drm_private *private = dev->dev_private;
	struct mtk_drm_disp_caps_info *caps_info = data;
	struct mtk_panel_params *params =
		mtk_drm_get_lcm_ext_params(private->crtc[0]);

	memset(caps_info, 0, sizeof(*caps_info));

	caps_info->disp_feature_flag |= DRM_DISP_FEATURE_DISP_SELF_REFRESH;

	if (mtk_drm_helper_get_opt(private->helper_opt, MTK_DRM_OPT_HRT))
		caps_info->disp_feature_flag |= DRM_DISP_FEATURE_HRT;

	if (mtk_drm_helper_get_opt(private->helper_opt, MTK_DRM_OPT_RPO)) {
		caps_info->disp_feature_flag |= DRM_DISP_FEATURE_RPO;
		caps_info->rsz_in_max[0] = private->rsz_in_max[0];
		caps_info->rsz_in_max[1] = private->rsz_in_max[1];
	}
	caps_info->disp_feature_flag |= DRM_DISP_FEATURE_FBDC;

#ifdef CONFIG_MTK_LCM_PHYSICAL_ROTATION_HW
	caps_info->lcm_degree = 180;
#endif

	caps_info->lcm_color_mode = MTK_DRM_COLOR_MODE_NATIVE;
	if (mtk_drm_helper_get_opt(private->helper_opt, MTK_DRM_OPT_OVL_WCG)) {
		if (params)
			caps_info->lcm_color_mode = params->lcm_color_mode;
		else
			DDPPR_ERR("%s: failed to get lcm color mode\n",
				  __func__);
	}

	if (params) {
		caps_info->min_luminance = params->min_luminance;
		caps_info->average_luminance = params->average_luminance;
		caps_info->max_luminance = params->max_luminance;
	} else
		DDPPR_ERR("%s: failed to get lcm luminance\n", __func__);

#ifdef DRM_MMPATH
	private->HWC_gpid = task_tgid_nr(current);
#endif

	return ret;
}

/* runtime calculate vsync fps*/
int lcm_fps_ctx_init(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *output_comp;
	unsigned int index;

	if (!crtc || crtc->index >= MAX_CRTC) {
		DDPPR_ERR("%s:invalid crtc:%d\n",
			  __func__, crtc->base.id);
		return -EINVAL;
	}
	index = crtc->index;

	if (atomic_read(&lcm_fps_ctx[index].is_inited))
		return 0;

	output_comp = mtk_ddp_comp_request_output(mtk_crtc);
	if (unlikely(!output_comp)) {
		DDPPR_ERR("%s:CRTC:%d invalid output comp\n",
			  __func__, index);
		return -EINVAL;
	}

	memset(&lcm_fps_ctx[index], 0, sizeof(struct lcm_fps_ctx_t));
	spin_lock_init(&lcm_fps_ctx[index].lock);
	atomic_set(&lcm_fps_ctx[index].skip_update, 0);
	if (!mtk_dsi_is_cmd_mode(output_comp))
		lcm_fps_ctx[index].dsi_mode = 1;
	else
		lcm_fps_ctx[index].dsi_mode = 0;
	atomic_set(&lcm_fps_ctx[index].is_inited, 1);

	DDPINFO("%s CRTC:%d done\n", __func__, index);
	return 0;
}

int lcm_fps_ctx_reset(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *output_comp;
	unsigned long flags = 0;
	unsigned int index;

	if (!crtc || crtc->index >= MAX_CRTC)
		return -EINVAL;
	index = crtc->index;

	if (!atomic_read(&lcm_fps_ctx[index].is_inited))
		return 0;

	if (atomic_read(&lcm_fps_ctx[index].skip_update))
		return 0;

	output_comp = mtk_ddp_comp_request_output(mtk_crtc);
	if (unlikely(!output_comp)) {
		DDPPR_ERR("%s:CRTC:%d invalid output comp\n",
			  __func__, index);
		return -EINVAL;
	}

	spin_lock_irqsave(&lcm_fps_ctx[index].lock, flags);
	if (!mtk_dsi_is_cmd_mode(output_comp))
		lcm_fps_ctx[index].dsi_mode = 1;
	else
		lcm_fps_ctx[index].dsi_mode = 0;
	lcm_fps_ctx[index].head_idx = 0;
	lcm_fps_ctx[index].num = 0;
	lcm_fps_ctx[index].last_ns = 0;
	memset(&lcm_fps_ctx[index].array, 0, sizeof(lcm_fps_ctx[index].array));
	atomic_set(&lcm_fps_ctx[index].skip_update, 0);
	spin_unlock_irqrestore(&lcm_fps_ctx[index].lock, flags);

	DDPINFO("%s CRTC:%d done\n", __func__, index);

	return 0;
}

int lcm_fps_ctx_update(unsigned long long cur_ns,
		unsigned int crtc_id, unsigned int mode)
{
	unsigned int idx, index = crtc_id;
	unsigned long long delta;
	unsigned long flags = 0;

	if (index > MAX_CRTC)
		return -EINVAL;

	if (!atomic_read(&lcm_fps_ctx[index].is_inited))
		return 0;

	if (lcm_fps_ctx[index].dsi_mode != mode)
		return 0;

	if (atomic_read(&lcm_fps_ctx[index].skip_update))
		return 0;

	spin_lock_irqsave(&lcm_fps_ctx[index].lock, flags);

	delta = cur_ns - lcm_fps_ctx[index].last_ns;
	if (delta == 0 || lcm_fps_ctx[index].last_ns == 0) {
		lcm_fps_ctx[index].last_ns = cur_ns;
		spin_unlock_irqrestore(&lcm_fps_ctx[index].lock, flags);
		return 0;
	}

	idx = (lcm_fps_ctx[index].head_idx +
		lcm_fps_ctx[index].num) % LCM_FPS_ARRAY_SIZE;
	lcm_fps_ctx[index].array[idx] = delta;

	if (lcm_fps_ctx[index].num < LCM_FPS_ARRAY_SIZE)
		lcm_fps_ctx[index].num++;
	else
		lcm_fps_ctx[index].head_idx = (lcm_fps_ctx[index].head_idx +
			 1) % LCM_FPS_ARRAY_SIZE;

	lcm_fps_ctx[index].last_ns = cur_ns;

	spin_unlock_irqrestore(&lcm_fps_ctx[index].lock, flags);

	DDPINFO("%s CRTC:%d update %lld to index %d\n",
		__func__, index, delta, idx);

	return 0;
}

unsigned int lcm_fps_ctx_get(unsigned int crtc_id)
{
	unsigned int i;
	unsigned long long duration_avg = 0;
	unsigned long long duration_min = (1ULL << 63) - 1ULL;
	unsigned long long duration_max = 0;
	unsigned long long duration_sum = 0;
	unsigned long long fps = 100000000000;
	unsigned long flags = 0;
	unsigned int index = crtc_id;

	if (crtc_id >= MAX_CRTC) {
		DDPPR_ERR("%s:invalid crtc:%u\n",
			  __func__, crtc_id);
		return 0;
	}

	if (!atomic_read(&lcm_fps_ctx[index].is_inited))
		return 0;

	spin_lock_irqsave(&lcm_fps_ctx[index].lock, flags);
	if (atomic_read(&lcm_fps_ctx[index].skip_update) &&
	    lcm_fps_ctx[index].fps) {
		spin_unlock_irqrestore(&lcm_fps_ctx[index].lock, flags);
		return lcm_fps_ctx[index].fps;
	}

	if (lcm_fps_ctx[index].num <= 6) {
		unsigned int ret = (lcm_fps_ctx[index].fps ?
			lcm_fps_ctx[index].fps : 0);

		DDPINFO("%s CRTC:%d num %d < 6, return fps:%u\n",
			__func__, index, lcm_fps_ctx[index].num, ret);
		spin_unlock_irqrestore(&lcm_fps_ctx[index].lock, flags);
		return ret;
	}


	for (i = 0; i < lcm_fps_ctx[index].num; i++) {
		duration_sum += lcm_fps_ctx[index].array[i];
		duration_min = min(duration_min, lcm_fps_ctx[index].array[i]);
		duration_max = max(duration_max, lcm_fps_ctx[index].array[i]);
	}
	duration_sum -= duration_min + duration_max;
	duration_avg = duration_sum / (lcm_fps_ctx[index].num - 2);
	do_div(fps, duration_avg);
	lcm_fps_ctx[index].fps = (unsigned int)fps;

	DDPMSG("%s CRTC:%d max=%lld, min=%lld, sum=%lld, num=%d, fps=%u\n",
		__func__, index, duration_max, duration_min,
		duration_sum, lcm_fps_ctx[index].num, (unsigned int)fps);

	if (lcm_fps_ctx[index].num >= LCM_FPS_ARRAY_SIZE) {
		atomic_set(&lcm_fps_ctx[index].skip_update, 1);
		DDPINFO("%s set skip_update\n", __func__);
	}
	spin_unlock_irqrestore(&lcm_fps_ctx[index].lock, flags);

	return (unsigned int)fps;
}

int mtk_drm_primary_get_info(struct drm_device *dev,
			     struct drm_mtk_session_info *info)
{
	int ret = 0;
	unsigned int vramsize = 0, fps = 0;
	phys_addr_t fb_base = 0;
	struct mtk_drm_private *private = dev->dev_private;
	struct mtk_panel_params *params =
		mtk_drm_get_lcm_ext_params(private->crtc[0]);

	if (params) {
		info->physicalWidthUm = params->physical_width_um;
		info->physicalHeightUm = params->physical_height_um;
	} else
		DDPPR_ERR("Cannot get lcm_ext_params\n");

	info->vsyncFPS = lcm_fps_ctx_get(0);
	if (!info->vsyncFPS) {
		_parse_tag_videolfb(&vramsize, &fb_base, &fps);
		info->vsyncFPS = fps;
	}

	return ret;
}

int mtk_drm_get_info_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	int ret = 0;
	struct drm_mtk_session_info *info = data;
	int s_type = MTK_SESSION_TYPE(info->session_id);

	if (s_type == MTK_SESSION_PRIMARY) {
		ret = mtk_drm_primary_get_info(dev, info);
#if ((defined CONFIG_MTK_HDMI_SUPPORT) ||                            \
	(defined(CONFIG_MTK_DUAL_DISPLAY_SUPPORT) &&                     \
	(CONFIG_MTK_DUAL_DISPLAY_SUPPORT == 2)))
	} else if (s_type == MTK_SESSION_EXTERNAL) {
		/* TODO: external_display_get_info(info, info->session_id); */
		DDPPR_ERR("%s not implement type:EXTERNAL\n", __func__);
		return -EINVAL;
#endif
	} else if (s_type == MTK_SESSION_MEMORY) {
		return ret;
	} else {
		DDPPR_ERR("invalid session type:0x%08x\n", info->session_id);
		return -EINVAL;
	}

	return ret;
}

int mtk_drm_set_ddp_mode(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	int ret = 0;
	unsigned int *session_mode = data;

	ret = mtk_session_set_mode(dev, *session_mode);
	return ret;
}

int mtk_drm_ioctl_get_lcm_index(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	int ret = 0;
	/* TODO */
	/*
	 * struct mtk_drm_private *private = dev->dev_private;
	 * struct mtk_panel_params *params =
	 *			mtk_drm_get_lcm_ext_params(private->crtc[0]);
	 */

	/* wait for member of mtk_panel_params include lcm index */
	/* *data = params->xxx; */

	return ret;
}

static int mtk_drm_kms_init(struct drm_device *drm)
{
	struct mtk_drm_private *private = drm->dev_private;
	struct platform_device *pdev;
	struct device_node *np;
	int ret;

	if (!iommu_present(&platform_bus_type))
		return -EPROBE_DEFER;

	DDPINFO("%s+\n", __func__);
	pdev = of_find_device_by_node(private->mutex_node);
	if (!pdev) {
		dev_err(drm->dev, "Waiting for disp-mutex device %s\n",
			private->mutex_node->full_name);
		of_node_put(private->mutex_node);
		return -EPROBE_DEFER;
	}

	private->mutex_dev = &pdev->dev;

	drm_mode_config_init(drm);

	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;

	/*
	 * set max width and height as default value(4096x4096).
	 * this value would be used to check framebuffer size limitation
	 * at drm_mode_addfb().
	 */
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.funcs = &mtk_drm_mode_config_funcs;
	drm->mode_config.allow_fb_modifiers = true;

	ret = component_bind_all(drm->dev, drm);
	if (ret)
		goto err_config_cleanup;

	/*
	 * We currently support two fixed data streams, each optional,
	 * and each statically assigned to a crtc:
	 * OVL0 -> COLOR0 -> AAL -> OD -> RDMA0 -> UFOE -> DSI0 ...
	 */
	ret = mtk_drm_crtc_create(drm, private->data->main_path_data);
	if (ret < 0)
		goto err_component_unbind;
	/* ... and OVL1 -> COLOR1 -> GAMMA -> RDMA1 -> DPI0. */
	ret = mtk_drm_crtc_create(drm, private->data->ext_path_data);
	if (ret < 0)
		goto err_component_unbind;

	ret = mtk_drm_crtc_create(drm, private->data->third_path_data);
	if (ret < 0)
		goto err_component_unbind;

	/* Use OVL device for all DMA memory allocations */
	np = private->comp_node[private->data->main_path_data
					->path[DDP_MAJOR][0][0]];
	if (np == NULL)
		np = private->comp_node[private->data->ext_path_data
						->path[DDP_MAJOR][0][0]];

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		ret = -ENODEV;
		dev_err(drm->dev, "Need at least one OVL device\n");
		goto err_component_unbind;
	}

	private->dma_dev = &pdev->dev;

	/*
	 * We don't use the drm_irq_install() helpers provided by the DRM
	 * core, so we need to set this manually in order to allow the
	 * DRM_IOCTL_WAIT_VBLANK to operate correctly.
	 */
	drm->irq_enabled = true;
	ret = drm_vblank_init(drm, MAX_CRTC);
	if (ret < 0)
		goto err_component_unbind;

	drm_kms_helper_poll_init(drm);
	drm_mode_config_reset(drm);

	INIT_WORK(&private->unreference.work, mtk_unreference_work);
	INIT_LIST_HEAD(&private->unreference.list);
	INIT_LIST_HEAD(&private->lyeblob_head);
	spin_lock_init(&private->unreference.lock);
	mutex_init(&private->commit.lock);
	mutex_init(&private->lyeblob_list_mutex);

	init_waitqueue_head(&private->repaint_data.wq);
	INIT_LIST_HEAD(&private->repaint_data.job_queue);
	INIT_LIST_HEAD(&private->repaint_data.job_pool);

#ifdef MTK_DRM_FENCE_SUPPORT
	/* fence release kthread */
	init_waitqueue_head(&_mtk_fence_wq);
	private->fence_release_thread =
			kthread_run(mtk_drm_fence_release_thread,
			private, "fence_release_thread");
	if (IS_ERR(private->fence_release_thread)) {
		DDPPR_ERR("Failed to create fence release thread\n");
		goto err_kms_helper_poll_fini;
	}
#endif

#ifdef CONFIG_DRM_MEDIATEK_DEBUG_FS
	mtk_drm_debugfs_init(drm, private);
#endif
	disp_dbg_init(drm);

	mtk_drm_mmdvfs_init();

	DDPINFO("%s-\n", __func__);

	ret = mtk_fbdev_init(drm);
	if (ret)
		goto err_kms_helper_poll_fini;

	mtk_drm_first_enable(drm);

	return 0;
err_kms_helper_poll_fini:
	drm_kms_helper_poll_fini(drm);
err_component_unbind:
	component_unbind_all(drm->dev, drm);
err_config_cleanup:
	drm_mode_config_cleanup(drm);

	return ret;
}

static void mtk_drm_kms_deinit(struct drm_device *drm)
{
	mtk_fbdev_fini(drm);
	drm_kms_helper_poll_fini(drm);

	drm_vblank_cleanup(drm);
	component_unbind_all(drm->dev, drm);
	drm_mode_config_cleanup(drm);

	disp_dbg_deinit();
}

static const struct drm_ioctl_desc mtk_ioctls[] = {
	DRM_IOCTL_DEF_DRV(MTK_GEM_CREATE, mtk_gem_create_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_GEM_MAP_OFFSET, mtk_gem_map_offset_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_GEM_SUBMIT, mtk_gem_submit_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_SESSION_CREATE, mtk_drm_session_create_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_SESSION_DESTROY, mtk_drm_session_destroy_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_LAYERING_RULE, mtk_layering_rule_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_CRTC_GETFENCE, mtk_drm_crtc_getfence_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_WAIT_REPAINT, mtk_drm_wait_repaint_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_GET_DISPLAY_CAPS, mtk_drm_get_display_caps_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_SET_DDP_MODE, mtk_drm_set_ddp_mode,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_GET_SESSION_INFO, mtk_drm_get_info_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_SET_CCORR, mtk_drm_ioctl_set_ccorr,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_CCORR_EVENTCTL, mtk_drm_ioctl_ccorr_eventctl,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_CCORR_GET_IRQ, mtk_drm_ioctl_ccorr_get_irq,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_SUPPORT_COLOR_TRANSFORM,
				mtk_drm_ioctl_support_color_matrix,
				DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_SET_GAMMALUT, mtk_drm_ioctl_set_gammalut,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_SET_PQPARAM, mtk_drm_ioctl_set_pqparam,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_SET_PQINDEX, mtk_drm_ioctl_set_pqindex,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_SET_COLOR_REG, mtk_drm_ioctl_set_color_reg,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_MUTEX_CONTROL, mtk_drm_ioctl_mutex_control,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_READ_REG, mtk_drm_ioctl_read_reg,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_WRITE_REG, mtk_drm_ioctl_write_reg,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_BYPASS_COLOR, mtk_drm_ioctl_bypass_color,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_PQ_SET_WINDOW, mtk_drm_ioctl_pq_set_window,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_WRITE_SW_REG, mtk_drm_ioctl_write_sw_reg,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_READ_SW_REG, mtk_drm_ioctl_read_sw_reg,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_GET_LCM_INDEX, mtk_drm_ioctl_get_lcm_index,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MTK_AAL_INIT_REG, mtk_drm_ioctl_aal_init_reg,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_AAL_GET_HIST, mtk_drm_ioctl_aal_get_hist,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_AAL_SET_PARAM, mtk_drm_ioctl_aal_set_param,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_AAL_EVENTCTL, mtk_drm_ioctl_aal_eventctl,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_AAL_INIT_DRE30, mtk_drm_ioctl_aal_init_dre30,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_AAL_GET_SIZE, mtk_drm_ioctl_aal_get_size,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(MTK_SEC_HND_TO_GEM_HND, mtk_drm_sec_hnd_to_gem_hnd,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
};

static const struct file_operations mtk_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = mtk_drm_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
};

static struct drm_driver mtk_drm_driver = {
	.driver_features =
		DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME | DRIVER_ATOMIC,

	/* .get_vblank_counter = drm_vblank_no_hw_counter, */
	.enable_vblank = mtk_drm_crtc_enable_vblank,
	.disable_vblank = mtk_drm_crtc_disable_vblank,
	.get_vblank_timestamp = mtk_crtc_get_vblank_timestamp,

	.gem_free_object_unlocked = mtk_drm_gem_free_object,
	.gem_vm_ops = &drm_gem_cma_vm_ops,
	.dumb_create = mtk_drm_gem_dumb_create,
	.dumb_map_offset = mtk_drm_gem_dumb_map_offset,
	.dumb_destroy = drm_gem_dumb_destroy,

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_import = mtk_gem_prime_import,
	.gem_prime_get_sg_table = mtk_gem_prime_get_sg_table,
	.gem_prime_import_sg_table = mtk_gem_prime_import_sg_table,
	.gem_prime_mmap = mtk_drm_gem_mmap_buf,
	.ioctls = mtk_ioctls,
	.num_ioctls = ARRAY_SIZE(mtk_ioctls),
	.fops = &mtk_drm_fops,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
};

static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static int mtk_drm_bind(struct device *dev)
{
	struct mtk_drm_private *private = dev_get_drvdata(dev);
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	DDPINFO("%s+\n", __func__);
	drm = drm_dev_alloc(&mtk_drm_driver, dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	drm->dev_private = private;
	private->drm = drm;

	ret = mtk_drm_kms_init(drm);
	if (ret < 0)
		goto err_free;

	ret = drm_dev_register(drm, 0);
	if (ret < 0)
		goto err_deinit;

	mtk_layering_rule_init(drm);
	crtc = list_first_entry(&(drm)->mode_config.crtc_list, typeof(*crtc),
				head);
	mtk_drm_assert_layer_init(crtc);

	DDPINFO("%s-\n", __func__);

	return 0;

err_deinit:
	mtk_drm_kms_deinit(drm);
err_free:
	drm_dev_unref(drm);
	return ret;
}

static void mtk_drm_unbind(struct device *dev)
{
	struct mtk_drm_private *private = dev_get_drvdata(dev);

	drm_dev_unregister(private->drm);
	drm_dev_unref(private->drm);
	private->drm = NULL;
}

static const struct component_master_ops mtk_drm_ops = {
	.bind = mtk_drm_bind, .unbind = mtk_drm_unbind,
};

static const struct of_device_id mtk_ddp_comp_dt_ids[] = {
	{.compatible = "mediatek,mt2701-disp-ovl",
	 .data = (void *)MTK_DISP_OVL},
	{.compatible = "mediatek,mt6779-disp-ovl",
	 .data = (void *)MTK_DISP_OVL},
	{.compatible = "mediatek,mt6873-disp-ovl",
	 .data = (void *)MTK_DISP_OVL},
	{.compatible = "mediatek,mt8173-disp-ovl",
	 .data = (void *)MTK_DISP_OVL},
	{.compatible = "mediatek,mt6885-disp-ovl",
	 .data = (void *)MTK_DISP_OVL},
	{.compatible = "mediatek,mt2701-disp-rdma",
	 .data = (void *)MTK_DISP_RDMA},
	{.compatible = "mediatek,mt6779-disp-rdma",
	 .data = (void *)MTK_DISP_RDMA},
	{.compatible = "mediatek,mt6873-disp-rdma",
	 .data = (void *)MTK_DISP_RDMA},
	{.compatible = "mediatek,mt8173-disp-rdma",
	 .data = (void *)MTK_DISP_RDMA},
	{.compatible = "mediatek,mt6885-disp-rdma",
	 .data = (void *)MTK_DISP_RDMA},
	{.compatible = "mediatek,mt8173-disp-wdma",
	 .data = (void *)MTK_DISP_WDMA},
	{.compatible = "mediatek,mt6779-disp-wdma",
	 .data = (void *)MTK_DISP_WDMA},
	{.compatible = "mediatek,mt6885-disp-wdma",
	 .data = (void *)MTK_DISP_WDMA},
	{.compatible = "mediatek,mt6873-disp-wdma",
	 .data = (void *)MTK_DISP_WDMA},
	{.compatible = "mediatek,mt6779-disp-ccorr",
	 .data = (void *)MTK_DISP_CCORR},
	{.compatible = "mediatek,mt6885-disp-ccorr",
	 .data = (void *)MTK_DISP_CCORR},
	{.compatible = "mediatek,mt6873-disp-ccorr",
	 .data = (void *)MTK_DISP_CCORR},
	{.compatible = "mediatek,mt2701-disp-color",
	 .data = (void *)MTK_DISP_COLOR},
	{.compatible = "mediatek,mt6779-disp-color",
	 .data = (void *)MTK_DISP_COLOR},
	{.compatible = "mediatek,mt6873-disp-color",
	 .data = (void *)MTK_DISP_COLOR},
	{.compatible = "mediatek,mt8173-disp-color",
	 .data = (void *)MTK_DISP_COLOR},
	{.compatible = "mediatek,mt6885-disp-color",
	 .data = (void *)MTK_DISP_COLOR},
	{.compatible = "mediatek,mt6779-disp-aal",
	 .data = (void *)MTK_DISP_AAL},
	{.compatible = "mediatek,mt6873-disp-aal",
	 .data = (void *)MTK_DISP_AAL},
	{.compatible = "mediatek,mt8173-disp-aal",
	 .data = (void *)MTK_DISP_AAL},
	{.compatible = "mediatek,mt6885-disp-aal",
	 .data = (void *)MTK_DISP_AAL},
	{.compatible = "mediatek,mt6779-disp-gamma",
	 .data = (void *)MTK_DISP_GAMMA},
	{.compatible = "mediatek,mt8173-disp-gamma",
	 .data = (void *)MTK_DISP_GAMMA},
	{.compatible = "mediatek,mt6885-disp-gamma",
	 .data = (void *)MTK_DISP_GAMMA},
	{.compatible = "mediatek,mt6873-disp-gamma",
	 .data = (void *)MTK_DISP_GAMMA},
	{.compatible = "mediatek,mt6779-disp-dither",
	 .data = (void *)MTK_DISP_DITHER},
	{.compatible = "mediatek,mt6885-disp-dither",
	 .data = (void *)MTK_DISP_DITHER},
	{.compatible = "mediatek,mt6873-disp-dither",
	 .data = (void *)MTK_DISP_DITHER},
	{.compatible = "mediatek,mt8173-disp-ufoe",
	 .data = (void *)MTK_DISP_UFOE},
	{.compatible = "mediatek,mt2701-dsi",
	 .data = (void *)MTK_DSI},
	{.compatible = "mediatek,mt6779-dsi",
	 .data = (void *)MTK_DSI},
	{.compatible = "mediatek,mt8173-dsi",
	 .data = (void *)MTK_DSI},
	{.compatible = "mediatek,mt6885-dsi",
	 .data = (void *)MTK_DSI},
	{.compatible = "mediatek,mt6873-dsi",
	 .data = (void *)MTK_DSI},
	{.compatible = "mediatek,mt2712-dpi",
	 .data = (void *)MTK_DPI},
	{.compatible = "mediatek,mt6779-dpi",
	 .data = (void *)MTK_DPI},
	{.compatible = "mediatek,mt8173-dpi",
	 .data = (void *)MTK_DPI},
	{.compatible = "mediatek,mt2701-disp-mutex",
	 .data = (void *)MTK_DISP_MUTEX},
	{.compatible = "mediatek,mt2712-disp-mutex",
	 .data = (void *)MTK_DISP_MUTEX},
	{.compatible = "mediatek,mt6779-disp-mutex",
	 .data = (void *)MTK_DISP_MUTEX},
	{.compatible = "mediatek,mt8173-disp-mutex",
	 .data = (void *)MTK_DISP_MUTEX},
	{.compatible = "mediatek,mt6885-disp-mutex",
	 .data = (void *)MTK_DISP_MUTEX},
	{.compatible = "mediatek,mt6873-disp-mutex",
	 .data = (void *)MTK_DISP_MUTEX},
	{.compatible = "mediatek,mt2701-disp-pwm",
	 .data = (void *)MTK_DISP_BLS},
	{.compatible = "mediatek,mt6779-disp-pwm",
	 .data = (void *)MTK_DISP_PWM},
	{.compatible = "mediatek,mt8173-disp-pwm",
	 .data = (void *)MTK_DISP_PWM},
	{.compatible = "mediatek,mt6885-disp-pwm",
	 .data = (void *)MTK_DISP_PWM},
	{.compatible = "mediatek,mt6873-disp-pwm",
	 .data = (void *)MTK_DISP_PWM},
	{.compatible = "mediatek,mt8173-disp-od",
	 .data = (void *)MTK_DISP_OD},
	{.compatible = "mediatek,mt6779-disp-rsz",
	 .data = (void *)MTK_DISP_RSZ},
	{.compatible = "mediatek,mt6885-disp-rsz",
	 .data = (void *)MTK_DISP_RSZ},
	{.compatible = "mediatek,mt6873-disp-rsz",
	 .data = (void *)MTK_DISP_RSZ},
	{.compatible = "mediatek,mt6779-disp-postmask",
	 .data = (void *)MTK_DISP_POSTMASK},
	{.compatible = "mediatek,mt6885-disp-postmask",
	 .data = (void *)MTK_DISP_POSTMASK},
	{.compatible = "mediatek,mt6873-disp-postmask",
	 .data = (void *)MTK_DISP_POSTMASK},
	{.compatible = "mediatek,mt6885-disp-dsc",
	 .data = (void *)MTK_DISP_DSC},
	{.compatible = "mediatek,mt6873-disp-dsc",
	 .data = (void *)MTK_DISP_DSC},
	{.compatible = "mediatek,mt6885-dmdp-aal",
	 .data = (void *)MTK_DMDP_AAL},
	{.compatible = "mediatek,mt6873-dmdp-aal",
	 .data = (void *)MTK_DMDP_AAL},
	{} };

static int mtk_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_drm_private *private;
	struct resource *mem;
	struct device_node *node;
	struct component_match *match = NULL;
	int ret;
	int i;

	disp_dbg_probe();

	DDPINFO("%s+\n", __func__);

	drm_debug = 0x2; /* DRIVER messages */
	private = devm_kzalloc(dev, sizeof(*private), GFP_KERNEL);
	if (!private)
		return -ENOMEM;

	private->data = of_device_get_match_data(dev);

	private->reg_data = mtk_ddp_get_mmsys_reg_data(private->data->mmsys_id);
	if (IS_ERR(private->reg_data)) {
		ret = PTR_ERR(private->config_regs);
		pr_info("Failed to get mmsys register data: %d\n", ret);
		return ret;
	}

	mutex_init(&private->commit.lock);
	INIT_WORK(&private->commit.work, mtk_atomic_work);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	private->config_regs = devm_ioremap_resource(dev, mem);
	if (IS_ERR(private->config_regs)) {
		ret = PTR_ERR(private->config_regs);
		dev_err(dev, "Failed to ioremap mmsys-config resource: %d\n",
			ret);
		return ret;
	}

	private->config_regs_pa = mem->start;
	private->mmsys_dev = dev;

	/* Get and enable top clk align to HW */
	mtk_drm_get_top_clk(private);

#if defined(CONFIG_MTK_IOMMU_V2)
	private->client = mtk_drm_gem_ion_create_client("disp_mm");
#endif

#ifdef MTK_FB_MMDVFS_SUPPORT
	plist_head_init(&private->bw_request_list);
	plist_head_init(&private->hrt_request_list);
	mm_qos_add_request(&private->hrt_request_list, &private->hrt_bw_request,
			   get_virtual_port(VIRTUAL_DISP));
#endif

	/* Iterate over sibling DISP function blocks */
	for_each_child_of_node(dev->of_node->parent, node) {
		const struct of_device_id *of_id;
		enum mtk_ddp_comp_type comp_type;
		enum mtk_ddp_comp_id comp_id;

		of_id = of_match_node(mtk_ddp_comp_dt_ids, node);
		if (!of_id)
			continue;

		if (!of_device_is_available(node)) {
			dev_dbg(dev, "Skipping disabled component %s\n",
				node->full_name);
			continue;
		}

		comp_type = (enum mtk_ddp_comp_type)of_id->data;

		if (comp_type == MTK_DISP_MUTEX) {
			private->mutex_node = of_node_get(node);
			continue;
		}

		DDPINFO("%s line:%d, comp_type:%d\n", __func__, __LINE__,
			comp_type);

		comp_id = mtk_ddp_comp_get_id(node, comp_type);
		if ((int)comp_id < 0) {
			dev_warn(dev, "Skipping unknown component %s\n",
				 node->full_name);
			continue;
		}
		DDPINFO("%s line:%d, comp_id:%d\n", __func__, __LINE__,
			comp_id);
		private->comp_node[comp_id] = of_node_get(node);

		/*
		 * Currently only the COLOR, OVL, RDMA, DSI, and DPI blocks have
		 * separate component platform drivers and initialize their own
		 * DDP component structure. The others are initialized here.
		 */
		if (comp_type == MTK_DISP_COLOR ||
		    comp_type == MTK_DISP_CCORR ||
		    comp_type == MTK_DISP_GAMMA || comp_type == MTK_DISP_AAL ||
		    comp_type == MTK_DISP_DITHER || comp_type == MTK_DISP_OVL ||
		    comp_type == MTK_DISP_RDMA || comp_type == MTK_DISP_WDMA ||
		    comp_type == MTK_DISP_RSZ ||
		    comp_type == MTK_DISP_POSTMASK || comp_type == MTK_DSI ||
		    comp_type == MTK_DPI || comp_type == MTK_DISP_DSC ||
		    comp_type == MTK_DMDP_AAL) {
			dev_info(dev, "Adding component match for %s\n",
				 node->full_name);
			component_match_add(dev, &match, compare_of, node);
		} else {
			struct mtk_ddp_comp *comp;

			comp = devm_kzalloc(dev, sizeof(*comp), GFP_KERNEL);
			if (!comp) {
				ret = -ENOMEM;
				goto err_node;
			}

			ret = mtk_ddp_comp_init(dev, node, comp, comp_id, NULL);
			if (ret)
				goto err_node;

		      private
			->ddp_comp[comp_id] = comp;
		}
	}

	if (!private->mutex_node) {
		dev_err(dev, "Failed to find disp-mutex node\n");
		ret = -ENODEV;
		goto err_node;
	}

	pm_runtime_enable(dev);

	platform_set_drvdata(pdev, private);

	ret = component_master_add_with_match(dev, &mtk_drm_ops, match);
	DDPINFO("%s- ret:%d\n", __func__, ret);
	if (ret)
		goto err_pm;

	mtk_fence_init();
	mtk_drm_helper_init(dev, &private->helper_opt);
	DDPINFO("%s-\n", __func__);

	disp_dts_gpio_init(dev, private);
#ifdef VENDOR_EDIT
	/* Yongpeng.Yi@PSW.MultiMedia.Display.LCD.Feature, 2018/09/10, Add for sau and silence close backlight */
	if ((oppo_boot_mode == OPPO_SILENCE_BOOT)
			||(get_boot_mode() == OPPO_SAU_BOOT))
	{
		printk("%s OPPO_SILENCE_BOOT set silence_mode to 1\n", __func__);
		silence_mode = 1;
	}
#endif
	return 0;

err_pm:
	pm_runtime_disable(dev);
err_node:
	of_node_put(private->mutex_node);
	for (i = 0; i < DDP_COMPONENT_ID_MAX; i++)
		of_node_put(private->comp_node[i]);
	return ret;
}

static void mtk_drm_shutdown(struct platform_device *pdev)
{
	struct mtk_drm_private *private = platform_get_drvdata(pdev);
	struct drm_device *drm = private->drm;

	if (drm) {
		DDPMSG("%s\n", __func__);
		drm_atomic_helper_shutdown(drm);
	}
}

static int mtk_drm_remove(struct platform_device *pdev)
{
	struct mtk_drm_private *private = platform_get_drvdata(pdev);
	struct drm_device *drm = private->drm;
	int i;

	drm_dev_unregister(drm);
	mtk_drm_kms_deinit(drm);
	drm_dev_unref(drm);

	component_master_del(&pdev->dev, &mtk_drm_ops);
	pm_runtime_disable(&pdev->dev);
	of_node_put(private->mutex_node);
	for (i = 0; i < DDP_COMPONENT_ID_MAX; i++)
		of_node_put(private->comp_node[i]);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int mtk_drm_sys_suspend(struct device *dev)
{
	struct mtk_drm_private *private = dev_get_drvdata(dev);
	struct drm_device *drm = private->drm;

	drm_kms_helper_poll_disable(drm);

	private->suspend_state = drm_atomic_helper_suspend(drm);
	if (IS_ERR(private->suspend_state)) {
		drm_kms_helper_poll_enable(drm);
		return PTR_ERR(private->suspend_state);
	}
#ifdef MTK_DRM_FENCE_SUPPORT
	mtk_drm_suspend_release_fence(dev);
#endif

	DRM_DEBUG_DRIVER("mtk_drm_sys_suspend\n");
	return 0;
}

static int mtk_drm_sys_resume(struct device *dev)
{
	struct mtk_drm_private *private = dev_get_drvdata(dev);
	struct drm_device *drm;

	drm = private->drm;
	if (!drm)
		return 0;

	if (!private->suspend_state) {
		DDPINFO("suspend_state is null\n");
		return 0;
	}

	drm_atomic_helper_resume(drm, private->suspend_state);
	drm_kms_helper_poll_enable(drm);

	DRM_DEBUG_DRIVER("mtk_drm_sys_resume\n");
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(mtk_drm_pm_ops, mtk_drm_sys_suspend,
			 mtk_drm_sys_resume);

static const struct of_device_id mtk_drm_of_ids[] = {
	{.compatible = "mediatek,mt2701-mmsys",
	 .data = &mt2701_mmsys_driver_data},
	{.compatible = "mediatek,mt2712-display",
	 .data = &mt2712_mmsys_driver_data},
	{.compatible = "mediatek,mt6779-mmsys",
	 .data = &mt6779_mmsys_driver_data},
	{.compatible = "mediatek,mt8173-mmsys",
	 .data = &mt8173_mmsys_driver_data},
	{.compatible = "mediatek,mt6885-mmsys",
	 .data = &mt6885_mmsys_driver_data},
	{.compatible = "mediatek,mt6873-mmsys",
	 .data = &mt6873_mmsys_driver_data},
	{} };

static struct platform_driver mtk_drm_platform_driver = {
	.probe = mtk_drm_probe,
	.remove = mtk_drm_remove,
	.shutdown = mtk_drm_shutdown,
	.driver = {

			.name = "mediatek-drm",
			.of_match_table = mtk_drm_of_ids,
			.pm = &mtk_drm_pm_ops,
		},
};

static struct platform_driver *const mtk_drm_drivers[] = {
	&mtk_drm_platform_driver,
	&mtk_ddp_driver,
	&mtk_disp_color_driver,
	&mtk_disp_ccorr_driver,
	&mtk_disp_gamma_driver,
	&mtk_disp_aal_driver,
	&mtk_dmdp_aal_driver,
	&mtk_disp_postmask_driver,
	&mtk_disp_dither_driver,
	&mtk_disp_ovl_driver,
	&mtk_disp_rdma_driver,
	&mtk_disp_wdma_driver,
	&mtk_disp_rsz_driver,
	&mtk_dsi_driver,
	&mtk_mipi_tx_driver,
#ifdef CONFIG_DRM_MEDIATEK_HDMI
	&mtk_dpi_driver,
	&mtk_lvds_driver,
	&mtk_lvds_tx_driver,
#endif
	&mtk_disp_dsc_driver,
};

static int __init mtk_drm_init(void)
{
	int ret;
	int i;

	DDPINFO("%s+\n", __func__);
	for (i = 0; i < ARRAY_SIZE(mtk_drm_drivers); i++) {
		ret = platform_driver_register(mtk_drm_drivers[i]);
		if (ret < 0) {
			DDPPR_ERR("Failed to register %s driver: %d\n",
				  mtk_drm_drivers[i]->driver.name, ret);
			goto err;
		}
	}
	DDPINFO("%s-\n", __func__);

	return 0;

err:
	while (--i >= 0)
		platform_driver_unregister(mtk_drm_drivers[i]);

	return ret;
}

static void __exit mtk_drm_exit(void)
{
	int i;

	for (i = ARRAY_SIZE(mtk_drm_drivers) - 1; i >= 0; i--)
		platform_driver_unregister(mtk_drm_drivers[i]);
}
module_init(mtk_drm_init);
module_exit(mtk_drm_exit);

MODULE_AUTHOR("YT SHEN <yt.shen@mediatek.com>");
MODULE_DESCRIPTION("Mediatek SoC DRM driver");
MODULE_LICENSE("GPL v2");
