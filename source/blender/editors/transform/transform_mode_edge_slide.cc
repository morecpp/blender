/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "BLI_math_matrix.h"
#include "BLI_string.h"

#include "BKE_editmesh.hh"
#include "BKE_editmesh_bvh.h"
#include "BKE_unit.hh"

#include "GPU_immediate.h"
#include "GPU_matrix.h"

#include "ED_mesh.hh"
#include "ED_screen.hh"

#include "WM_api.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"

#include "BLT_translation.hh"

#include "transform.hh"
#include "transform_constraints.hh"
#include "transform_convert.hh"
#include "transform_mode.hh"
#include "transform_snap.hh"

/* -------------------------------------------------------------------- */
/** \name Transform (Edge Slide)
 * \{ */

struct EdgeSlideData {
  TransDataEdgeSlideVert *sv;
  int totsv;

  int mval_start[2], mval_end[2];
  int curr_sv_index;
};

struct EdgeSlideParams {
  float perc;

  /** when un-clamped - use this index: #TransDataEdgeSlideVert.dir_side */
  int curr_side_unclamp;

  bool use_even;
  bool flipped;
};

/**
 * Get the first valid TransDataContainer *.
 *
 * Note we cannot trust TRANS_DATA_CONTAINER_FIRST_OK because of multi-object that
 * may leave items with invalid custom data in the transform data container.
 */
static TransDataContainer *edge_slide_container_first_ok(TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    if (tc->custom.mode.data) {
      return tc;
    }
  }
  BLI_assert_msg(0, "Should never happen, at least one EdgeSlideData should be valid");
  return nullptr;
}

static EdgeSlideData *edgeSlideFirstGet(TransInfo *t)
{
  TransDataContainer *tc = edge_slide_container_first_ok(t);
  return static_cast<EdgeSlideData *>(tc->custom.mode.data);
}

static void calcEdgeSlideCustomPoints(TransInfo *t)
{
  EdgeSlideData *sld = edgeSlideFirstGet(t);

  setCustomPoints(t, &t->mouse, sld->mval_end, sld->mval_start);

  /* setCustomPoints isn't normally changing as the mouse moves,
   * in this case apply mouse input immediately so we don't refresh
   * with the value from the previous points */
  applyMouseInput(t, &t->mouse, t->mval, t->values);
}

/* Interpolates along a line made up of 2 segments (used for edge slide). */
static void interp_line_v3_v3v3v3(
    float p[3], const float v1[3], const float v2[3], const float v3[3], float t)
{
  float t_mid, t_delta;

  /* could be pre-calculated */
  t_mid = line_point_factor_v3(v2, v1, v3);

  t_delta = t - t_mid;
  if (t_delta < 0.0f) {
    if (UNLIKELY(fabsf(t_mid) < FLT_EPSILON)) {
      copy_v3_v3(p, v2);
    }
    else {
      interp_v3_v3v3(p, v1, v2, t / t_mid);
    }
  }
  else {
    t = t - t_mid;
    t_mid = 1.0f - t_mid;

    if (UNLIKELY(fabsf(t_mid) < FLT_EPSILON)) {
      copy_v3_v3(p, v3);
    }
    else {
      interp_v3_v3v3(p, v2, v3, t / t_mid);
    }
  }
}

static blender::float4x4 edge_slide_projmat_get(TransInfo *t, TransDataContainer *tc)
{
  RegionView3D *rv3d = nullptr;

  if (t->spacetype == SPACE_VIEW3D) {
    /* Background mode support. */
    rv3d = static_cast<RegionView3D *>(t->region ? t->region->regiondata : nullptr);
  }

  if (!rv3d) {
    /* Ok, let's try to survive this. */
    return blender::float4x4::identity();
  }
  return ED_view3d_ob_project_mat_get(rv3d, tc->obedit);
}

