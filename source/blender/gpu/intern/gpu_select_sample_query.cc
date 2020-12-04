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
 * The Original Code is Copyright (C) 2014 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup gpu
 *
 * Interface for accessing gpu-related methods for selection. The semantics will be
 * similar to glRenderMode(GL_SELECT) since the goal is to maintain compatibility.
 */

#include <cstdlib>

#include "GPU_debug.h"
#include "GPU_framebuffer.h"
#include "GPU_select.h"
#include "GPU_state.h"

#include "MEM_guardedalloc.h"

#include "BLI_rect.h"

#include "BLI_bitmap.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "gpu_backend.hh"
#include "gpu_query.hh"

#include "gpu_select_private.h"

using namespace blender;
using namespace blender::gpu;

struct GPUSelectQueryState {
  /* Tracks whether a query has been issued so that gpu_load_id can end the previous one */
  bool query_issued;
  /* GPU queries abstraction. Contains an array of queries. */
  QueryPool *queries;
  /* Array holding the id corresponding id to each query. */
  Vector<uint> *ids;
  /* cache on initialization */
  uint (*buffer)[4];
  /* buffer size (stores number of integers, for actual size multiply by sizeof integer)*/
  uint bufsize;
  /* mode of operation */
  char mode;
  uint index;
  int oldhits;

  /* Previous state to restore after drawing. */
  int viewport[4];
  int scissor[4];
  eGPUWriteMask write_mask;
  eGPUDepthTest depth_test;
};

static GPUSelectQueryState g_query_state = {false};

void gpu_select_query_begin(
    uint (*buffer)[4], uint bufsize, const rcti *input, char mode, int oldhits)
{
  GPU_debug_group_begin("Selection Queries");

  g_query_state.query_issued = false;
  g_query_state.bufsize = bufsize;
  g_query_state.buffer = buffer;
  g_query_state.mode = mode;
  g_query_state.index = 0;
  g_query_state.oldhits = oldhits;

  g_query_state.ids = new Vector<uint>();
  g_query_state.queries = GPUBackend::get()->querypool_alloc();
  g_query_state.queries->init(GPU_QUERY_OCCLUSION);

  g_query_state.write_mask = GPU_write_mask_get();
  g_query_state.depth_test = GPU_depth_test_get();
  GPU_scissor_get(g_query_state.scissor);
  GPU_viewport_size_get_i(g_query_state.viewport);

  /* Write to color buffer. Seems to fix issues with selecting alpha blended geom (see T7997). */
  GPU_color_mask(true, true, true, true);

  /* In order to save some fill rate we minimize the viewport using rect.
   * We need to get the region of the viewport so that our geometry doesn't
   * get rejected before the depth test. Should probably cull rect against
   * the viewport but this is a rare case I think */

  int viewport[4] = {
      UNPACK2(g_query_state.viewport), BLI_rcti_size_x(input), BLI_rcti_size_y(input)};

  GPU_viewport(UNPACK4(viewport));
  GPU_scissor(UNPACK4(viewport));
  GPU_scissor_test(false);

  /* occlusion queries operates on fragments that pass tests and since we are interested on all
   * objects in the view frustum independently of their order, we need to disable the depth test */
  if (mode == GPU_SELECT_ALL) {
    /* glQueries on Windows+Intel drivers only works with depth testing turned on.
     * See T62947 for details */
    GPU_depth_test(GPU_DEPTH_ALWAYS);
    GPU_depth_mask(true);
  }
  else if (mode == GPU_SELECT_NEAREST_FIRST_PASS) {
    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
    GPU_depth_mask(true);
    GPU_clear_depth(1.0f);
  }
  else if (mode == GPU_SELECT_NEAREST_SECOND_PASS) {
    GPU_depth_test(GPU_DEPTH_EQUAL);
    GPU_depth_mask(false);
  }
}

bool gpu_select_query_load_id(uint id)
{
  if (g_query_state.query_issued) {
    g_query_state.queries->end_query();
  }

  g_query_state.queries->begin_query();
  g_query_state.ids->append(id);
  g_query_state.query_issued = true;

  if (g_query_state.mode == GPU_SELECT_NEAREST_SECOND_PASS) {
    /* Second pass should never run if first pass fails, can read past 'bufsize' in this case. */
    BLI_assert(g_query_state.oldhits != -1);
    if (g_query_state.index < g_query_state.oldhits) {
      if (g_query_state.buffer[g_query_state.index][3] == id) {
        g_query_state.index++;
        return true;
      }
      return false;
    }
  }
  return true;
}

uint gpu_select_query_end(void)
{
  uint hits = 0;
  const uint maxhits = g_query_state.bufsize;

  if (g_query_state.query_issued) {
    g_query_state.queries->end_query();
  }

  Span<uint> ids = *g_query_state.ids;
  Vector<uint32_t> result(ids.size());
  g_query_state.queries->get_occlusion_result(result);

  for (int i = 0; i < result.size(); i++) {
    if (result[i] != 0) {
      if (g_query_state.mode != GPU_SELECT_NEAREST_SECOND_PASS) {
        if (hits < maxhits) {
          g_query_state.buffer[hits][0] = 1;
          g_query_state.buffer[hits][1] = 0xFFFF;
          g_query_state.buffer[hits][2] = 0xFFFF;
          g_query_state.buffer[hits][3] = ids[i];
          hits++;
        }
        else {
          hits = -1;
          break;
        }
      }
      else {
        int j;
        /* search in buffer and make selected object first */
        for (j = 0; j < g_query_state.oldhits; j++) {
          if (g_query_state.buffer[j][3] == ids[i]) {
            g_query_state.buffer[j][1] = 0;
            g_query_state.buffer[j][2] = 0;
          }
        }
        break;
      }
    }
  }

  delete g_query_state.queries;
  delete g_query_state.ids;

  GPU_write_mask(g_query_state.write_mask);
  GPU_depth_test(g_query_state.depth_test);
  GPU_viewport(UNPACK4(g_query_state.viewport));

  GPU_debug_group_end();

  return hits;
}
