/* SPDX-FileCopyrightText: 2010 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Implements the PBVH node hiding operator.
 */

#include "MEM_guardedalloc.h"

#include "BLI_array_utils.hh"
#include "BLI_bit_span_ops.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_ccg.hh"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subsurf.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "bmesh.hh"

#include "paint_intern.hh"

/* For undo push. */
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::hide {

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

Span<int> node_visible_verts(const PBVHNode &node,
                             const Span<bool> hide_vert,
                             Vector<int> &indices)
{
  if (BKE_pbvh_node_fully_hidden_get(&node)) {
    return {};
  }
  const Span<int> verts = bke::pbvh::node_unique_verts(node);
  if (hide_vert.is_empty()) {
    return verts;
  }
  indices.resize(verts.size());
  const int *end = std::copy_if(verts.begin(), verts.end(), indices.begin(), [&](const int vert) {
    return !hide_vert[vert];
  });
  indices.resize(end - indices.begin());
  return indices;
}

void sync_all_from_faces(Object &object)
{
  SculptSession &ss = *object.sculpt;
  Mesh &mesh = *static_cast<Mesh *>(object.data);

  SCULPT_topology_islands_invalidate(ss);

  switch (BKE_pbvh_type(*ss.pbvh)) {
    case PBVH_FACES: {
      /* We may have adjusted the ".hide_poly" attribute, now make the hide status attributes for
       * vertices and edges consistent. */
      bke::mesh_hide_face_flush(mesh);
      break;
    }
    case PBVH_GRIDS: {
      /* In addition to making the hide status of the base mesh consistent, we also have to
       * propagate the status to the Multires grids. */
      bke::mesh_hide_face_flush(mesh);
      BKE_sculpt_sync_face_visibility_to_grids(&mesh, ss.subdiv_ccg);
      break;
    }
    case PBVH_BMESH: {
      BMesh &bm = *ss.bm;
      BMIter iter;
      BMFace *f;

      /* Hide all verts and edges attached to faces. */
      BM_ITER_MESH (f, &iter, &bm, BM_FACES_OF_MESH) {
        BMLoop *l = f->l_first;
        do {
          BM_elem_flag_enable(l->v, BM_ELEM_HIDDEN);
          BM_elem_flag_enable(l->e, BM_ELEM_HIDDEN);
        } while ((l = l->next) != f->l_first);
      }

      /* Unhide verts and edges attached to visible faces. */
      BM_ITER_MESH (f, &iter, &bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
          continue;
        }

        BMLoop *l = f->l_first;
        do {
          BM_elem_flag_disable(l->v, BM_ELEM_HIDDEN);
          BM_elem_flag_disable(l->e, BM_ELEM_HIDDEN);
        } while ((l = l->next) != f->l_first);
      }
      break;
    }
  }
}

void tag_update_visibility(const bContext &C)
{
  ARegion *region = CTX_wm_region(&C);
  ED_region_tag_redraw(region);

  Object *ob = CTX_data_active_object(&C);
  WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, ob);

  DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
  const RegionView3D *rv3d = CTX_wm_region_view3d(&C);
  if (!BKE_sculptsession_use_pbvh_draw(ob, rv3d)) {
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }
}

void mesh_show_all(Object &object, const Span<PBVHNode *> nodes)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (const VArray<bool> attribute = *attributes.lookup<bool>(".hide_vert",
                                                              bke::AttrDomain::Point))
  {
    const VArraySpan hide_vert(attribute);
    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (PBVHNode *node : nodes.slice(range)) {
        const Span<int> verts = bke::pbvh::node_verts(*node);
        if (std::any_of(verts.begin(), verts.end(), [&](const int i) { return hide_vert[i]; })) {
          undo::push_node(object, node, undo::Type::HideVert);
          BKE_pbvh_node_mark_rebuild_draw(node);
        }
      }
    });
  }
  for (PBVHNode *node : nodes) {
    BKE_pbvh_node_fully_hidden_set(node, false);
  }
  attributes.remove(".hide_vert");
  bke::mesh_hide_vert_flush(mesh);
}