static void edge_slide_pair_project(TransDataEdgeSlideVert *sv,
                                    ARegion *region,
                                    const float projectMat[4][4],
                                    float r_sco_a[3],
                                    float r_sco_b[3])
{
  BMVert *v = sv->v;

  add_v3_v3v3(r_sco_b, v->co, sv->dir_side[1]);
  ED_view3d_project_float_v3_m4(region, r_sco_b, r_sco_b, projectMat);

  add_v3_v3v3(r_sco_a, v->co, sv->dir_side[0]);
  ED_view3d_project_float_v3_m4(region, r_sco_a, r_sco_a, projectMat);
}

static void edge_slide_data_init_mval(MouseInput *mi, EdgeSlideData *sld, float *mval_dir)
{
  /* Possible all of the edge loops are pointing directly at the view. */
  if (UNLIKELY(len_squared_v2(mval_dir) < 0.1f)) {
    mval_dir[0] = 0.0f;
    mval_dir[1] = 100.0f;
  }

  float mval_start[2], mval_end[2];

  /* Zero out Start. */
  zero_v2(mval_start);

  /* dir holds a vector along edge loop */
  copy_v2_v2(mval_end, mval_dir);
  mul_v2_fl(mval_end, 0.5f);

  sld->mval_start[0] = mi->imval[0] + mval_start[0];
  sld->mval_start[1] = mi->imval[1] + mval_start[1];

  sld->mval_end[0] = mi->imval[0] + mval_end[0];
  sld->mval_end[1] = mi->imval[1] + mval_end[1];
}

/**
 * Calculate screen-space `mval_start` / `mval_end`, optionally slide direction.
 */
static void calcEdgeSlide_mval_range(TransInfo *t,
                                     TransDataContainer *tc,
                                     EdgeSlideData *sld,
                                     const int loop_nr,
                                     const blender::float2 &mval,
                                     const bool use_occlude_geometry,
                                     const bool use_calc_direction)
{
  TransDataEdgeSlideVert *sv;
  BMEditMesh *em = BKE_editmesh_from_object(tc->obedit);
  ARegion *region = t->region;
  View3D *v3d = nullptr;
  BMBVHTree *bmbvh;

  /* only for use_calc_direction */
  float(*loop_dir)[3] = nullptr, *loop_maxdist = nullptr;

  float mval_dir[3], dist_best_sq;

  if (t->spacetype == SPACE_VIEW3D) {
    /* background mode support */
    v3d = static_cast<View3D *>(t->area ? t->area->spacedata.first : nullptr);
  }

  const blender::float4x4 projection = edge_slide_projmat_get(t, tc);

  if (use_occlude_geometry) {
    bmbvh = BKE_bmbvh_new_from_editmesh(em, BMBVH_RESPECT_HIDDEN, nullptr, false);
  }
  else {
    bmbvh = nullptr;
  }

  /* find mouse vectors, the global one, and one per loop in case we have
   * multiple loops selected, in case they are oriented different */
  zero_v3(mval_dir);
  dist_best_sq = -1.0f;

  if (use_calc_direction) {
    loop_dir = static_cast<float(*)[3]>(MEM_callocN(sizeof(float[3]) * loop_nr, "sv loop_dir"));
    loop_maxdist = static_cast<float *>(MEM_mallocN(sizeof(float) * loop_nr, "sv loop_maxdist"));
    copy_vn_fl(loop_maxdist, loop_nr, -1.0f);
  }

  sv = &sld->sv[0];
  for (int i = 0; i < sld->totsv; i++, sv++) {
    BMIter iter_other;
    BMEdge *e;
    BMVert *v = sv->v;

    /* Search cross edges for visible edge to the mouse cursor,
     * then use the shared vertex to calculate screen vector. */
    BM_ITER_ELEM (e, &iter_other, v, BM_EDGES_OF_VERT) {
      /* screen-space coords */
      float sco_a[3], sco_b[3];
      float dist_sq;
      int l_nr;

      if (BM_elem_flag_test(e, BM_ELEM_SELECT)) {
        continue;
      }

      /* This test is only relevant if object is not wire-drawn! See #32068. */
      bool is_visible = !use_occlude_geometry ||
                        BMBVH_EdgeVisible(bmbvh, e, t->depsgraph, region, v3d, tc->obedit);

      if (!is_visible && !use_calc_direction) {
        continue;
      }

      edge_slide_pair_project(sv, region, projection.ptr(), sco_a, sco_b);

      /* global direction */
      dist_sq = dist_squared_to_line_segment_v2(mval, sco_b, sco_a);
      if (is_visible) {
        if ((dist_best_sq == -1.0f) ||
            /* intentionally use 2d size on 3d vector */
            (dist_sq < dist_best_sq && (len_squared_v2v2(sco_b, sco_a) > 0.1f)))
        {
          dist_best_sq = dist_sq;
          sub_v3_v3v3(mval_dir, sco_b, sco_a);
        }
      }

      if (use_calc_direction) {
        /* per loop direction */
        l_nr = sv->loop_nr;
        if (loop_maxdist[l_nr] == -1.0f || dist_sq < loop_maxdist[l_nr]) {
          loop_maxdist[l_nr] = dist_sq;
          sub_v3_v3v3(loop_dir[l_nr], sco_b, sco_a);
        }
      }
    }
  }

  if (use_calc_direction) {
    int i;
    sv = &sld->sv[0];
    for (i = 0; i < sld->totsv; i++, sv++) {
      /* switch a/b if loop direction is different from global direction */
      int l_nr = sv->loop_nr;
      if (dot_v3v3(loop_dir[l_nr], mval_dir) < 0.0f) {
        swap_v3_v3(sv->dir_side[0], sv->dir_side[1]);
      }
    }

    MEM_freeN(loop_dir);
    MEM_freeN(loop_maxdist);
  }

  edge_slide_data_init_mval(&t->mouse, sld, mval_dir);

  if (bmbvh) {
    BKE_bmbvh_free(bmbvh);
  }
}

