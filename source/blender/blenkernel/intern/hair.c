/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Lukas Toenne
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/hair.c
 *  \ingroup bke
 */

#include <limits.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_kdtree.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rand.h"
#include "BLI_sort.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.h"

#include "DNA_hair_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BKE_hair.h"
#include "BKE_library.h"
#include "BKE_mesh.h"
#include "BKE_mesh_sample.h"

#include "BLT_translation.h"

HairSystem* BKE_hair_new(void)
{
	HairSystem *hair = MEM_callocN(sizeof(HairSystem), "hair system");
	
	hair->pattern = MEM_callocN(sizeof(HairPattern), "hair pattern");
	
	return hair;
}

HairSystem* BKE_hair_copy(HairSystem *hsys)
{
	HairSystem *nhsys = MEM_dupallocN(hsys);
	
	if (hsys->pattern)
	{
		nhsys->pattern = MEM_dupallocN(hsys->pattern);
		nhsys->pattern->follicles = MEM_dupallocN(hsys->pattern->follicles);
	}
	
	if (hsys->curve_data.curves)
	{
		nhsys->curve_data.curves = MEM_dupallocN(hsys->curve_data.curves);
	}
	if (hsys->curve_data.verts)
	{
		nhsys->curve_data.verts = MEM_dupallocN(hsys->curve_data.verts);
	}
	
	nhsys->draw_batch_cache = NULL;
	nhsys->draw_texture_cache = NULL;
	
	return nhsys;
}

void BKE_hair_free(HairSystem *hsys)
{
	BKE_hair_batch_cache_free(hsys);
	
	if (hsys->curve_data.curves)
	{
		MEM_freeN(hsys->curve_data.curves);
	}
	if (hsys->curve_data.verts)
	{
		MEM_freeN(hsys->curve_data.verts);
	}
	
	if (hsys->pattern)
	{
		if (hsys->pattern->follicles)
		{
			MEM_freeN(hsys->pattern->follicles);
		}
		MEM_freeN(hsys->pattern);
	}
	
	MEM_freeN(hsys);
}

/* Calculate surface area of a scalp mesh */
float BKE_hair_calc_surface_area(const Mesh *scalp)
{
	BLI_assert(scalp != NULL);
	
	int numpolys = scalp->totpoly;
	MPoly *mpolys = scalp->mpoly;
	MLoop *mloops = scalp->mloop;
	MVert *mverts = scalp->mvert;

	float area = 0.0f;
	for (int i = 0; i < numpolys; ++i)
	{
		area += BKE_mesh_calc_poly_area(&mpolys[i], mloops + mpolys[i].loopstart, mverts);
	}
	return area;
}

/* Calculate a density value based on surface area and sample count */
float BKE_hair_calc_density_from_count(float area, int count)
{
	return area > 0.0f ? count / area : 0.0f;
}

/* Calculate maximum sample count based on surface area and density */
int BKE_hair_calc_max_count_from_density(float area, float density)
{
	return (int)(density * area);
}

/* Calculate a density value based on a minimum distance */
float BKE_hair_calc_density_from_min_distance(float min_distance)
{
	// max. circle packing density (sans pi factor): 1 / (2 * sqrt(3))
	static const float max_factor = 0.288675135;
	
	return min_distance > 0.0f ? max_factor / (min_distance * min_distance) : 0.0f;
}

/* Calculate a minimum distance based on density */
float BKE_hair_calc_min_distance_from_density(float density)
{
	// max. circle packing density (sans pi factor): 1 / (2 * sqrt(3))
	static const float max_factor = 0.288675135;
	
	return density > 0.0f ? sqrt(max_factor / density) : 0.0f;
}

/* Distribute hair follicles on a scalp mesh */
void BKE_hair_generate_follicles(
        HairSystem* hsys,
        struct Mesh *scalp,
        unsigned int seed,
        int count)
{
	BKE_hair_generate_follicles_ex(hsys, scalp, seed, count, NULL);
}

/* Distribute hair follicles on a scalp mesh.
 * Optional per-loop weights control follicle density on the scalp.
 */