void grids_show_all(Depsgraph &depsgraph, Object &object, const Span<PBVHNode *> nodes)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  PBVH &pbvh = *object.sculpt->pbvh;
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  bool any_changed = false;
  if (!grid_hidden.is_empty()) {
    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (PBVHNode *node : nodes.slice(range)) {
        const Span<int> grids = bke::pbvh::node_grid_indices(*node);
        if (std::any_of(grids.begin(), grids.end(), [&](const int i) {
              return bits::any_bit_set(grid_hidden[i]);
            }))
        {
          any_changed = true;
          undo::push_node(object, node, undo::Type::HideVert);
          BKE_pbvh_node_mark_rebuild_draw(node);
        }
      }
    });
  }
  if (!any_changed) {
    return;
  }
  for (PBVHNode *node : nodes) {
    BKE_pbvh_node_fully_hidden_set(node, false);
  }
  BKE_subdiv_ccg_grid_hidden_free(subdiv_ccg);
  BKE_pbvh_sync_visibility_from_verts(pbvh, &mesh);
  multires_mark_as_modified(&depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Visibility Utilities
 * Functions that assist with applying changes to the different PBVH types.
 * \{ */

enum class VisAction {
  Hide = 0,
  Show = 1,
};

static bool action_to_hide(const VisAction action)
{
  return action == VisAction::Hide;
}

/* Calculates whether a face should be hidden based on all of its corner vertices.*/
static void calc_face_hide(const Span<int> node_faces,
                           const OffsetIndices<int> faces,
                           const Span<int> corner_verts,
                           const Span<bool> hide_vert,
                           MutableSpan<bool> hide_face)
{
  for (const int i : node_faces.index_range()) {
    Span<int> face_verts = corner_verts.slice(faces[node_faces[i]]);
    hide_face[i] = std::any_of(
        face_verts.begin(), face_verts.end(), [&](const int v) { return hide_vert[v]; });
  }
}

/* Updates a node's face's visibility based on the updated vertex visibility. */
static void flush_face_changes_node(Mesh &mesh,
                                    const Span<PBVHNode *> nodes,
                                    const Span<bool> hide_vert)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  const Span<int> tri_faces = mesh.corner_tri_faces();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  bke::SpanAttributeWriter<bool> hide_poly = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_poly", bke::AttrDomain::Face);

  struct TLS {
    Vector<int> face_indices;
    Vector<bool> new_hide;
  };
  threading::EnumerableThreadSpecific<TLS> all_tls;
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    TLS &tls = all_tls.local();
    for (PBVHNode *node : nodes.slice(range)) {
      const Span<int> node_faces = bke::pbvh::node_face_indices_calc_mesh(
          tri_faces, *node, tls.face_indices);

      tls.new_hide.reinitialize(node_faces.size());
      array_utils::gather(hide_poly.span.as_span(), node_faces, tls.new_hide.as_mutable_span());

      calc_face_hide(node_faces, faces, corner_verts, hide_vert, tls.new_hide.as_mutable_span());

      if (array_utils::indexed_data_equal<bool>(hide_poly.span, node_faces, tls.new_hide)) {
        continue;
      }

      array_utils::scatter(tls.new_hide.as_span(), node_faces, hide_poly.span);
      BKE_pbvh_node_mark_update_visibility(node);
      bke::pbvh::node_update_visibility_mesh(hide_vert, *node);
    }
  });
  hide_poly.finish();
}

/* Updates a node's face's visibility based on the updated vertex visibility. */
static void flush_face_changes(Mesh &mesh, const Span<bool> hide_vert)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  bke::SpanAttributeWriter<bool> hide_poly = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_poly", bke::AttrDomain::Face);

  bke::mesh_face_hide_from_vert(mesh.faces(), mesh.corner_verts(), hide_vert, hide_poly.span);
  hide_poly.finish();
}

/* Updates all of a mesh's edge visibility based on vertex visibility. */
static void flush_edge_changes(Mesh &mesh, const Span<bool> hide_vert)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  bke::SpanAttributeWriter<bool> hide_edge = attributes.lookup_or_add_for_write_only_span<bool>(
      ".hide_edge", bke::AttrDomain::Edge);
  bke::mesh_edge_hide_from_vert(mesh.edges(), hide_vert, hide_edge.span);
  hide_edge.finish();
}

static void vert_hide_update(Object &object,
                             const Span<PBVHNode *> nodes,
                             const FunctionRef<void(Span<int>, MutableSpan<bool>)> calc_hide)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<bool> hide_vert = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_vert", bke::AttrDomain::Point);

  bool any_changed = false;
  threading::EnumerableThreadSpecific<Vector<bool>> all_new_hide;
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    Vector<bool> &new_hide = all_new_hide.local();
    for (PBVHNode *node : nodes.slice(range)) {
      const Span<int> verts = bke::pbvh::node_unique_verts(*node);

      new_hide.reinitialize(verts.size());
      array_utils::gather(hide_vert.span.as_span(), verts, new_hide.as_mutable_span());
      calc_hide(verts, new_hide);
      if (array_utils::indexed_data_equal<bool>(hide_vert.span, verts, new_hide)) {
        continue;
      }

      any_changed = true;
      undo::push_node(object, node, undo::Type::HideVert);
      array_utils::scatter(new_hide.as_span(), verts, hide_vert.span);
    }
  });

  hide_vert.finish();
  if (any_changed) {
    /* We handle flushing ourselves at the node level instead of delegating to
     * bke::mesh_hide_vert_flush because we need to tag node visibility changes as well in cases
     * where the vertices hidden are on a node boundary.*/
    flush_face_changes_node(mesh, nodes, hide_vert.span);
    flush_edge_changes(mesh, hide_vert.span);
  }
}

static void grid_hide_update(Depsgraph &depsgraph,
                             Object &object,
                             const Span<PBVHNode *> nodes,
                             const FunctionRef<void(const int, MutableBoundedBitSpan)> calc_hide)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  PBVH &pbvh = *object.sculpt->pbvh;
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;
  BitGroupVector<> &grid_hidden = BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg);

  bool any_changed = false;
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      const Span<int> grids = bke::pbvh::node_grid_indices(*node);
      BitGroupVector<> new_hide(grids.size(), grid_hidden.group_size());
      for (const int i : grids.index_range()) {
        new_hide[i].copy_from(grid_hidden[grids[i]].as_span());
      }

      for (const int i : grids.index_range()) {
        calc_hide(grids[i], new_hide[i]);
      }

      if (std::all_of(grids.index_range().begin(), grids.index_range().end(), [&](const int i) {
            return bits::spans_equal(grid_hidden[grids[i]], new_hide[i]);
          }))
      {
        continue;
      }

      any_changed = true;
      undo::push_node(object, node, undo::Type::HideVert);

      for (const int i : grids.index_range()) {
        grid_hidden[grids[i]].copy_from(new_hide[i].as_span());
      }

      BKE_pbvh_node_mark_update_visibility(node);
      bke::pbvh::node_update_visibility_grids(grid_hidden, *node);
    }
  });

  if (any_changed) {
    multires_mark_as_modified(&depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
    BKE_pbvh_sync_visibility_from_verts(pbvh, &mesh);
  }
}