static void calcEdgeSlide_even(TransInfo *t,
                               TransDataContainer *tc,
                               EdgeSlideData *sld,
                               const blender::float2 &mval)
{
  TransDataEdgeSlideVert *sv = sld->sv;

  if (sld->totsv > 0) {
    ARegion *region = t->region;

    int i = 0;

    float dist_min_sq = FLT_MAX;

    const blender::float4x4 projection = edge_slide_projmat_get(t, tc);

    for (i = 0; i < sld->totsv; i++, sv++) {
      /* Set length */
      sv->edge_len = len_v3v3(sv->dir_side[0], sv->dir_side[1]);

      const blender::float2 v_proj = ED_view3d_project_float_v2_m4(region, sv->v->co, projection);
      const float dist_sq = len_squared_v2v2(mval, v_proj);
      if (dist_sq < dist_min_sq) {
        dist_min_sq = dist_sq;
        sld->curr_sv_index = i;
      }
    }
  }
  else {
    sld->curr_sv_index = 0;
  }
}

static EdgeSlideData *createEdgeSlideVerts(TransInfo *t,
                                           TransDataContainer *tc,
                                           const bool use_double_side)
{
  int loop_nr;
  EdgeSlideData *sld = static_cast<EdgeSlideData *>(MEM_callocN(sizeof(*sld), "sld"));
  sld->curr_sv_index = 0;

  sld->sv = transform_mesh_edge_slide_data_create(tc, use_double_side, &loop_nr, &sld->totsv);

  if (sld->sv == nullptr) {
    MEM_freeN(sld);
    return nullptr;
  }

  bool use_occlude_geometry = false;
  View3D *v3d = nullptr;
  RegionView3D *rv3d = nullptr;

  /* use for visibility checks */
  if (t->spacetype == SPACE_VIEW3D) {
    v3d = static_cast<View3D *>(t->area ? t->area->spacedata.first : nullptr);
    rv3d = static_cast<RegionView3D *>(t->region ? t->region->regiondata : nullptr);
    use_occlude_geometry = (v3d && TRANS_DATA_CONTAINER_FIRST_OK(t)->obedit->dt > OB_WIRE &&
                            !XRAY_ENABLED(v3d));
  }

  calcEdgeSlide_mval_range(t, tc, sld, loop_nr, t->mval, use_occlude_geometry, false);

  if (rv3d) {
    calcEdgeSlide_even(t, tc, sld, t->mval);
  }

  return sld;
}