void BKE_hair_generate_follicles_ex(
        HairSystem* hsys,
        struct Mesh *scalp,
        unsigned int seed,
        int count,
        const float *loop_weights)
{
	HairPattern *pattern = hsys->pattern;
	
	// Limit max_count to theoretical limit based on area
	float scalp_area = BKE_hair_calc_surface_area(scalp);
	float density = BKE_hair_calc_density_from_count(scalp_area, count);
	float min_distance = BKE_hair_calc_min_distance_from_density(density);
	
	if (pattern->follicles)
	{
		MEM_freeN(pattern->follicles);
	}
	pattern->follicles = MEM_callocN(sizeof(HairFollicle) * count, "hair follicles");
	
	{
		MeshSampleGenerator *gen = BKE_mesh_sample_gen_surface_poissondisk(seed, min_distance, count, loop_weights);
		
		BKE_mesh_sample_generator_bind(gen, scalp);
		
		static const bool use_threads = false;
		pattern->num_follicles = BKE_mesh_sample_generate_batch_ex(
		            gen,
		            &pattern->follicles->mesh_sample,
		            sizeof(HairFollicle),
		            count,
		            use_threads);
		
		BKE_mesh_sample_free_generator(gen);
	}
	
	hsys->flag |= HAIR_SYSTEM_UPDATE_FOLLICLE_BINDING;
	BKE_hair_batch_cache_dirty(hsys, BKE_HAIR_BATCH_DIRTY_ALL);
}

/* ================================= */

void BKE_hair_fiber_curves_begin(HairSystem *hsys, int totcurves)
{
	if (totcurves != hsys->curve_data.totcurves)
	{
		hsys->curve_data.curves = MEM_reallocN(hsys->curve_data.curves, sizeof(HairFiberCurve) * totcurves);
		hsys->curve_data.totcurves = totcurves;

		hsys->flag |= HAIR_SYSTEM_UPDATE_FOLLICLE_BINDING;
		BKE_hair_batch_cache_dirty(hsys, BKE_HAIR_BATCH_DIRTY_ALL);
	}
}

void BKE_hair_set_fiber_curve(HairSystem *hsys, int index, const MeshSample *mesh_sample, int numverts,
                              float taper_length, float taper_thickness)
{
	BLI_assert(index <= hsys->curve_data.totcurves);
	
	HairFiberCurve *curve = &hsys->curve_data.curves[index];
	memcpy(&curve->mesh_sample, mesh_sample, sizeof(MeshSample));
	curve->numverts = numverts;
	curve->taper_length = taper_length;
	curve->taper_thickness = taper_thickness;
	
	hsys->flag |= HAIR_SYSTEM_UPDATE_FOLLICLE_BINDING;
	BKE_hair_batch_cache_dirty(hsys, BKE_HAIR_BATCH_DIRTY_ALL);
}

/* Calculate vertex start indices on all curves based on length.
 * Returns the total number of vertices.
 */
static int hair_curve_calc_vertstart(HairSystem *hsys)
{
	/* Recalculate vertex count and start offsets in curves */
	int vertstart = 0;
	for (int i = 0; i < hsys->curve_data.totcurves; ++i)
	{
		hsys->curve_data.curves[i].vertstart = vertstart;
		vertstart += hsys->curve_data.curves[i].numverts;
	}
	
	return vertstart;
}

void BKE_hair_fiber_curves_end(HairSystem *hsys)
{
	const int totverts = hair_curve_calc_vertstart(hsys);

	if (totverts != hsys->curve_data.totverts)
	{
		hsys->curve_data.verts = MEM_reallocN(hsys->curve_data.verts, sizeof(HairFiberVertex) * totverts);
		hsys->curve_data.totverts = totverts;

		BKE_hair_batch_cache_dirty(hsys, BKE_HAIR_BATCH_DIRTY_ALL);
	}
}

void BKE_hair_set_fiber_vertex(HairSystem *hsys, int index, int flag, const float co[3])
{
	BLI_assert(index <= hsys->curve_data.totverts);
	
	HairFiberVertex *vertex = &hsys->curve_data.verts[index];
	vertex->flag = flag;
	copy_v3_v3(vertex->co, co);
	
	BKE_hair_batch_cache_dirty(hsys, BKE_HAIR_BATCH_DIRTY_ALL);
}