static void partialvis_update_bmesh_verts(const Set<BMVert *, 0> &verts,
                                          const VisAction action,
                                          const FunctionRef<bool(BMVert *v)> should_update,
                                          bool *any_changed,
                                          bool *any_visible)
{
  for (BMVert *v : verts) {
    if (should_update(v)) {
      if (action == VisAction::Hide) {
        BM_elem_flag_enable(v, BM_ELEM_HIDDEN);
      }
      else {
        BM_elem_flag_disable(v, BM_ELEM_HIDDEN);
      }
      (*any_changed) = true;
    }

    if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
      (*any_visible) = true;
    }
  }
}

static void partialvis_update_bmesh_faces(const Set<BMFace *, 0> &faces)
{
  for (BMFace *f : faces) {
    if (paint_is_bmesh_face_hidden(f)) {
      BM_elem_flag_enable(f, BM_ELEM_HIDDEN);
    }
    else {
      BM_elem_flag_disable(f, BM_ELEM_HIDDEN);
    }
  }
}

static void partialvis_update_bmesh_nodes(Object &ob,
                                          const Span<PBVHNode *> nodes,
                                          const VisAction action,
                                          const FunctionRef<bool(BMVert *v)> vert_test_fn)
{
  for (PBVHNode *node : nodes) {
    bool any_changed = false;
    bool any_visible = false;

    undo::push_node(ob, node, undo::Type::HideVert);

    partialvis_update_bmesh_verts(
        BKE_pbvh_bmesh_node_unique_verts(node), action, vert_test_fn, &any_changed, &any_visible);

    partialvis_update_bmesh_verts(
        BKE_pbvh_bmesh_node_other_verts(node), action, vert_test_fn, &any_changed, &any_visible);

    /* Finally loop over node faces and tag the ones that are fully hidden. */
    partialvis_update_bmesh_faces(BKE_pbvh_bmesh_node_faces(node));

    if (any_changed) {
      BKE_pbvh_node_mark_rebuild_draw(node);
      BKE_pbvh_node_fully_hidden_set(node, !any_visible);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Global Visibility Operators
 * Operators that act upon the entirety of a given object's mesh.
 * \{ */

static void partialvis_all_update_mesh(Object &object,
                                       const VisAction action,
                                       const Span<PBVHNode *> nodes)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (action == VisAction::Show && !attributes.contains(".hide_vert")) {
    /* If everything is already visible, don't do anything. */
    return;
  }

  switch (action) {
    case VisAction::Hide:
      vert_hide_update(object, nodes, [&](const Span<int> /*verts*/, MutableSpan<bool> hide) {
        hide.fill(true);
      });
      break;
    case VisAction::Show:
      mesh_show_all(object, nodes);
      break;
  }
}

static void partialvis_all_update_grids(Depsgraph &depsgraph,
                                        Object &object,
                                        const VisAction action,
                                        const Span<PBVHNode *> nodes)
{
  switch (action) {
    case VisAction::Hide:
      grid_hide_update(depsgraph,
                       object,
                       nodes,
                       [&](const int /*verts*/, MutableBoundedBitSpan hide) { hide.fill(true); });
      break;
    case VisAction::Show:
      grids_show_all(depsgraph, object, nodes);
      break;
  }
}

static void partialvis_all_update_bmesh(Object &ob,
                                        const VisAction action,
                                        const Span<PBVHNode *> nodes)
{
  partialvis_update_bmesh_nodes(ob, nodes, action, [](const BMVert * /*vert*/) { return true; });
}

static int hide_show_all_exec(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  const VisAction action = VisAction(RNA_enum_get(op->ptr, "action"));

  PBVH *pbvh = BKE_sculpt_object_pbvh_ensure(depsgraph, &ob);
  BLI_assert(BKE_object_sculpt_pbvh_get(&ob) == pbvh);

  /* Start undo. */
  switch (action) {
    case VisAction::Hide:
      undo::push_begin_ex(ob, "Hide area");
      break;
    case VisAction::Show:
      undo::push_begin_ex(ob, "Show area");
      break;
  }

  Vector<PBVHNode *> nodes = bke::pbvh::search_gather(*pbvh, {});

  switch (BKE_pbvh_type(*pbvh)) {
    case PBVH_FACES:
      partialvis_all_update_mesh(ob, action, nodes);
      break;
    case PBVH_GRIDS:
      partialvis_all_update_grids(*depsgraph, ob, action, nodes);
      break;
    case PBVH_BMESH:
      partialvis_all_update_bmesh(ob, action, nodes);
      break;
  }

  /* End undo. */
  undo::push_end(ob);

  SCULPT_topology_islands_invalidate(*ob.sculpt);
  tag_update_visibility(*C);

  return OPERATOR_FINISHED;
}

static void partialvis_masked_update_mesh(Object &object,
                                          const VisAction action,
                                          const Span<PBVHNode *> nodes)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (action == VisAction::Show && !attributes.contains(".hide_vert")) {
    /* If everything is already visible, don't do anything. */
    return;
  }

  const bool value = action_to_hide(action);
  const VArraySpan<float> mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
  if (action == VisAction::Show && mask.is_empty()) {
    mesh_show_all(object, nodes);
  }
  else if (!mask.is_empty()) {
    vert_hide_update(object, nodes, [&](const Span<int> verts, MutableSpan<bool> hide) {
      for (const int i : verts.index_range()) {
        if (mask[verts[i]] > 0.5f) {
          hide[i] = value;
        }
      }
    });
  }
}

static void partialvis_masked_update_grids(Depsgraph &depsgraph,
                                           Object &object,
                                           const VisAction action,
                                           const Span<PBVHNode *> nodes)
{
  PBVH &pbvh = *object.sculpt->pbvh;
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;

  const bool value = action_to_hide(action);
  const CCGKey key = *BKE_pbvh_get_grid_key(pbvh);
  const Span<CCGElem *> grids = subdiv_ccg.grids;
  if (!key.has_mask) {
    grid_hide_update(depsgraph,
                     object,
                     nodes,
                     [&](const int /*verts*/, MutableBoundedBitSpan hide) { hide.fill(value); });
  }
  else {
    grid_hide_update(
        depsgraph, object, nodes, [&](const int grid_index, MutableBoundedBitSpan hide) {
          CCGElem *grid = grids[grid_index];
          for (const int y : IndexRange(key.grid_size)) {
            for (const int x : IndexRange(key.grid_size)) {
              CCGElem *elem = CCG_grid_elem(key, grid, x, y);
              if (CCG_elem_mask(key, elem) > 0.5f) {
                hide[y * key.grid_size + x].set(value);
              }
            }
          }
        });
  }
}

static void partialvis_masked_update_bmesh(Object &ob,
                                           PBVH *pbvh,
                                           const VisAction action,
                                           const Span<PBVHNode *> nodes)
{
  BMesh *bm = BKE_pbvh_get_bmesh(*pbvh);
  const int mask_offset = CustomData_get_offset_named(&bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
  const auto mask_test_fn = [&](const BMVert *v) {
    const float vmask = BM_ELEM_CD_GET_FLOAT(v, mask_offset);
    return vmask > 0.5f;
  };

  partialvis_update_bmesh_nodes(ob, nodes, action, mask_test_fn);
}

static int hide_show_masked_exec(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  const VisAction action = VisAction(RNA_enum_get(op->ptr, "action"));

  PBVH *pbvh = BKE_sculpt_object_pbvh_ensure(depsgraph, &ob);
  BLI_assert(BKE_object_sculpt_pbvh_get(&ob) == pbvh);

  /* Start undo. */
  switch (action) {
    case VisAction::Hide:
      undo::push_begin_ex(ob, "Hide area");
      break;
    case VisAction::Show:
      undo::push_begin_ex(ob, "Show area");
      break;
  }

  Vector<PBVHNode *> nodes = bke::pbvh::search_gather(*pbvh, {});

  switch (BKE_pbvh_type(*pbvh)) {
    case PBVH_FACES:
      partialvis_masked_update_mesh(ob, action, nodes);
      break;
    case PBVH_GRIDS:
      partialvis_masked_update_grids(*depsgraph, ob, action, nodes);
      break;
    case PBVH_BMESH:
      partialvis_masked_update_bmesh(ob, pbvh, action, nodes);
      break;
  }

  /* End undo. */
  undo::push_end(ob);

  SCULPT_topology_islands_invalidate(*ob.sculpt);
  tag_update_visibility(*C);

  return OPERATOR_FINISHED;
}

static void hide_show_operator_properties(wmOperatorType *ot)
{
  static const EnumPropertyItem action_items[] = {
      {int(VisAction::Hide), "HIDE", 0, "Hide", "Hide vertices"},
      {int(VisAction::Show), "SHOW", 0, "Show", "Show vertices"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_enum(ot->srna,
               "action",
               action_items,
               int(VisAction::Hide),
               "Visibility Action",
               "Whether to hide or show vertices");
}

void PAINT_OT_hide_show_masked(wmOperatorType *ot)
{
  ot->name = "Hide/Show Masked";
  ot->idname = "PAINT_OT_hide_show_masked";
  ot->description = "Hide/show all masked vertices above a threshold";

  ot->exec = hide_show_masked_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  hide_show_operator_properties(ot);
}

void PAINT_OT_hide_show_all(wmOperatorType *ot)
{
  ot->name = "Hide/Show All";
  ot->idname = "PAINT_OT_hide_show_all";
  ot->description = "Hide/show all vertices";

  ot->exec = hide_show_all_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  hide_show_operator_properties(ot);
}

static void invert_visibility_mesh(Object &object, const Span<PBVHNode *> nodes)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  const Span<int> tri_faces = mesh.corner_tri_faces();
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<bool> hide_poly = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_poly", bke::AttrDomain::Face);

  threading::EnumerableThreadSpecific<Vector<int>> all_index_data;
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    Vector<int> &faces = all_index_data.local();
    for (PBVHNode *node : nodes.slice(range)) {
      undo::push_node(object, node, undo::Type::HideFace);
      bke::pbvh::node_face_indices_calc_mesh(tri_faces, *node, faces);
      for (const int face : faces) {
        hide_poly.span[face] = !hide_poly.span[face];
      }
      BKE_pbvh_node_mark_update_visibility(node);
    }
  });

  hide_poly.finish();
  bke::mesh_hide_face_flush(mesh);
}

static void invert_visibility_grids(Depsgraph &depsgraph,
                                    Object &object,
                                    const Span<PBVHNode *> nodes)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  PBVH &pbvh = *object.sculpt->pbvh;
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;
  BitGroupVector<> &grid_hidden = BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg);

  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      undo::push_node(object, node, undo::Type::HideVert);
      for (const int i : bke::pbvh::node_grid_indices(*node)) {
        bits::invert(grid_hidden[i]);
      }
      BKE_pbvh_node_mark_update_visibility(node);
      bke::pbvh::node_update_visibility_grids(grid_hidden, *node);
    }
  });

  multires_mark_as_modified(&depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
  BKE_pbvh_sync_visibility_from_verts(pbvh, &mesh);
}