static void freeEdgeSlideVerts(TransInfo * /*t*/,
                               TransDataContainer * /*tc*/,
                               TransCustomData *custom_data)
{
  EdgeSlideData *sld = static_cast<EdgeSlideData *>(custom_data->data);

  if (sld == nullptr) {
    return;
  }

  MEM_freeN(sld->sv);
  MEM_freeN(sld);

  custom_data->data = nullptr;
}

static eRedrawFlag handleEventEdgeSlide(TransInfo *t, const wmEvent *event)
{
  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);

  if (slp) {
    switch (event->type) {
      case EVT_EKEY:
        if (event->val == KM_PRESS) {
          slp->use_even = !slp->use_even;
          calcEdgeSlideCustomPoints(t);
          return TREDRAW_HARD;
        }
        break;
      case EVT_FKEY:
        if (event->val == KM_PRESS) {
          slp->flipped = !slp->flipped;
          calcEdgeSlideCustomPoints(t);
          return TREDRAW_HARD;
        }
        break;
      case EVT_CKEY:
        /* use like a modifier key */
        if (event->val == KM_PRESS) {
          t->flag ^= T_ALT_TRANSFORM;
          calcEdgeSlideCustomPoints(t);
          return TREDRAW_HARD;
        }
        break;
      case MOUSEMOVE:
        calcEdgeSlideCustomPoints(t);
        break;
      default:
        break;
    }
  }
  return TREDRAW_NOTHING;
}