void BKE_hair_set_fiber_curves(HairSystem *hsys, HairCurveData *curves)
{
	if (hsys->curve_data.curves)
	{
		MEM_freeN(hsys->curve_data.curves);
	}
	hsys->curve_data.curves = MEM_dupallocN(hsys->curve_data.curves);
	hsys->curve_data.totcurves = curves->totcurves;

	if (hsys->curve_data.verts)
	{
		MEM_freeN(hsys->curve_data.verts);
	}
	hsys->curve_data.verts = MEM_dupallocN(hsys->curve_data.verts);
	hsys->curve_data.totverts = curves->totverts;

#ifndef NDEBUG
	const int vertcount = hair_curve_calc_vertstart(hsys);
	BLI_assert(vertcount <= hsys->curve_data.totverts);
#endif

	hsys->flag |= HAIR_SYSTEM_UPDATE_FOLLICLE_BINDING;
	BKE_hair_batch_cache_dirty(hsys, BKE_HAIR_BATCH_DIRTY_ALL);
}

void BKE_hair_clear_fiber_curves(HairSystem *hsys)
{
	if (hsys->curve_data.curves)
	{
		MEM_freeN(hsys->curve_data.curves);
		hsys->curve_data.curves = NULL;
	}
	hsys->curve_data.totcurves = 0;

	if (hsys->curve_data.verts)
	{
		MEM_freeN(hsys->curve_data.verts);
		hsys->curve_data.verts = NULL;
	}
	hsys->curve_data.totverts = 0;

	hsys->flag &= ~HAIR_SYSTEM_UPDATE_FOLLICLE_BINDING;
	BKE_hair_batch_cache_dirty(hsys, BKE_HAIR_BATCH_DIRTY_ALL);
}

/* ================================= */

BLI_INLINE void hair_fiber_verify_weights(HairFollicle *follicle)
{
	const float *w = follicle->parent_weight;
	
	BLI_assert(w[0] >= 0.0f && w[1] >= 0.0f && w[2] >= 0.0f && w[3] >= 0.0f);
	float sum = w[0] + w[1] + w[2] + w[3];
	float epsilon = 1.0e-2;
	BLI_assert(sum > 1.0f - epsilon && sum < 1.0f + epsilon);
	UNUSED_VARS(sum, epsilon);
	
	BLI_assert(w[0] >= w[1] && w[1] >= w[2] && w[2] >= w[3]);
}

static void hair_fiber_sort_weights(HairFollicle *follicle)
{
	unsigned int *idx = follicle->parent_index;
	float *w = follicle->parent_weight;

#define FIBERSWAP(a, b) \
	SWAP(unsigned int, idx[a], idx[b]); \
	SWAP(float, w[a], w[b]);

	for (int k = 0; k < 3; ++k) {
		int maxi = k;
		float maxw = w[k];
		for (int i = k+1; i < 4; ++i) {
			if (w[i] > maxw) {
				maxi = i;
				maxw = w[i];
			}
		}
		if (maxi != k)
			FIBERSWAP(k, maxi);
	}
	
#undef FIBERSWAP
}

static void hair_fiber_find_closest_strand(
        HairFollicle *follicle,
        const float loc[3],
        const KDTree *tree,
        const float (*strandloc)[3])
{
	/* Use the 3 closest strands for interpolation.
	 * Note that we have up to 4 possible weights, but we
	 * only look for a triangle with this method.
	 */
	KDTreeNearest nearest[3];
	const float *sloc[3] = {NULL};
	int k, found = BLI_kdtree_find_nearest_n(tree, loc, nearest, 3);
	for (k = 0; k < found; ++k) {
		follicle->parent_index[k] = (unsigned int)nearest[k].index;
		sloc[k] = strandloc[nearest[k].index];
	}
	for (; k < 4; ++k) {
		follicle->parent_index[k] = HAIR_STRAND_INDEX_NONE;
		follicle->parent_weight[k] = 0.0f;
	}
	
	/* calculate barycentric interpolation weights */
	if (found == 3) {
		float closest[3];
		closest_on_tri_to_point_v3(closest, loc, sloc[0], sloc[1], sloc[2]);
		
		float w[3];
		interp_weights_tri_v3(w, sloc[0], sloc[1], sloc[2], closest);
		copy_v3_v3(follicle->parent_weight, w);
		/* float precisions issues can cause slightly negative weights */
		CLAMP3(follicle->parent_weight, 0.0f, 1.0f);
	}
	else if (found == 2) {
		follicle->parent_weight[1] = line_point_factor_v3(loc, sloc[0], sloc[1]);
		follicle->parent_weight[0] = 1.0f - follicle->parent_weight[1];
		/* float precisions issues can cause slightly negative weights */
		CLAMP2(follicle->parent_weight, 0.0f, 1.0f);
	}
	else if (found == 1) {
		follicle->parent_weight[0] = 1.0f;
	}
	
	hair_fiber_sort_weights(follicle);
}

