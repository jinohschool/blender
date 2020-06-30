/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2008 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup spinfo
 */

#include <BKE_global.h>
#include <BKE_main.h>
#include <BKE_report.h>
#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_context.h"
#include "BKE_screen.h"

#include "ED_screen.h"
#include "ED_space_api.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_types.h"

#include "RNA_access.h"

#include "UI_interface.h"
#include "UI_resources.h"
#include "UI_view2d.h"

#include "BLO_readfile.h"
#include "GPU_framebuffer.h"
#include "info_intern.h" /* own include */

/* ******************** default callbacks for info space ***************** */

static SpaceLink *info_new(const ScrArea *UNUSED(area), const Scene *UNUSED(scene))
{
  ARegion *region;
  SpaceInfo *sinfo;

  sinfo = MEM_callocN(sizeof(SpaceInfo), "initinfo");
  sinfo->spacetype = SPACE_INFO;
  sinfo->rpt_mask = INFO_RPT_OP;

  /* header */
  region = MEM_callocN(sizeof(ARegion), "header for info");

  BLI_addtail(&sinfo->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

  /* main region */
  region = MEM_callocN(sizeof(ARegion), "main region for info");

  BLI_addtail(&sinfo->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  /* keep in sync with console */
  region->v2d.scroll |= V2D_SCROLL_RIGHT;
  region->v2d.align |= V2D_ALIGN_NO_NEG_X | V2D_ALIGN_NO_NEG_Y; /* align bottom left */
  region->v2d.keepofs |= V2D_LOCKOFS_X;
  region->v2d.keepzoom = (V2D_LOCKZOOM_X | V2D_LOCKZOOM_Y | V2D_LIMITZOOM | V2D_KEEPASPECT);
  region->v2d.keeptot = V2D_KEEPTOT_BOUNDS;
  region->v2d.minzoom = region->v2d.maxzoom = 1.0f;

  /* for now, aspect ratio should be maintained, and zoom is clamped within sane default limits */
  // region->v2d.keepzoom = (V2D_KEEPASPECT|V2D_LIMITZOOM);

  return (SpaceLink *)sinfo;
}

/* not spacelink itself */
static void info_free(SpaceLink *sl)
{
  SpaceInfo *sinfo = (SpaceInfo *)sl;
  if (sinfo->view == INFO_VIEW_CLOG) {
    BKE_reports_clear(sinfo->active_reports);
    MEM_freeN(sinfo->active_reports);
  }
}

static void info_report_source_update(wmWindowManager *wm, SpaceInfo *sinfo)
{
  switch (sinfo->view) {
    case INFO_VIEW_REPORTS: {
      if (sinfo->active_reports != &wm->reports) {
        /* reports come from log, deinit them*/
        BKE_reports_clear(sinfo->active_reports);
        MEM_freeN(sinfo->active_reports);
      }
      sinfo->active_reports = &wm->reports;
      break;
    }
    case INFO_VIEW_CLOG: {
      if (sinfo->active_reports == &wm->reports) {
        sinfo->active_reports = clog_to_report_list();
      }
      break;
    }
  }
}

/* spacetype; init callback */
static void info_init(struct wmWindowManager *wm, ScrArea *area)
{
  SpaceInfo *sinfo = area->spacedata.first;
  if (sinfo->active_reports == NULL) {
    switch (sinfo->view) {
      case INFO_VIEW_REPORTS: {
        sinfo->active_reports = &wm->reports;
        break;
      }
      case INFO_VIEW_CLOG: {
        sinfo->active_reports = clog_to_report_list();
        break;
      }
    }
  }
}

static SpaceLink *info_duplicate(SpaceLink *sl)
{
  SpaceInfo *sinfo_new = MEM_dupallocN(sl);
  sinfo_new->active_reports = NULL;  // will be reinitialized in info_init

  return (SpaceLink *)sinfo_new;
}

/* add handlers, stuff you only do once or on area/region changes */
static void info_main_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  /* force it on init, for old files, until it becomes config */
  region->v2d.scroll = (V2D_SCROLL_RIGHT);

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_CUSTOM, region->winx, region->winy);

  /* own keymap */
  keymap = WM_keymap_ensure(wm->defaultconf, "Info", SPACE_INFO, 0);
  WM_event_add_keymap_handler(&region->handlers, keymap);
}

static void info_textview_update_rect(const bContext *C, ARegion *region)
{
  SpaceInfo *sinfo = CTX_wm_space_info(C);
  View2D *v2d = &region->v2d;

  UI_view2d_totRect_set(
      v2d, region->winx - 1, info_textview_height(sinfo, region, sinfo->active_reports));
}

static void info_main_region_draw(const bContext *C, ARegion *region)
{
  /* draw entirely, view changes should be handled here */
  SpaceInfo *sinfo = CTX_wm_space_info(C);
  View2D *v2d = &region->v2d;

  /* clear and setup matrix */
  UI_ThemeClearColor(TH_BACK);
  GPU_clear(GPU_COLOR_BIT);

  /* quick way to avoid drawing if not bug enough */
  if (region->winy < 16) {
    return;
  }

  info_textview_update_rect(C, region);

  /* worlks best with no view2d matrix set */
  UI_view2d_view_ortho(v2d);

  info_textview_main(sinfo, region, sinfo->active_reports);

  /* reset view matrix */
  UI_view2d_view_restore(C);

  /* scrollers */
  UI_view2d_scrollers_draw(v2d, NULL);
}