static void drawEdgeSlide(TransInfo *t)
{
  EdgeSlideData *sld = edgeSlideFirstGet(t);
  if (sld == nullptr) {
    return;
  }

  const EdgeSlideParams *slp = static_cast<const EdgeSlideParams *>(t->custom.mode.data);
  const bool is_clamp = !(t->flag & T_ALT_TRANSFORM);

  const float line_size = UI_GetThemeValuef(TH_OUTLINE_WIDTH) + 0.5f;

  GPU_depth_test(GPU_DEPTH_NONE);

  GPU_blend(GPU_BLEND_ALPHA);

  GPU_matrix_push();
  GPU_matrix_mul(TRANS_DATA_CONTAINER_FIRST_OK(t)->obedit->object_to_world().ptr());

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  if (slp->use_even == true) {
    /* Even mode */
    float co_a[3], co_b[3], co_mark[3];
    TransDataEdgeSlideVert *curr_sv = &sld->sv[sld->curr_sv_index];
    const float fac = (slp->perc + 1.0f) / 2.0f;
    const float ctrl_size = UI_GetThemeValuef(TH_FACEDOT_SIZE) + 1.5f;
    const float guide_size = ctrl_size - 0.5f;
    const int alpha_shade = -30;

    add_v3_v3v3(co_a, curr_sv->v_co_orig, curr_sv->dir_side[0]);
    add_v3_v3v3(co_b, curr_sv->v_co_orig, curr_sv->dir_side[1]);

    GPU_line_width(line_size);
    immUniformThemeColorShadeAlpha(TH_EDGE_SELECT, 80, alpha_shade);
    immBeginAtMost(GPU_PRIM_LINES, 4);
    if (!is_zero_v3(curr_sv->dir_side[0])) {
      immVertex3fv(pos, co_a);
      immVertex3fv(pos, curr_sv->v_co_orig);
    }
    if (!is_zero_v3(curr_sv->dir_side[1])) {
      immVertex3fv(pos, co_b);
      immVertex3fv(pos, curr_sv->v_co_orig);
    }
    immEnd();

    {
      float *co_test = nullptr;
      if (slp->flipped) {
        if (!is_zero_v3(curr_sv->dir_side[1])) {
          co_test = co_b;
        }
      }
      else {
        if (!is_zero_v3(curr_sv->dir_side[0])) {
          co_test = co_a;
        }
      }

      if (co_test != nullptr) {
        immUniformThemeColorShadeAlpha(TH_SELECT, -30, alpha_shade);
        GPU_point_size(ctrl_size);
        immBegin(GPU_PRIM_POINTS, 1);
        immVertex3fv(pos, co_test);
        immEnd();
      }
    }

    immUniformThemeColorShadeAlpha(TH_SELECT, 255, alpha_shade);
    GPU_point_size(guide_size);
    immBegin(GPU_PRIM_POINTS, 1);
    interp_line_v3_v3v3v3(co_mark, co_b, curr_sv->v_co_orig, co_a, fac);
    immVertex3fv(pos, co_mark);
    immEnd();
  }
  else if (is_clamp == false) {
    const int side_index = slp->curr_side_unclamp;
    TransDataEdgeSlideVert *sv;
    int i;
    const int alpha_shade = -160;

    GPU_line_width(line_size);
    immUniformThemeColorShadeAlpha(TH_EDGE_SELECT, 80, alpha_shade);
    immBegin(GPU_PRIM_LINES, sld->totsv * 2);

    /* TODO(@ideasman42): Loop over all verts. */
    sv = sld->sv;
    for (i = 0; i < sld->totsv; i++, sv++) {
      float a[3], b[3];

      if (!is_zero_v3(sv->dir_side[side_index])) {
        copy_v3_v3(a, sv->dir_side[side_index]);
      }
      else {
        copy_v3_v3(a, sv->dir_side[!side_index]);
      }

      mul_v3_fl(a, 100.0f);
      negate_v3_v3(b, a);
      add_v3_v3(a, sv->v_co_orig);
      add_v3_v3(b, sv->v_co_orig);

      immVertex3fv(pos, a);
      immVertex3fv(pos, b);
    }
    immEnd();
  }
  else {
    /* Common case. */
    TransDataEdgeSlideVert *curr_sv = &sld->sv[sld->curr_sv_index];
    const int alpha_shade = -160;

    float co_dir[3];
    add_v3_v3v3(co_dir, curr_sv->v_co_orig, curr_sv->dir_side[slp->curr_side_unclamp]);

    GPU_line_width(line_size);
    immUniformThemeColorShadeAlpha(TH_EDGE_SELECT, 80, alpha_shade);
    immBeginAtMost(GPU_PRIM_LINES, 2);
    immVertex3fv(pos, curr_sv->v_co_orig);
    immVertex3fv(pos, co_dir);
    immEnd();
  }

  immUnbindProgram();

  GPU_matrix_pop();

  GPU_blend(GPU_BLEND_NONE);

  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
}