bool BKE_hair_bind_follicles(HairSystem *hsys, const Mesh *scalp)
{
	if (!(hsys->flag & HAIR_SYSTEM_UPDATE_FOLLICLE_BINDING))
	{
		return true;
	}
	hsys->flag &= ~HAIR_SYSTEM_UPDATE_FOLLICLE_BINDING;
	
	HairPattern *pattern = hsys->pattern;
	if (!pattern)
	{
		return true;
	}
	
	const int num_strands = hsys->curve_data.totcurves;
	/* Need at least one curve for binding */
	if (num_strands == 0)
	{
		HairFollicle *follicle = pattern->follicles;
		for (int i = 0; i < pattern->num_follicles; ++i, ++follicle)
		{
			for (int k = 0; k < 4; ++k)
			{
				follicle->parent_index[k] = HAIR_STRAND_INDEX_NONE;
				follicle->parent_weight[k] = 0.0f;
			}
		}
		return false;
	}
	
	float (*strandloc)[3] = MEM_mallocN(sizeof(float) * 3 * num_strands, "strand locations");
	{
		for (int i = 0; i < num_strands; ++i)
		{
			float nor[3], tang[3];
			if (!BKE_mesh_sample_eval(scalp, &hsys->curve_data.curves[i].mesh_sample, strandloc[i], nor, tang))
			{
				zero_v3(strandloc[i]);
			}
		}
	}
	
	KDTree *tree = BLI_kdtree_new(num_strands);
	for (int c = 0; c < num_strands; ++c)
	{
		BLI_kdtree_insert(tree, c, strandloc[c]);
	}
	BLI_kdtree_balance(tree);
	
	{
		HairFollicle *follicle = pattern->follicles;
		for (int i = 0; i < pattern->num_follicles; ++i, ++follicle)
		{
			float loc[3], nor[3], tang[3];
			if (BKE_mesh_sample_eval(scalp, &follicle->mesh_sample, loc, nor, tang))
			{
				hair_fiber_find_closest_strand(follicle, loc, tree, strandloc);
				hair_fiber_verify_weights(follicle);
			}
		}
	}
	
	BLI_kdtree_free(tree);
	MEM_freeN(strandloc);
	
	return true;
}

/* === Export === */

/* Returns number of vertices in a curve after subdivision */
BLI_INLINE int hair_get_strand_subdiv_length(int orig_length, int subdiv)
{
	return ((orig_length - 1) << subdiv) + 1;
}

/* Returns total number of vertices after subdivision */
BLI_INLINE int hair_get_strand_subdiv_numverts(int numstrands, int numverts, int subdiv)
{
	return ((numverts - numstrands) << subdiv) + numstrands;
}

/* Subdivide a curve */
static int hair_curve_subdivide(const HairFiberCurve* curve, const HairFiberVertex* verts,
                                int subdiv, const float rootpos[3], HairFiberVertex *r_verts)
{
	{
		/* Move vertex positions from the dense array to their initial configuration for subdivision.
		 * Also add offset to ensure the curve starts on the scalp surface.
		 */
		const int step = (1 << subdiv);
		
		BLI_assert(curve->numverts > 0);
		float offset[3];
		sub_v3_v3v3(offset, rootpos, verts[0].co);
		
		HairFiberVertex *dst = r_verts;
		for (int i = 0; i < curve->numverts; ++i) {
			add_v3_v3v3(dst->co, verts[i].co, offset);
			dst += step;
		}
	}
	
	/* Subdivide */
	for (int d = 0; d < subdiv; ++d) {
		const int num_edges = (curve->numverts - 1) << d;
		const int hstep = 1 << (subdiv - d - 1);
		const int step = 1 << (subdiv - d);
		
		/* Calculate edge points */
		{
			int index = 0;
			for (int k = 0; k < num_edges; ++k, index += step) {
				add_v3_v3v3(r_verts[index + hstep].co, r_verts[index].co, r_verts[index + step].co);
				mul_v3_fl(r_verts[index + hstep].co, 0.5f);
			}
		}
		
		/* Move original points */
		{
			int index = step;
			for (int k = 1; k < num_edges; ++k, index += step) {
				add_v3_v3v3(r_verts[index].co, r_verts[index - hstep].co, r_verts[index + hstep].co);
				mul_v3_fl(r_verts[index].co, 0.5f);
			}
		}
	}
	
	const int num_verts = ((curve->numverts - 1) << subdiv) + 1;
	return num_verts;
}