static void invert_visibility_bmesh(Object &object, const Span<PBVHNode *> nodes)
{
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      undo::push_node(object, node, undo::Type::HideVert);
      bool fully_hidden = true;
      for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node)) {
        BM_elem_flag_toggle(vert, BM_ELEM_HIDDEN);
        fully_hidden &= BM_elem_flag_test_bool(vert, BM_ELEM_HIDDEN);
      }
      BKE_pbvh_node_fully_hidden_set(node, fully_hidden);
      BKE_pbvh_node_mark_rebuild_draw(node);
    }
  });
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      partialvis_update_bmesh_faces(BKE_pbvh_bmesh_node_faces(node));
    }
  });
}

static int visibility_invert_exec(bContext *C, wmOperator *op)
{
  Object &object = *CTX_data_active_object(C);
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);

  PBVH *pbvh = BKE_sculpt_object_pbvh_ensure(&depsgraph, &object);
  BLI_assert(BKE_object_sculpt_pbvh_get(&object) == pbvh);

  Vector<PBVHNode *> nodes = bke::pbvh::search_gather(*pbvh, {});
  undo::push_begin(object, op);
  switch (BKE_pbvh_type(*pbvh)) {
    case PBVH_FACES:
      invert_visibility_mesh(object, nodes);
      break;
    case PBVH_GRIDS:
      invert_visibility_grids(depsgraph, object, nodes);
      break;
    case PBVH_BMESH:
      invert_visibility_bmesh(object, nodes);
      break;
  }

  undo::push_end(object);

  SCULPT_topology_islands_invalidate(*object.sculpt);
  tag_update_visibility(*C);

  return OPERATOR_FINISHED;
}