static void info_operatortypes(void)
{
  WM_operatortype_append(FILE_OT_autopack_toggle);
  WM_operatortype_append(FILE_OT_pack_all);
  WM_operatortype_append(FILE_OT_pack_libraries);
  WM_operatortype_append(FILE_OT_unpack_all);
  WM_operatortype_append(FILE_OT_unpack_item);
  WM_operatortype_append(FILE_OT_unpack_libraries);

  WM_operatortype_append(FILE_OT_make_paths_relative);
  WM_operatortype_append(FILE_OT_make_paths_absolute);
  WM_operatortype_append(FILE_OT_report_missing_files);
  WM_operatortype_append(FILE_OT_find_missing_files);
  WM_operatortype_append(INFO_OT_reports_display_update);

  /* info_report.c */
  WM_operatortype_append(INFO_OT_select_pick);
  WM_operatortype_append(INFO_OT_select_all);
  WM_operatortype_append(INFO_OT_select_box);

  WM_operatortype_append(INFO_OT_report_replay);
  WM_operatortype_append(INFO_OT_report_delete);
  WM_operatortype_append(INFO_OT_report_copy);
}

static void info_keymap(struct wmKeyConfig *keyconf)
{
  WM_keymap_ensure(keyconf, "Window", 0, 0);
  WM_keymap_ensure(keyconf, "Info", SPACE_INFO, 0);
}

/* add handlers, stuff you only do once or on area/region changes */
static void info_header_region_init(wmWindowManager *UNUSED(wm), ARegion *region)
{
  ED_region_header_init(region);
}

static void info_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

static void info_main_region_listener(wmWindow *UNUSED(win),
                                      ScrArea *area,
                                      ARegion *region,
                                      wmNotifier *wmn,
                                      const Scene *UNUSED(scene))
{
  SpaceInfo *sinfo = area->spacedata.first;

  /* context changes */
  switch (wmn->category) {
    case NC_SPACE:
      if (wmn->data == ND_SPACE_INFO_REPORT) {
        /* redraw also but only for report view, could do less redraws by checking the type */
        ED_region_tag_redraw(region);
      }
      else if (wmn->data == ND_SPACE_INFO_CHANGE_REPORT_SOURCE) {
        // todo this is very bad
        Main *bmain = G_MAIN;
        wmWindowManager *wm = bmain->wm.first;
        info_report_source_update(wm, sinfo);
        ED_region_tag_redraw(region);
      }
      break;
  }
}

static void info_header_listener(wmWindow *UNUSED(win),
                                 ScrArea *UNUSED(area),
                                 ARegion *region,
                                 wmNotifier *wmn,
                                 const Scene *UNUSED(scene))
{
  /* context changes */
  switch (wmn->category) {
    case NC_SCREEN:
      if (ELEM(wmn->data, ND_LAYER, ND_ANIMPLAY)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_WM:
      if (wmn->data == ND_JOB) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SCENE:
      if (wmn->data == ND_RENDER_RESULT) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_INFO) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_ID:
      if (wmn->action == NA_RENAME) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

static void info_header_region_message_subscribe(const bContext *UNUSED(C),
                                                 WorkSpace *UNUSED(workspace),
                                                 Scene *UNUSED(scene),
                                                 bScreen *UNUSED(screen),
                                                 ScrArea *UNUSED(area),
                                                 ARegion *region,
                                                 struct wmMsgBus *mbus)
{
  wmMsgSubscribeValue msg_sub_value_region_tag_redraw = {
      .owner = region,
      .user_data = region,
      .notify = ED_region_do_msg_notify_tag_redraw,
  };

  WM_msg_subscribe_rna_anon_prop(mbus, Window, view_layer, &msg_sub_value_region_tag_redraw);
  WM_msg_subscribe_rna_anon_prop(mbus, ViewLayer, name, &msg_sub_value_region_tag_redraw);
}

/* only called once, from space/spacetypes.c */
void ED_spacetype_info(void)
{
  SpaceType *st = MEM_callocN(sizeof(SpaceType), "spacetype info");
  ARegionType *art;

  st->spaceid = SPACE_INFO;
  strncpy(st->name, "Info", BKE_ST_MAXNAME);

  st->new = info_new;
  st->free = info_free;
  st->init = info_init;
  st->duplicate = info_duplicate;
  st->operatortypes = info_operatortypes;
  st->keymap = info_keymap;

  /* regions: main window */
  art = MEM_callocN(sizeof(ARegionType), "spacetype info region");
  art->regionid = RGN_TYPE_WINDOW;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_FRAMES;

  art->init = info_main_region_init;
  art->draw = info_main_region_draw;
  art->listener = info_main_region_listener;

  BLI_addhead(&st->regiontypes, art);

  /* regions: header */
  art = MEM_callocN(sizeof(ARegionType), "spacetype info region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;

  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_FRAMES | ED_KEYMAP_HEADER;
  art->listener = info_header_listener;
  art->message_subscribe = info_header_region_message_subscribe;
  art->init = info_header_region_init;
  art->draw = info_header_region_draw;

  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(st);
}