/* Calculate tangent and normal vector changes from one segment to the next */
static void hair_curve_transport_frame(const float co1[3], const float co2[3],
                                       float prev_tang[3], float prev_nor[3],
                                       float r_tang[3], float r_nor[3])
{
	/* segment direction */
	sub_v3_v3v3(r_tang, co2, co1);
	normalize_v3(r_tang);
	
	/* rotate the frame */
	float rot[3][3];
	rotation_between_vecs_to_mat3(rot, prev_tang, r_tang);
	mul_v3_m3v3(r_nor, rot, prev_nor);
	
	copy_v3_v3(prev_tang, r_tang);
	copy_v3_v3(prev_nor, r_nor);
}

/* Calculate tangent and normal vectors for all vertices on a curve */
static void hair_curve_calc_vectors(const HairFiberVertex* verts, int numverts, float rootmat[3][3],
                                    float (*r_tangents)[3], float (*r_normals)[3])
{
	BLI_assert(numverts >= 2);
	
	float prev_tang[3], prev_nor[3];
	
	copy_v3_v3(prev_tang, rootmat[2]);
	copy_v3_v3(prev_nor, rootmat[0]);
	
	hair_curve_transport_frame(
	        verts[0].co, verts[1].co,
	        prev_tang, prev_nor,
	        r_tangents[0], r_normals[0]);
	
	for (int i = 1; i < numverts - 1; ++i)
	{
		hair_curve_transport_frame(
		        verts[i-1].co, verts[i+1].co,
		        prev_tang, prev_nor,
		        r_tangents[i], r_normals[i]);
	}
	
	hair_curve_transport_frame(
	        verts[numverts-2].co, verts[numverts-1].co,
	        prev_tang, prev_nor,
	        r_tangents[numverts-1], r_normals[numverts-1]);
}

/* Create a new export cache.
 * This can be used to construct full fiber data for rendering.
 */

HairExportCache* BKE_hair_export_cache_new(void)
{
	HairExportCache *cache = MEM_callocN(sizeof(HairExportCache), "hair export cache");
	return cache;
}

/* Returns flags for missing data parts */

static int hair_export_cache_get_required_updates(const HairExportCache *cache)
{
	int data = 0;
	if (!cache->fiber_curves)
	{
		data |= HAIR_EXPORT_FIBER_CURVES;
	}
	if (!cache->fiber_verts || !cache->fiber_normals || !cache->fiber_tangents)
	{
		data |= HAIR_EXPORT_FIBER_VERTICES;
	}
	if (!cache->follicles)
	{
		data |= HAIR_EXPORT_FOLLICLE_BINDING;
	}
	if (!cache->fiber_root_position)
	{
		data |= HAIR_EXPORT_FIBER_ROOT_POSITIONS;
	}
	if (!cache->fiber_numverts)
	{
		data |= HAIR_EXPORT_FIBER_VERTEX_COUNTS;
	}
	return data;
}

/* Include data dependencies of the given flags */

static int hair_export_cache_get_dependencies(int data)
{
	/* Ordering here is important to account for recursive dependencies */
	
	if (data & HAIR_EXPORT_FIBER_CURVES)
		data |= HAIR_EXPORT_FIBER_VERTICES | HAIR_EXPORT_FOLLICLE_BINDING;
	
	if (data & HAIR_EXPORT_FOLLICLE_BINDING)
		data |= HAIR_EXPORT_FIBER_ROOT_POSITIONS | HAIR_EXPORT_FIBER_VERTEX_COUNTS;
	
	return data;
}