void PAINT_OT_visibility_invert(wmOperatorType *ot)
{
  ot->name = "Invert Visibility";
  ot->idname = "PAINT_OT_visibility_invert";
  ot->description = "Invert the visibility of all vertices";

  ot->exec = visibility_invert_exec;
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;
}

/* Number of vertices per iteration step size when growing or shrinking visibility. */
static constexpr float VERTEX_ITERATION_THRESHOLD = 50000.0f;

/* Extracting the loop and comparing against / writing with a constant `false` or `true` instead of
 * using #action_to_hide results in a nearly 600ms speedup on a mesh with 1.5m verts. */
template<bool value>
static void affect_visibility_mesh(const IndexRange face,
                                   const Span<int> corner_verts,
                                   const Span<bool> read_buffer,
                                   MutableSpan<bool> write_buffer)
{
  for (const int corner : face) {
    int vert = corner_verts[corner];
    if (read_buffer[vert] != value) {
      continue;
    }

    const int prev = bke::mesh::face_corner_prev(face, corner);
    const int prev_vert = corner_verts[prev];
    write_buffer[prev_vert] = value;

    const int next = bke::mesh::face_corner_next(face, corner);
    const int next_vert = corner_verts[next];
    write_buffer[next_vert] = value;
  }
}

struct DualBuffer {
  Array<bool> front;
  Array<bool> back;

  MutableSpan<bool> write_buffer(int count)
  {
    return count % 2 == 0 ? back.as_mutable_span() : front.as_mutable_span();
  }

  Span<bool> read_buffer(int count)
  {
    return count % 2 == 0 ? front.as_span() : back.as_span();
  }
};

static void propagate_vertex_visibility(Mesh &mesh,
                                        DualBuffer &buffers,
                                        const VArraySpan<bool> &hide_poly,
                                        const VisAction action,
                                        const int iterations)
{
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  for (const int i : IndexRange(iterations)) {
    Span<bool> read_buffer = buffers.read_buffer(i);
    MutableSpan<bool> write_buffer = buffers.write_buffer(i);
    threading::parallel_for(faces.index_range(), 1024, [&](const IndexRange range) {
      for (const int face_index : range) {
        if (!hide_poly[face_index]) {
          continue;
        }
        const IndexRange face = faces[face_index];
        if (action == VisAction::Hide) {
          affect_visibility_mesh<true>(face, corner_verts, read_buffer, write_buffer);
        }
        else {
          affect_visibility_mesh<false>(face, corner_verts, read_buffer, write_buffer);
        }
      }
    });

    flush_face_changes(mesh, write_buffer);
  }
}

static void update_undo_state(Object &object,
                              const Span<PBVHNode *> nodes,
                              const Span<bool> old_hide_vert,
                              const Span<bool> new_hide_vert)
{
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      for (const int vert : bke::pbvh::node_unique_verts(*node)) {
        if (old_hide_vert[vert] != new_hide_vert[vert]) {
          undo::push_node(object, node, undo::Type::HideVert);
          break;
        }
      }
    }
  });
}

static void update_node_visibility_from_face_changes(const Span<PBVHNode *> nodes,
                                                     const Span<int> tri_faces,
                                                     const Span<bool> orig_hide_poly,
                                                     const Span<bool> new_hide_poly,
                                                     const Span<bool> hide_vert)
{
  threading::EnumerableThreadSpecific<Vector<int>> all_face_indices;
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    Vector<int> &face_indices = all_face_indices.local();
    for (PBVHNode *node : nodes.slice(range)) {
      bool any_changed = false;
      const Span<int> indices = bke::pbvh::node_face_indices_calc_mesh(
          tri_faces, *node, face_indices);
      for (const int face_index : indices) {
        if (orig_hide_poly[face_index] != new_hide_poly[face_index]) {
          any_changed = true;
          break;
        }
      }

      if (any_changed) {
        BKE_pbvh_node_mark_update_visibility(node);
        bke::pbvh::node_update_visibility_mesh(hide_vert, *node);
      }
    }
  });
}