static void edge_slide_snap_apply(TransInfo *t, float *value)
{
  TransDataContainer *tc = edge_slide_container_first_ok(t);
  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);
  EdgeSlideData *sld_active = static_cast<EdgeSlideData *>(tc->custom.mode.data);
  TransDataEdgeSlideVert *sv = &sld_active->sv[sld_active->curr_sv_index];
  float snap_point[3], co_orig[3], co_dest[2][3], dvec[3];

  copy_v3_v3(co_orig, sv->v_co_orig);
  add_v3_v3v3(co_dest[0], co_orig, sv->dir_side[0]);
  add_v3_v3v3(co_dest[1], co_orig, sv->dir_side[1]);
  if (tc->use_local_mat) {
    mul_m4_v3(tc->mat, co_orig);
    mul_m4_v3(tc->mat, co_dest[0]);
    mul_m4_v3(tc->mat, co_dest[1]);
  }

  getSnapPoint(t, dvec);
  sub_v3_v3(dvec, t->tsnap.snap_source);
  add_v3_v3v3(snap_point, co_orig, dvec);

  float perc = *value;
  int side_index;
  float t_mid;
  if (slp->use_even == false) {
    const bool is_clamp = !(t->flag & T_ALT_TRANSFORM);
    if (is_clamp) {
      side_index = perc < 0.0f;
    }
    else {
      side_index = slp->curr_side_unclamp;
    }
  }
  else {
    /* Could be pre-calculated. */
    t_mid = line_point_factor_v3(
        blender::float3{0.0f, 0.0f, 0.0f}, sv->dir_side[0], sv->dir_side[1]);

    float t_snap = line_point_factor_v3(snap_point, co_dest[0], co_dest[1]);
    side_index = t_snap >= t_mid;
  }

  if (t->tsnap.target_type & (SCE_SNAP_TO_EDGE | SCE_SNAP_TO_FACE)) {
    float co_dir[3];
    sub_v3_v3v3(co_dir, co_dest[side_index], co_orig);
    normalize_v3(co_dir);
    if (t->tsnap.target_type & SCE_SNAP_TO_EDGE) {
      transform_constraint_snap_axis_to_edge(t, co_dir, dvec);
    }
    else {
      transform_constraint_snap_axis_to_face(t, co_dir, dvec);
    }
    add_v3_v3v3(snap_point, co_orig, dvec);
  }

  perc = line_point_factor_v3(snap_point, co_orig, co_dest[side_index]);
  if (slp->use_even == false) {
    if (side_index) {
      perc *= -1;
    }
  }
  else {
    if (!side_index) {
      perc = (1.0f - perc) * t_mid;
    }
    else {
      perc = perc * (1.0f - t_mid) + t_mid;
    }

    if (slp->flipped) {
      perc = 1.0f - perc;
    }

    perc = (2 * perc) - 1.0f;

    if (!slp->flipped) {
      perc *= -1;
    }
  }

  *value = perc;
}

static void edge_slide_apply_elem(const TransDataEdgeSlideVert *sv,
                                  const float fac,
                                  const float curr_length_fac,
                                  const int curr_side_unclamp,
                                  const bool use_clamp,
                                  const bool use_even,
                                  const bool use_flip,
                                  float r_co[3])
{
  copy_v3_v3(r_co, sv->v_co_orig);

  if (use_even == false) {
    if (use_clamp) {
      const int side_index = (fac < 0.0f);
      const float fac_final = fabsf(fac);
      madd_v3_v3fl(r_co, sv->dir_side[side_index], fac_final);
    }
    else {
      int side_index = curr_side_unclamp;
      if (is_zero_v3(sv->dir_side[side_index])) {
        side_index = !side_index;
      }
      const float fac_final = (side_index == (fac < 0.0f) ? fabsf(fac) : -fabsf(fac));
      madd_v3_v3fl(r_co, sv->dir_side[side_index], fac_final);
    }
  }
  else {
    /**
     * NOTE(@ideasman42): Implementation note, even mode ignores the starting positions and uses
     * only the a/b verts, this could be changed/improved so the distance is
     * still met but the verts are moved along their original path (which may not be straight),
     * however how it works now is OK and matches 2.4x.
     *
     * \note `len_v3v3(curr_sv->dir_side[0], curr_sv->dir_side[1])`
     * is the same as the distance between the original vert locations,
     * same goes for the lines below.
     */
    if (sv->edge_len > FLT_EPSILON) {
      float co_a[3], co_b[3];
      const float fac_final = min_ff(sv->edge_len, curr_length_fac) / sv->edge_len;

      add_v3_v3v3(co_a, sv->v_co_orig, sv->dir_side[0]);
      add_v3_v3v3(co_b, sv->v_co_orig, sv->dir_side[1]);

      if (use_flip) {
        interp_line_v3_v3v3v3(r_co, co_b, sv->v_co_orig, co_a, fac_final);
      }
      else {
        interp_line_v3_v3v3v3(r_co, co_a, sv->v_co_orig, co_b, fac_final);
      }
    }
  }
}