/* Update an existing export cache to ensure it contains the requested data.
 * Returns flags for data that has been updated.
 */

int BKE_hair_export_cache_update(HairExportCache *cache, const HairSystem *hsys,
                                 int subdiv, Mesh *scalp, int requested_data)
{
	/* Include dependencies */
	int data = hair_export_cache_get_dependencies(requested_data);
	
	int uncached = hair_export_cache_get_required_updates(cache);
	/* Invalid data should already include all dependencies */
	BLI_assert(uncached == hair_export_cache_get_dependencies(uncached));
	
	/* Only update invalidated parts */
	data &= uncached;
	
	if (data & HAIR_EXPORT_FIBER_CURVES)
	{
		/* Cache subdivided curves */
		const int totcurves = cache->totcurves = hsys->curve_data.totcurves;
		cache->fiber_curves = MEM_reallocN_id(cache->fiber_curves, sizeof(HairFiberCurve) * totcurves, "hair export curves");
		
		int totverts = 0;
		for (int i = 0; i < totcurves; ++i) {
			const HairFiberCurve *curve_orig = &hsys->curve_data.curves[i];
			HairFiberCurve *curve = &cache->fiber_curves[i];
			
			memcpy(curve, curve_orig, sizeof(HairFiberCurve));
			curve->numverts = hair_get_strand_subdiv_length(curve_orig->numverts, subdiv);
			curve->vertstart = totverts;
			
			totverts  += curve->numverts;
		}
		cache->totverts = totverts;
	}
	
	if (data & HAIR_EXPORT_FIBER_VERTICES)
	{
		const int totcurves = cache->totcurves;
		const int totverts = cache->totverts;
		cache->fiber_verts = MEM_reallocN_id(cache->fiber_verts, sizeof(HairFiberVertex) * totverts, "hair export verts");
		cache->fiber_tangents = MEM_reallocN_id(cache->fiber_tangents, sizeof(float[3]) * totverts, "hair export tangents");
		cache->fiber_normals = MEM_reallocN_id(cache->fiber_normals, sizeof(float[3]) * totverts, "hair export normals");
		
		for (int i = 0; i < totcurves; ++i) {
			const HairFiberCurve *curve_orig = &hsys->curve_data.curves[i];
			const HairFiberVertex *verts_orig = &hsys->curve_data.verts[curve_orig->vertstart];
			const HairFiberCurve *curve = &cache->fiber_curves[i];
			HairFiberVertex *verts = &cache->fiber_verts[curve->vertstart];
			float (*tangents)[3] = &cache->fiber_tangents[curve->vertstart];
			float (*normals)[3] = &cache->fiber_normals[curve->vertstart];
			
			/* Root matrix for offsetting to the scalp surface and for initial normal direction */
			float rootpos[3];
			float rootmat[3][3];
			BKE_mesh_sample_eval(scalp, &curve->mesh_sample, rootpos, rootmat[2], rootmat[0]);
			cross_v3_v3v3(rootmat[1], rootmat[2], rootmat[0]);
			
			hair_curve_subdivide(curve_orig, verts_orig, subdiv, rootpos, verts);
			
			hair_curve_calc_vectors(verts, curve->numverts, rootmat, tangents, normals);
		}
	}

	if (hsys->pattern)
	{
		if (data & HAIR_EXPORT_FOLLICLE_BINDING)
		{
			cache->follicles = hsys->pattern->follicles;
			cache->totfibercurves = hsys->pattern->num_follicles;
		}

		if (data & HAIR_EXPORT_FIBER_VERTEX_COUNTS)
		{
			/* Calculate the length of each fiber from the weighted average of its parents */
			const int totcurves = cache->totcurves;
			const int totfibercurves = cache->totfibercurves;
			
			cache->fiber_numverts = MEM_reallocN_id(cache->fiber_numverts, sizeof(int) * totfibercurves, "fiber numverts");
			cache->totfiberverts = 0;
			
			const HairFollicle *follicle = hsys->pattern->follicles;
			for (int i = 0; i < totfibercurves; ++i, ++follicle) {
				float fiblen = 0.0f;
				
				for (int k = 0; k < 4; ++k) {
					const int si = follicle->parent_index[k];
					const float sw = follicle->parent_weight[k];
					if (si == HAIR_STRAND_INDEX_NONE || sw == 0.0f) {
						break;
					}
					BLI_assert(si < totcurves);
					
					fiblen += (float)cache->fiber_curves[si].numverts * sw;
				}
				
				/* Use rounded number of segments */
				const int numverts = (int)(fiblen + 0.5f);
				cache->fiber_numverts[i] = numverts;
				cache->totfiberverts += numverts;
			}
		}

		if (data & HAIR_EXPORT_FIBER_ROOT_POSITIONS)
		{
			const int totfibercurves = cache->totfibercurves;
			
			cache->fiber_root_position = MEM_reallocN_id(cache->fiber_root_position, sizeof(float[3]) * totfibercurves, "fiber root position");
			const HairFollicle *follicle = hsys->pattern->follicles;
			for (int i = 0; i < totfibercurves; ++i, ++follicle) {
				/* Cache fiber root position */
				float nor[3], tang[3];
				BKE_mesh_sample_eval(scalp, &follicle->mesh_sample, cache->fiber_root_position[i], nor, tang);
			}
		}
	}
	else
	{
		cache->follicles = NULL;
		cache->totfibercurves = 0;
		
		if (cache->fiber_numverts)
		{
			MEM_freeN(cache->fiber_numverts);
			cache->fiber_numverts = NULL;
		}
		if (cache->fiber_root_position)
		{
			MEM_freeN(cache->fiber_root_position);
			cache->fiber_root_position = NULL;
		}
	}
	
	return data;
}