static void grow_shrink_visibility_mesh(Object &object,
                                        const Span<PBVHNode *> nodes,
                                        const VisAction action,
                                        const int iterations)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (!attributes.contains(".hide_vert")) {
    /* If the entire mesh is visible, we can neither grow nor shrink the boundary. */
    return;
  }

  bke::SpanAttributeWriter<bool> hide_vert = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_vert", bke::AttrDomain::Point);
  const VArraySpan hide_poly = *attributes.lookup_or_default<bool>(
      ".hide_poly", bke::AttrDomain::Face, false);

  DualBuffer buffers;
  buffers.back.reinitialize(hide_vert.span.size());
  buffers.front.reinitialize(hide_vert.span.size());
  array_utils::copy(hide_vert.span.as_span(), buffers.back.as_mutable_span());
  array_utils::copy(hide_vert.span.as_span(), buffers.front.as_mutable_span());

  Array<bool> orig_hide_poly(hide_poly);
  propagate_vertex_visibility(mesh, buffers, hide_poly, action, iterations);

  const Span<bool> last_buffer = buffers.write_buffer(iterations - 1);

  update_undo_state(object, nodes, hide_vert.span, last_buffer);

  /* We can wait until after all iterations are done to flush edge changes as they are
   * not used for coarse filtering while iterating.*/
  flush_edge_changes(mesh, last_buffer);

  update_node_visibility_from_face_changes(
      nodes, mesh.corner_tri_faces(), orig_hide_poly, hide_poly, last_buffer);
  array_utils::copy(last_buffer, hide_vert.span);
  hide_vert.finish();
}

struct DualBitBuffer {
  BitGroupVector<> front;
  BitGroupVector<> back;

  BitGroupVector<> &write_buffer(int count)
  {
    return count % 2 == 0 ? back : front;
  }

  BitGroupVector<> &read_buffer(int count)
  {
    return count % 2 == 0 ? front : back;
  }
};

static void grow_shrink_visibility_grid(Depsgraph &depsgraph,
                                        Object &object,
                                        PBVH &pbvh,
                                        const Span<PBVHNode *> nodes,
                                        const VisAction action,
                                        const int iterations)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;

  BitGroupVector<> &grid_hidden = BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg);

  const bool desired_state = action_to_hide(action);
  const CCGKey key = *BKE_pbvh_get_grid_key(pbvh);

  DualBitBuffer buffers;
  buffers.front = grid_hidden;
  buffers.back = grid_hidden;

  Array<bool> node_changed(nodes.size(), false);

  for (const int i : IndexRange(iterations)) {
    BitGroupVector<> &read_buffer = buffers.read_buffer(i);
    BitGroupVector<> &write_buffer = buffers.write_buffer(i);

    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (const int node_index : range) {
        PBVHNode *node = nodes[node_index];
        const Span<int> grids = bke::pbvh::node_grid_indices(*node);

        for (const int grid_index : grids) {
          for (const int y : IndexRange(key.grid_size)) {
            for (const int x : IndexRange(key.grid_size)) {
              const int grid_elem_idx = CCG_grid_xy_to_index(key.grid_size, x, y);
              if (read_buffer[grid_index][grid_elem_idx] != desired_state) {
                continue;
              }

              SubdivCCGCoord coord{};
              coord.grid_index = grid_index;
              coord.x = x;
              coord.y = y;

              SubdivCCGNeighbors neighbors;
              BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, true, neighbors);

              for (const SubdivCCGCoord neighbor : neighbors.coords) {
                const int neighbor_grid_elem_idx = CCG_grid_xy_to_index(
                    key.grid_size, neighbor.x, neighbor.y);

                write_buffer[neighbor.grid_index][neighbor_grid_elem_idx].set(desired_state);
              }
            }
          }
        }

        node_changed[node_index] = true;
      }
    });
  }

  IndexMaskMemory memory;
  IndexMask mask = IndexMask::from_bools(node_changed, memory);
  mask.foreach_index(GrainSize(1), [&](const int64_t index) {
    undo::push_node(object, nodes[index], undo::Type::HideVert);
  });

  BitGroupVector<> &last_buffer = buffers.write_buffer(iterations - 1);
  grid_hidden = std::move(last_buffer);

  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (const int node_index : range) {
      if (!node_changed[node_index]) {
        continue;
      }
      PBVHNode *node = nodes[node_index];

      BKE_pbvh_node_mark_update_visibility(node);
      bke::pbvh::node_update_visibility_grids(grid_hidden, *node);
    }
  });

  multires_mark_as_modified(&depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
  BKE_pbvh_sync_visibility_from_verts(pbvh, &mesh);
}

static Array<bool> duplicate_visibility_bmesh(const Object &object)
{
  const SculptSession &ss = *object.sculpt;
  BMesh &bm = *ss.bm;
  Array<bool> result(bm.totvert);
  BM_mesh_elem_table_ensure(&bm, BM_VERT);
  for (const int i : result.index_range()) {
    result[i] = BM_elem_flag_test_bool(BM_vert_at_index(&bm, i), BM_ELEM_HIDDEN);
  }
  return result;
}