static void doEdgeSlide(TransInfo *t, float perc)
{
  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);
  EdgeSlideData *sld_active = edgeSlideFirstGet(t);

  slp->perc = perc;

  const bool use_clamp = !(t->flag & T_ALT_TRANSFORM);
  const bool use_even = slp->use_even;
  const bool use_flip = slp->flipped;

  const int curr_side_unclamp = slp->curr_side_unclamp;
  float curr_length_fac = 0.0f;
  if (use_even) {
    TransDataEdgeSlideVert *sv_active = &sld_active->sv[sld_active->curr_sv_index];
    curr_length_fac = sv_active->edge_len * (((use_flip ? perc : -perc) + 1.0f) / 2.0f);
  }
  else if (use_clamp) {
    slp->curr_side_unclamp = (perc < 0.0f);
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    EdgeSlideData *sld = static_cast<EdgeSlideData *>(tc->custom.mode.data);

    if (sld == nullptr) {
      continue;
    }

    TransDataEdgeSlideVert *sv = sld->sv;
    for (int i = 0; i < sld->totsv; i++, sv++) {
      edge_slide_apply_elem(
          sv, perc, curr_length_fac, curr_side_unclamp, use_clamp, use_even, use_flip, sv->v->co);
    }
  }
}

static void applyEdgeSlide(TransInfo *t)
{
  char str[UI_MAX_DRAW_STR];
  size_t ofs = 0;
  float final;
  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);
  bool flipped = slp->flipped;
  bool use_even = slp->use_even;
  const bool is_clamp = !(t->flag & T_ALT_TRANSFORM);
  const bool is_constrained = !(is_clamp == false || hasNumInput(&t->num));

  final = t->values[0] + t->values_modal_offset[0];

  transform_snap_mixed_apply(t, &final);
  if (!validSnap(t)) {
    transform_snap_increment(t, &final);
  }

  /* only do this so out of range values are not displayed */
  if (is_constrained) {
    CLAMP(final, -1.0f, 1.0f);
  }

  applyNumInput(&t->num, &final);

  t->values_final[0] = final;

  /* header string */
  ofs += BLI_strncpy_rlen(str + ofs, RPT_("Edge Slide: "), sizeof(str) - ofs);
  if (hasNumInput(&t->num)) {
    char c[NUM_STR_REP_LEN];
    outputNumInput(&(t->num), c, &t->scene->unit);
    ofs += BLI_strncpy_rlen(str + ofs, &c[0], sizeof(str) - ofs);
  }
  else {
    ofs += BLI_snprintf_rlen(str + ofs, sizeof(str) - ofs, "%.4f ", final);
  }
  ofs += BLI_snprintf_rlen(
      str + ofs, sizeof(str) - ofs, RPT_("(E)ven: %s, "), WM_bool_as_string(use_even));
  if (use_even) {
    ofs += BLI_snprintf_rlen(
        str + ofs, sizeof(str) - ofs, RPT_("(F)lipped: %s, "), WM_bool_as_string(flipped));
  }
  ofs += BLI_snprintf_rlen(
      str + ofs, sizeof(str) - ofs, RPT_("Alt or (C)lamp: %s"), WM_bool_as_string(is_clamp));
  /* done with header string */

  /* do stuff here */
  doEdgeSlide(t, final);

  recalc_data(t);

  ED_area_status_text(t->area, str);
}

static void edge_slide_transform_matrix_fn(TransInfo *t, float mat_xform[4][4])
{
  float delta[3], orig_co[3], final_co[3];

  EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(t->custom.mode.data);
  TransDataContainer *tc = edge_slide_container_first_ok(t);
  EdgeSlideData *sld_active = static_cast<EdgeSlideData *>(tc->custom.mode.data);
  TransDataEdgeSlideVert *sv_active = &sld_active->sv[sld_active->curr_sv_index];

  copy_v3_v3(orig_co, sv_active->v_co_orig);

  const float fac = t->values_final[0];
  float curr_length_fac = 0.0f;
  if (slp->use_even) {
    curr_length_fac = sv_active->edge_len * (((slp->flipped ? fac : -fac) + 1.0f) / 2.0f);
  }

  edge_slide_apply_elem(sv_active,
                        fac,
                        curr_length_fac,
                        slp->curr_side_unclamp,
                        !(t->flag & T_ALT_TRANSFORM),
                        slp->use_even,
                        slp->flipped,
                        final_co);

  if (tc->use_local_mat) {
    mul_m4_v3(tc->mat, orig_co);
    mul_m4_v3(tc->mat, final_co);
  }

  sub_v3_v3v3(delta, final_co, orig_co);
  add_v3_v3(mat_xform[3], delta);
}