/* Free the given export cache */

void BKE_hair_export_cache_free(HairExportCache *cache)
{
	if (cache->fiber_numverts)
	{
		MEM_freeN(cache->fiber_numverts);
	}
	if (cache->fiber_curves)
	{
		MEM_freeN(cache->fiber_curves);
	}
	if (cache->fiber_verts)
	{
		MEM_freeN(cache->fiber_verts);
	}
	if (cache->fiber_tangents)
	{
		MEM_freeN(cache->fiber_tangents);
	}
	if (cache->fiber_normals)
	{
		MEM_freeN(cache->fiber_normals);
	}
	if (cache->fiber_root_position)
	{
		MEM_freeN(cache->fiber_root_position);
	}
	MEM_freeN(cache);
}

/* Invalidate all data in a hair export cache */

void BKE_hair_export_cache_clear(HairExportCache *cache)
{
	/* Invalidate everything */
	BKE_hair_export_cache_invalidate(cache, HAIR_EXPORT_ALL);
}

/* Invalidate part of the data in a hair export cache.
 *
 * Note some parts may get invalidated automatically based on internal dependencies.
 */

void BKE_hair_export_cache_invalidate(HairExportCache *cache, int invalidate)
{
	/* Include dependencies */
	int data = hair_export_cache_get_dependencies(invalidate);

	if (data & HAIR_EXPORT_FIBER_CURVES)
	{
		if (cache->fiber_curves)
		{
			MEM_freeN(cache->fiber_curves);
			cache->fiber_curves = 0;
		}
	}
	if (data & HAIR_EXPORT_FIBER_VERTICES)
	{
		if (cache->fiber_verts)
		{
			MEM_freeN(cache->fiber_verts);
			cache->fiber_verts = NULL;
		}
		if (cache->fiber_tangents)
		{
			MEM_freeN(cache->fiber_tangents);
			cache->fiber_tangents = NULL;
		}
		if (cache->fiber_normals)
		{
			MEM_freeN(cache->fiber_normals);
			cache->fiber_tangents = NULL;
		}
	}
	if (data & HAIR_EXPORT_FOLLICLE_BINDING)
	{
		cache->follicles = NULL;
	}
	if (data & HAIR_EXPORT_FIBER_ROOT_POSITIONS)
	{
		if (cache->fiber_root_position)
		{
			MEM_freeN(cache->fiber_root_position);
			cache->fiber_root_position = NULL;
		}
	}
	if (data & HAIR_EXPORT_FIBER_VERTEX_COUNTS)
	{
		if (cache->fiber_numverts)
		{
			MEM_freeN(cache->fiber_numverts);
			cache->fiber_numverts = NULL;
		}
	}
}