static void grow_shrink_visibility_bmesh(Object &object,
                                         const Span<PBVHNode *> nodes,
                                         const VisAction action,
                                         const int iterations)
{
  for (const int i : IndexRange(iterations)) {
    UNUSED_VARS(i);
    const Array<bool> prev_visibility = duplicate_visibility_bmesh(object);
    partialvis_update_bmesh_nodes(object, nodes, action, [&](BMVert *vert) {
      Vector<BMVert *, 64> neighbors;
      for (BMVert *neighbor : vert_neighbors_get_bmesh(*vert, neighbors)) {
        if (prev_visibility[BM_elem_index_get(neighbor)] == action_to_hide(action)) {
          return true;
        }
      }
      return false;
    });
  }
}

static int visibility_filter_exec(bContext *C, wmOperator *op)
{
  Object &object = *CTX_data_active_object(C);
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);

  PBVH &pbvh = *BKE_sculpt_object_pbvh_ensure(&depsgraph, &object);
  BLI_assert(BKE_object_sculpt_pbvh_get(&object) == &pbvh);

  const VisAction mode = VisAction(RNA_enum_get(op->ptr, "action"));

  Vector<PBVHNode *> nodes = bke::pbvh::search_gather(pbvh, {});

  const SculptSession &ss = *object.sculpt;
  int num_verts = SCULPT_vertex_count_get(ss);

  int iterations = RNA_int_get(op->ptr, "iterations");

  if (RNA_boolean_get(op->ptr, "auto_iteration_count")) {
    /* Automatically adjust the number of iterations based on the number
     * of vertices in the mesh. */
    iterations = int(num_verts / VERTEX_ITERATION_THRESHOLD) + 1;
  }

  undo::push_begin(object, op);
  switch (BKE_pbvh_type(pbvh)) {
    case PBVH_FACES:
      grow_shrink_visibility_mesh(object, nodes, mode, iterations);
      break;
    case PBVH_GRIDS:
      grow_shrink_visibility_grid(depsgraph, object, pbvh, nodes, mode, iterations);
      break;
    case PBVH_BMESH:
      grow_shrink_visibility_bmesh(object, nodes, mode, iterations);
      break;
  }
  undo::push_end(object);

  SCULPT_topology_islands_invalidate(*object.sculpt);
  tag_update_visibility(*C);

  return OPERATOR_FINISHED;
}