static void initEdgeSlide_ex(
    TransInfo *t, bool use_double_side, bool use_even, bool flipped, bool use_clamp)
{
  EdgeSlideData *sld;
  bool ok = false;

  t->mode = TFM_EDGE_SLIDE;

  {
    EdgeSlideParams *slp = static_cast<EdgeSlideParams *>(MEM_callocN(sizeof(*slp), __func__));
    slp->use_even = use_even;
    slp->flipped = flipped;
    /* happens to be best for single-sided */
    if (use_double_side == false) {
      slp->flipped = !flipped;
    }
    slp->perc = 0.0f;

    if (!use_clamp) {
      t->flag |= T_ALT_TRANSFORM;
    }

    t->custom.mode.data = slp;
    t->custom.mode.use_free = true;
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    sld = createEdgeSlideVerts(t, tc, use_double_side);
    if (sld) {
      tc->custom.mode.data = sld;
      tc->custom.mode.free_cb = freeEdgeSlideVerts;
      ok = true;
    }
  }

  if (!ok) {
    t->state = TRANS_CANCEL;
    return;
  }

  /* set custom point first if you want value to be initialized by init */
  calcEdgeSlideCustomPoints(t);
  initMouseInputMode(t, &t->mouse, INPUT_CUSTOM_RATIO_FLIP);

  t->idx_max = 0;
  t->num.idx_max = 0;
  t->snap[0] = 0.1f;
  t->snap[1] = t->snap[0] * 0.1f;

  copy_v3_fl(t->num.val_inc, t->snap[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_type[0] = B_UNIT_NONE;
}

static void initEdgeSlide(TransInfo *t, wmOperator *op)
{
  bool use_double_side = true;
  bool use_even = false;
  bool flipped = false;
  bool use_clamp = true;
  if (op) {
    use_double_side = !RNA_boolean_get(op->ptr, "single_side");
    use_even = RNA_boolean_get(op->ptr, "use_even");
    flipped = RNA_boolean_get(op->ptr, "flipped");
    use_clamp = RNA_boolean_get(op->ptr, "use_clamp");
  }
  initEdgeSlide_ex(t, use_double_side, use_even, flipped, use_clamp);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mouse Input Utilities
 * \{ */

void transform_mode_edge_slide_reproject_input(TransInfo *t)
{
  ARegion *region = t->region;

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    EdgeSlideData *sld = static_cast<EdgeSlideData *>(tc->custom.mode.data);
    if (sld) {
      const blender::float4x4 projection = edge_slide_projmat_get(t, tc);

      TransDataEdgeSlideVert *curr_sv = &sld->sv[sld->curr_sv_index];

      float mval_dir[3], sco_a[3], sco_b[3];
      edge_slide_pair_project(curr_sv, region, projection.ptr(), sco_a, sco_b);
      sub_v3_v3v3(mval_dir, sco_b, sco_a);
      edge_slide_data_init_mval(&t->mouse, sld, mval_dir);
    }
  }

  EdgeSlideData *sld = edgeSlideFirstGet(t);
  setCustomPoints(t, &t->mouse, sld->mval_end, sld->mval_start);
}

/** \} */

TransModeInfo TransMode_edgeslide = {
    /*flags*/ T_NO_CONSTRAINT,
    /*init_fn*/ initEdgeSlide,
    /*transform_fn*/ applyEdgeSlide,
    /*transform_matrix_fn*/ edge_slide_transform_matrix_fn,
    /*handle_event_fn*/ handleEventEdgeSlide,
    /*snap_distance_fn*/ transform_snap_distance_len_squared_fn,
    /*snap_apply_fn*/ edge_slide_snap_apply,
    /*draw_fn*/ drawEdgeSlide,
};
