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
 * Copyright 2020, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */
#include "DRW_render.h"

#include "draw_cache_impl.h"

#include "DNA_space_types.h"

#include "UI_resources.h"

#include "BLI_math_color.h"

#include "overlay2d_engine.h"
#include "overlay2d_private.h"

void OVERLAY2D_background_engine_init(OVERLAY2D_Data *vedata)
{
  OVERLAY2D_StorageList *stl = vedata->stl;
  OVERLAY2D_PrivateData *pd = stl->pd;

  pd->background.do_transparency_checkerboard = true;
}

void OVERLAY2D_background_cache_init(OVERLAY2D_Data *vedata)
{
  OVERLAY2D_PassList *psl = vedata->psl;
  DefaultTextureList *dtxl = DRW_viewport_texture_list_get();

  /* solid background */
  {
    DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_BACKGROUND;
    DRW_PASS_CREATE(psl->background, state);
    GPUShader *sh = OVERLAY2D_shaders_background_get();
    DRWShadingGroup *grp = DRW_shgroup_create(sh, psl->background);
    DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
    DRW_shgroup_uniform_texture_ref(grp, "colorBuffer", &dtxl->color);
    DRW_shgroup_uniform_texture_ref(grp, "depthBuffer", &dtxl->depth);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }
}

void OVERLAY2D_background_draw_scene(OVERLAY2D_Data *vedata)
{
  OVERLAY2D_PassList *psl = vedata->psl;

  DRW_draw_pass(psl->background);
}