void PAINT_OT_visibility_filter(wmOperatorType *ot)
{
  static EnumPropertyItem actions[] = {
      {int(VisAction::Show),
       "GROW",
       0,
       "Grow Visibility",
       "Grow the visibility by one face based on mesh topology"},
      {int(VisAction::Hide),
       "SHRINK",
       0,
       "Shrink Visibility",
       "Shrink the visibility by one face based on mesh topology"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Visibility Filter";
  ot->idname = "PAINT_OT_visibility_filter";
  ot->description = "Edit the visibility of the current mesh";

  ot->exec = visibility_filter_exec;
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "action", actions, int(VisAction::Show), "Action", "");

  RNA_def_int(ot->srna,
              "iterations",
              1,
              1,
              100,
              "Iterations",
              "Number of times that the filter is going to be applied",
              1,
              100);
  RNA_def_boolean(
      ot->srna,
      "auto_iteration_count",
      true,
      "Auto Iteration Count",
      "Use an automatic number of iterations based on the number of vertices of the sculpt");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gesture-based Visibility Operators
 * Operators that act upon a user-selected area.
 * \{ */

struct HideShowOperation {
  gesture::Operation op;

  VisAction action;
};

static void partialvis_gesture_update_mesh(gesture::GestureData &gesture_data)
{
  HideShowOperation *operation = reinterpret_cast<HideShowOperation *>(gesture_data.operation);
  Object *object = gesture_data.vc.obact;
  const VisAction action = operation->action;
  const Span<PBVHNode *> nodes = gesture_data.nodes;

  PBVH &pbvh = *object->sculpt->pbvh;
  Mesh *mesh = static_cast<Mesh *>(object->data);
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  if (action == VisAction::Show && !attributes.contains(".hide_vert")) {
    /* If everything is already visible, don't do anything. */
    return;
  }

  const bool value = action_to_hide(action);
  const Span<float3> positions = BKE_pbvh_get_vert_positions(pbvh);
  const Span<float3> normals = BKE_pbvh_get_vert_normals(pbvh);
  vert_hide_update(*object, nodes, [&](const Span<int> verts, MutableSpan<bool> hide) {
    for (const int i : verts.index_range()) {
      if (gesture::is_affected(gesture_data, positions[verts[i]], normals[verts[i]])) {
        hide[i] = value;
      }
    }
  });
}

static void partialvis_gesture_update_grids(Depsgraph &depsgraph,
                                            gesture::GestureData &gesture_data)
{
  HideShowOperation *operation = reinterpret_cast<HideShowOperation *>(gesture_data.operation);
  Object *object = gesture_data.vc.obact;
  const VisAction action = operation->action;
  const Span<PBVHNode *> nodes = gesture_data.nodes;

  PBVH &pbvh = *object->sculpt->pbvh;
  SubdivCCG *subdiv_ccg = object->sculpt->subdiv_ccg;

  const bool value = action_to_hide(action);
  const CCGKey key = *BKE_pbvh_get_grid_key(pbvh);
  const Span<CCGElem *> grids = subdiv_ccg->grids;
  grid_hide_update(
      depsgraph, *object, nodes, [&](const int grid_index, MutableBoundedBitSpan hide) {
        CCGElem *grid = grids[grid_index];
        for (const int y : IndexRange(key.grid_size)) {
          for (const int x : IndexRange(key.grid_size)) {
            CCGElem *elem = CCG_grid_elem(key, grid, x, y);
            if (gesture::is_affected(gesture_data, CCG_elem_co(key, elem), CCG_elem_no(key, elem)))
            {
              hide[y * key.grid_size + x].set(value);
            }
          }
        }
      });
}

static void partialvis_gesture_update_bmesh(gesture::GestureData &gesture_data)
{
  const auto selection_test_fn = [&](const BMVert *v) {
    return gesture::is_affected(gesture_data, v->co, v->no);
  };

  HideShowOperation *operation = reinterpret_cast<HideShowOperation *>(gesture_data.operation);

  partialvis_update_bmesh_nodes(
      *gesture_data.vc.obact, gesture_data.nodes, operation->action, selection_test_fn);
}

static void hide_show_begin(bContext &C, wmOperator &op, gesture::GestureData & /*gesture_data*/)
{
  Object *ob = CTX_data_active_object(&C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);

  undo::push_begin(*ob, &op);
  BKE_sculpt_object_pbvh_ensure(depsgraph, ob);
}

static void hide_show_apply_for_symmetry_pass(bContext &C, gesture::GestureData &gesture_data)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);

  switch (BKE_pbvh_type(*gesture_data.ss->pbvh)) {
    case PBVH_FACES:
      partialvis_gesture_update_mesh(gesture_data);
      break;
    case PBVH_GRIDS:
      partialvis_gesture_update_grids(*depsgraph, gesture_data);
      break;
    case PBVH_BMESH:
      partialvis_gesture_update_bmesh(gesture_data);
      break;
  }
}
static void hide_show_end(bContext &C, gesture::GestureData &gesture_data)
{
  SCULPT_topology_islands_invalidate(*gesture_data.vc.obact->sculpt);
  tag_update_visibility(C);
  undo::push_end(*gesture_data.vc.obact);
}

static void hide_show_init_properties(bContext & /*C*/,
                                      gesture::GestureData &gesture_data,
                                      wmOperator &op)
{
  gesture_data.operation = reinterpret_cast<gesture::Operation *>(
      MEM_cnew<HideShowOperation>(__func__));

  HideShowOperation *operation = reinterpret_cast<HideShowOperation *>(gesture_data.operation);

  operation->op.begin = hide_show_begin;
  operation->op.apply_for_symmetry_pass = hide_show_apply_for_symmetry_pass;
  operation->op.end = hide_show_end;

  operation->action = VisAction(RNA_enum_get(op.ptr, "action"));
  gesture_data.selection_type = gesture::SelectionType(RNA_enum_get(op.ptr, "area"));
}

static int hide_show_gesture_box_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_box(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  hide_show_init_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static int hide_show_gesture_lasso_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_lasso(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  hide_show_init_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static int hide_show_gesture_line_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_line(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  hide_show_init_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static int hide_show_gesture_polyline_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_polyline(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  hide_show_init_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static void hide_show_operator_gesture_properties(wmOperatorType *ot)
{
  static const EnumPropertyItem area_items[] = {
      {int(gesture::SelectionType::Outside),
       "OUTSIDE",
       0,
       "Outside",
       "Hide or show vertices outside the selection"},
      {int(gesture::SelectionType::Inside),
       "Inside",
       0,
       "Inside",
       "Hide or show vertices inside the selection"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_enum(ot->srna,
               "area",
               area_items,
               int(gesture::SelectionType::Inside),
               "Visibility Area",
               "Which vertices to hide or show");
}

void PAINT_OT_hide_show(wmOperatorType *ot)
{
  ot->name = "Hide/Show";
  ot->idname = "PAINT_OT_hide_show";
  ot->description = "Hide/show some vertices";

  ot->invoke = WM_gesture_box_invoke;
  ot->modal = WM_gesture_box_modal;
  ot->exec = hide_show_gesture_box_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_border(ot);
  hide_show_operator_properties(ot);
  hide_show_operator_gesture_properties(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Box);
}

void PAINT_OT_hide_show_lasso_gesture(wmOperatorType *ot)
{
  ot->name = "Hide/Show Lasso";
  ot->idname = "PAINT_OT_hide_show_lasso_gesture";
  ot->description = "Hide/show some vertices";

  ot->invoke = WM_gesture_lasso_invoke;
  ot->modal = WM_gesture_lasso_modal;
  ot->exec = hide_show_gesture_lasso_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER | OPTYPE_DEPENDS_ON_CURSOR;

  WM_operator_properties_gesture_lasso(ot);
  hide_show_operator_properties(ot);
  hide_show_operator_gesture_properties(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Lasso);
}

void PAINT_OT_hide_show_line_gesture(wmOperatorType *ot)
{
  ot->name = "Hide/Show Line";
  ot->idname = "PAINT_OT_hide_show_line_gesture";
  ot->description = "Hide/show some vertices";

  ot->invoke = WM_gesture_straightline_active_side_invoke;
  ot->modal = WM_gesture_straightline_oneshot_modal;
  ot->exec = hide_show_gesture_line_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
  hide_show_operator_properties(ot);
  hide_show_operator_gesture_properties(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Line);
}

void PAINT_OT_hide_show_polyline_gesture(wmOperatorType *ot)
{
  ot->name = "Hide/Show Polyline";
  ot->idname = "PAINT_OT_hide_show_polyline_gesture";
  ot->description = "Hide/show some vertices";

  ot->invoke = WM_gesture_polyline_invoke;
  ot->modal = WM_gesture_polyline_modal;
  ot->exec = hide_show_gesture_polyline_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER | OPTYPE_DEPENDS_ON_CURSOR;

  WM_operator_properties_gesture_polyline(ot);
  hide_show_operator_properties(ot);
  hide_show_operator_gesture_properties(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Lasso);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::hide
