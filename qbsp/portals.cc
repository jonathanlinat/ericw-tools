/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/
// portals.c

#include <qbsp/brush.hh>
#include <qbsp/portals.hh>

#include <qbsp/map.hh>
#include <qbsp/brushbsp.hh>
#include <qbsp/qbsp.hh>
#include <qbsp/outside.hh>
#include <qbsp/tree.hh>
// TEMP
#include <qbsp/writebsp.hh>

#include <atomic>

#include "tbb/task_group.h"
#include "common/vectorutils.hh"

contentflags_t ClusterContents(const node_t *node)
{
    /* Pass the leaf contents up the stack */
    if (node->is_leaf)
        return node->contents;

    return qbsp_options.target_game->cluster_contents(
        ClusterContents(node->children[0]), ClusterContents(node->children[1]));
}

/*
=============
Portal_VisFlood

Returns true if the portal is empty or translucent, allowing
the PVS calculation to see through it.
The nodes on either side of the portal may actually be clusters,
not leafs, so all contents should be ored together
=============
*/
bool Portal_VisFlood(const portal_t *p)
{
    if (!p->onnode) {
        return false; // to global outsideleaf
    }

    contentflags_t contents0 = ClusterContents(p->nodes[0]);
    contentflags_t contents1 = ClusterContents(p->nodes[1]);

    /* Can't see through func_illusionary_visblocker */
    if (contents0.illusionary_visblocker || contents1.illusionary_visblocker)
        return false;

    // Check per-game visibility
    return qbsp_options.target_game->portal_can_see_through(
        contents0, contents1, qbsp_options.transwater.value(), qbsp_options.transsky.value());
}

/*
===============
Portal_EntityFlood

The entity flood determines which areas are
"outside" on the map, which are then filled in.
Flowing from side s to side !s
===============
*/
bool Portal_EntityFlood(const portal_t *p, int32_t s)
{
    if (!p->nodes[0]->is_leaf || !p->nodes[1]->is_leaf) {
        FError("Portal_EntityFlood: not a leaf");
    }

    // can never cross to a solid
    if (p->nodes[0]->contents.is_any_solid(qbsp_options.target_game) ||
        p->nodes[1]->contents.is_any_solid(qbsp_options.target_game)) {
        return false;
    }

    // can flood through everything else
    return true;
}

/*
=============
AddPortalToNodes
=============
*/
static void AddPortalToNodes(portal_t *p, node_t *front, node_t *back)
{
    if (p->nodes[0] || p->nodes[1])
        FError("portal already included");

    p->nodes[0] = front;
    p->next[0] = front->portals;
    front->portals = p;

    p->nodes[1] = back;
    p->next[1] = back->portals;
    back->portals = p;
}

/*
================
MakeHeadnodePortals

The created portals will face the global outside_node
================
*/
std::list<std::unique_ptr<buildportal_t>> MakeHeadnodePortals(tree_t *tree)
{
    int i, j, n;
    std::array<std::unique_ptr<buildportal_t>, 6> portals;
    qplane3d bplanes[6];

    // pad with some space so there will never be null volume leafs
    aabb3d bounds = tree->bounds.grow(SIDESPACE);

    tree->outside_node.is_leaf = true;
    tree->outside_node.contents = qbsp_options.target_game->create_solid_contents();
    tree->outside_node.portals = NULL;

    // create 6 portals forming a cube around the bounds of the map.
    // these portals will have `outside_node` on one side, and headnode on the other.
    for (i = 0; i < 3; i++)
        for (j = 0; j < 2; j++) {
            n = j * 3 + i;

            portals[n] = std::make_unique<buildportal_t>();
            auto *p = portals[n].get();

            qplane3d &pl = bplanes[n] = {};

            if (j) {
                pl.normal[i] = -1;
                pl.dist = -bounds[j][i];
            } else {
                pl.normal[i] = 1;
                pl.dist = bounds[j][i];
            }
            bool side = p->plane.set_plane(pl, true);

            p->winding = std::make_unique<winding_t>(BaseWindingForPlane(pl));
            if (side) {
                p->set_nodes(&tree->outside_node, tree->headnode);
            } else {
                p->set_nodes(tree->headnode, &tree->outside_node);
            }
        }

    // clip the basewindings by all the other planes
    for (i = 0; i < 6; i++) {
        winding_t &w = *portals[i]->winding.get();

        for (j = 0; j < 6; j++) {
            if (j == i)
                continue;

            if (auto w2 = w.clip_front(bplanes[j], qbsp_options.epsilon.value(), true)) {
                w = std::move(*w2);
            } else {
                FError("portal winding clipped away");
            }
        }
    }

    // move into std::list
    std::list<std::unique_ptr<buildportal_t>> result;
    for (auto &p : portals) {
        result.push_back(std::move(p));
    }
    return result;
}

//============================================================================

/*
================
BaseWindingForNode

Creates a winding from the given node plane, clipped by all parent nodes.
================
*/
constexpr vec_t BASE_WINDING_EPSILON = 0.001;
constexpr vec_t SPLIT_WINDING_EPSILON = 0.001;

static std::optional<winding_t> BaseWindingForNode(const node_t *node)
{
    std::optional<winding_t> w = BaseWindingForPlane(node->get_plane());

    // clip by all the parents
    for (auto *np = node->parent; np && w;) {

        if (np->children[0] == node) {
            w = w->clip_front(np->get_plane(), BASE_WINDING_EPSILON, false);
        } else {
            w = w->clip_back(np->get_plane(), BASE_WINDING_EPSILON, false);
        }

        node = np;
        np = np->parent;
    }

    return w;
}

/*
==================
MakeNodePortal

create the new portal by taking the full plane winding for the cutting plane
and clipping it by all of parents of this node, as well as all the other
portals in the node.
==================
*/
std::unique_ptr<buildportal_t> MakeNodePortal(node_t *node, const std::list<std::unique_ptr<buildportal_t>> &boundary_portals, portalstats_t &stats)
{
    auto w = BaseWindingForNode(node);

    // clip the portal by all the other portals in the node
    for (auto &p : boundary_portals) {
        if (!w) {
            break;
        }
        qplane3d plane;

        if (p->nodes[0] == node) {
            plane = p->plane;
        } else if (p->nodes[1] == node) {
            plane = -p->plane;
        } else {
            Error("CutNodePortals_r: mislinked portal");
        }

        // fixme-brushbsp: magic number
        w = w->clip_front(plane, 0.1, false);
    }

    if (!w) {
        return {};
    }

    if (WindingIsTiny(*w)) {
        stats.c_tinyportals++;
        return {};
    }

    auto new_portal = std::make_unique<buildportal_t>();
    new_portal->plane = node->get_plane();
    new_portal->onnode = node;
    new_portal->winding = std::make_unique<winding_t>(*w);
    new_portal->set_nodes(node->children[0], node->children[1]);

    return new_portal;
}

/*
==============
SplitNodePortals

Move or split the portals that bound node so that the node's
children have portals instead of node.
==============
*/
twosided<std::list<std::unique_ptr<buildportal_t>>> SplitNodePortals(const node_t *node, std::list<std::unique_ptr<buildportal_t>> boundary_portals, portalstats_t &stats)
{
    const auto &plane = node->get_plane();
    node_t *f = node->children[0];
    node_t *b = node->children[1];

    twosided<std::list<std::unique_ptr<buildportal_t>>> result;

    for (auto&& p : boundary_portals) {
        // which side of p `node` is on
        planeside_t side;
        if (p->nodes[SIDE_FRONT] == node)
            side = SIDE_FRONT;
        else if (p->nodes[SIDE_BACK] == node)
            side = SIDE_BACK;
        else
            FError("CutNodePortals_r: mislinked portal");

        node_t *other_node = p->nodes[!side];
        p->set_nodes(nullptr, nullptr);

        //
        // cut the portal into two portals, one on each side of the cut plane
        //
        auto [frontwinding, backwinding] = p->winding->clip(plane, SPLIT_WINDING_EPSILON, true);

        if (frontwinding && WindingIsTiny(*frontwinding)) {
            frontwinding = {};
            stats.c_tinyportals++;
        }

        if (backwinding && WindingIsTiny(*backwinding)) {
            backwinding = {};
            stats.c_tinyportals++;
        }

        if (!frontwinding && !backwinding) { // tiny windings on both sides
            continue;
        }

        if (!frontwinding) {
            if (side == SIDE_FRONT)
                p->set_nodes(b, other_node);
            else
                p->set_nodes(other_node, b);

            result.back.push_back(std::move(p));
            continue;
        }
        if (!backwinding) {
            if (side == SIDE_FRONT)
                p->set_nodes(f, other_node);
            else
                p->set_nodes(other_node, f);

            result.front.push_back(std::move(p));
            continue;
        }

        // the winding is split
        auto new_portal = std::make_unique<buildportal_t>();
        new_portal->plane = p->plane;
        new_portal->onnode = p->onnode;
        new_portal->nodes[0] = p->nodes[0];
        new_portal->nodes[1] = p->nodes[1];
        new_portal->winding = std::make_unique<winding_t>(*backwinding);
        p->winding = std::make_unique<winding_t>(*frontwinding);

        if (side == SIDE_FRONT) {
            p->set_nodes(f, other_node);
            new_portal->set_nodes(b, other_node);
        } else {
            p->set_nodes(other_node, f);
            new_portal->set_nodes(other_node, b);
        }

        result.front.push_back(std::move(p));
        result.back.push_back(std::move(new_portal));
    }

    return result;
}

/*
================
MakePortalsFromBuildportals
================
*/
void MakePortalsFromBuildportals(tree_t *tree, std::list<std::unique_ptr<buildportal_t>> buildportals)
{
    tree->portals.reserve(buildportals.size());
    for (const auto& buildportal : buildportals) {
        portal_t *new_portal = tree->create_portal();
        new_portal->plane = buildportal->plane;
        new_portal->onnode = buildportal->onnode;
        new_portal->winding = std::move(buildportal->winding);
        AddPortalToNodes(new_portal, buildportal->nodes[0], buildportal->nodes[1]);
    }
}

/*
================
CalcNodeBounds
================
*/
void CalcNodeBounds(node_t *node)
{
    // calc mins/maxs for both leafs and nodes
    node->bounds = aabb3d{};

    for (portal_t *p = node->portals; p;) {
        int s = (p->nodes[1] == node);
        for (auto &point : *p->winding) {
            node->bounds += point;
        }
        p = p->next[s];
    }
}

static void CalcTreeBounds_r(tree_t *tree, node_t *node)
{
    if (node->is_leaf) {
        CalcNodeBounds(node);
    } else {
        tbb::task_group g;
        g.run([&]() { CalcTreeBounds_r(tree, node->children[0]); });
        g.run([&]() { CalcTreeBounds_r(tree, node->children[1]); });
        g.wait();

        node->bounds = node->children[0]->bounds + node->children[1]->bounds;
    }

    if (node->bounds.mins()[0] >= node->bounds.maxs()[0]) {
        logging::print("WARNING: {} without a volume\n", node->is_leaf ? "leaf" : "node");

        // fixme-brushbsp: added this to work around leafs with no portals showing up in "qbspfeatures.map" among other
        // test maps. Not sure if correct or there's another underlying problem.
        node->bounds = {node->parent->bounds.mins(), node->parent->bounds.mins()};
    }

    for (auto &v : node->bounds.mins()) {
        if (fabs(v) > qbsp_options.worldextent.value()) {
            logging::print("WARNING: {} with unbounded volume\n", node->is_leaf ? "leaf" : "node");
            break;
        }
    }
}

/*
==================
ClipNodePortalToTree_r

Given portals which are connected to `node` on one side,
descends the tree, splitting the portals as needed until they are connected to leaf nodes.

The other side of the portals will remain untouched.
==================
*/
static std::list<std::unique_ptr<buildportal_t>> ClipNodePortalsToTree_r(node_t *node, portaltype_t type, std::list<std::unique_ptr<buildportal_t>> portals, portalstats_t &stats)
{
    if (portals.empty()) {
        return portals;
    }
    if (node->is_leaf || (type == portaltype_t::VIS && node->detail_separator)) {
        return portals;
    }

    auto boundary_portals_split = SplitNodePortals(node, std::move(portals), stats);

    auto front_fragments = ClipNodePortalsToTree_r(node->children[0], type, std::move(boundary_portals_split.front), stats);
    auto back_fragments = ClipNodePortalsToTree_r(node->children[1], type, std::move(boundary_portals_split.back), stats);

    std::list<std::unique_ptr<buildportal_t>> merged_result;
    merged_result.splice(merged_result.end(), front_fragments);
    merged_result.splice(merged_result.end(), back_fragments);
    return merged_result;
}

/*
==================
MakeTreePortals_r

Given the list of portals bounding `node`, returns the portal list for a fully-portalized `node`.
==================
*/
std::list<std::unique_ptr<buildportal_t>> MakeTreePortals_r(tree_t *tree, node_t *node, portaltype_t type, std::list<std::unique_ptr<buildportal_t>> boundary_portals, portalstats_t &stats, logging::percent_clock &clock)
{
    clock.increase();

    if (node->is_leaf || (type == portaltype_t::VIS && node->detail_separator)) {
        return boundary_portals;
    }

    // make the node portal before we move out the boundary_portals
    std::unique_ptr<buildportal_t> nodeportal = MakeNodePortal(node, boundary_portals, stats);

    // parallel part: split boundary_portals between the front and back, and obtain the fully
    // portalized front/back sides in parallel

    auto boundary_portals_split = SplitNodePortals(node, std::move(boundary_portals), stats);

    std::list<std::unique_ptr<buildportal_t>> result_portals_front, result_portals_back;

    tbb::task_group g;
    g.run([&]() { result_portals_front = MakeTreePortals_r(tree, node->children[0], type, std::move(boundary_portals_split.front), stats, clock); });
    g.run([&]() { result_portals_back = MakeTreePortals_r(tree, node->children[1], type, std::move(boundary_portals_split.back), stats, clock); });
    g.wait();

    // sequential part: push the nodeportal down each side of the bsp so it connects leafs

    std::list<std::unique_ptr<buildportal_t>> result_portals_onnode;
    if (nodeportal) {
        // to start with, `nodeportal` is a portal between node->children[0] and node->children[1]

        // these portal fragments have node->children[1] on one side, and the leaf nodes from
        // node->children[0] on the other side
        std::list<std::unique_ptr<buildportal_t>> half_clipped =
            ClipNodePortalsToTree_r(node->children[0], type, make_list(std::move(nodeportal)), stats);

        for (auto &clipped_p : ClipNodePortalsToTree_r(node->children[1], type, std::move(half_clipped), stats)) {
            result_portals_onnode.push_back(std::move(clipped_p));
        }
    }

    // all done, merge together the lists and return
    std::list<std::unique_ptr<buildportal_t>> merged_result;
    merged_result.splice(merged_result.end(), result_portals_front);
    merged_result.splice(merged_result.end(), result_portals_back);
    merged_result.splice(merged_result.end(), result_portals_onnode);
    return merged_result;
}

/*
==================
MakeTreePortals
==================
*/
void MakeTreePortals(tree_t *tree)
{
    logging::funcheader();

    FreeTreePortals(tree);

    portalstats_t stats{};

    auto headnodeportals = MakeHeadnodePortals(tree);

    {
        logging::percent_clock clock;
        clock.max = tree->nodes.size() + 1;

        auto buildportals = MakeTreePortals_r(tree, tree->headnode, portaltype_t::TREE, std::move(headnodeportals), stats, clock);

        MakePortalsFromBuildportals(tree, std::move(buildportals));
    }

    logging::header("CalcTreeBounds");

    CalcTreeBounds_r(tree, tree->headnode);

    logging::print(logging::flag::STAT, "       {:8} tiny portals\n", stats.c_tinyportals);
    logging::print(logging::flag::STAT, "       {:8} tree portals\n", tree->portals.size());
}

/*
=========================================================

FLOOD AREAS

=========================================================
*/

static void ApplyArea_r(node_t *node)
{
    node->area = map.c_areas;

    if (!node->is_leaf) {
        ApplyArea_r(node->children[0]);
        ApplyArea_r(node->children[1]);
    }
}

static mapentity_t *AreanodeEntityForLeaf(node_t *node)
{
    // if detail cluster, search the children recursively
    if (!node->is_leaf) {
        if (auto *child0result = AreanodeEntityForLeaf(node->children[0]); child0result) {
            return child0result;
        }
        return AreanodeEntityForLeaf(node->children[1]);
    }

    for (auto &brush : node->original_brushes) {
        if (brush->mapbrush->func_areaportal) {
            return brush->mapbrush->func_areaportal;
        }
    }
    return nullptr;
}

/*
=============
FloodAreas_r
=============
*/
static void FloodAreas_r(node_t *node)
{
    if ((node->is_leaf || node->detail_separator) && (ClusterContents(node).native & Q2_CONTENTS_AREAPORTAL)) {
        // grab the func_areanode entity
        mapentity_t *entity = AreanodeEntityForLeaf(node);

        if (entity == nullptr) {
            logging::print("WARNING: areaportal contents in node, but no entity found {} -> {}\n", node->bounds.mins(),
                node->bounds.maxs());
            return;
        }

        // this node is part of an area portal;
        // if the current area has allready touched this
        // portal, we are done
        if (entity->portalareas[0] == map.c_areas || entity->portalareas[1] == map.c_areas)
            return;

        // note the current area as bounding the portal
        if (entity->portalareas[1]) {
            logging::print("WARNING: areaportal entity {} touches > 2 areas\n  Entity Bounds: {} -> {}\n",
                entity - map.entities.data(), entity->bounds.mins(), entity->bounds.maxs());
            return;
        }

        if (entity->portalareas[0])
            entity->portalareas[1] = map.c_areas;
        else
            entity->portalareas[0] = map.c_areas;

        return;
    }

    if (node->area)
        return; // already got it

    node->area = map.c_areas;

    // propagate area assignment to descendants if we're a cluster
    if (!node->is_leaf) {
        ApplyArea_r(node);
    }

    int32_t s;

    for (portal_t *p = node->portals; p; p = p->next[s]) {
        s = (p->nodes[1] == node);
#if 0
		if (p->nodes[!s]->occupied)
			continue;
#endif
        if (!Portal_EntityFlood(p, s))
            continue;

        FloodAreas_r(p->nodes[!s]);
    }
}

/*
=============
FindAreas_r

Just decend the tree, and for each node that hasn't had an
area set, flood fill out from there
=============
*/
static void FindAreas(node_t *node)
{
    auto leafs = FindOccupiedClusters(node);
    for (auto *leaf : leafs) {
        if (leaf->area)
            continue;

        // area portals are always only flooded into, never
        // out of
        if (ClusterContents(leaf).native & Q2_CONTENTS_AREAPORTAL)
            continue;

        map.c_areas++;
        FloodAreas_r(leaf);
    }
}

/*
=============
SetAreaPortalAreas_r

Just decend the tree, and for each node that hasn't had an
area set, flood fill out from there
=============
*/
static void SetAreaPortalAreas_r(node_t *node)
{
    if (!node->is_leaf) {
        SetAreaPortalAreas_r(node->children[0]);
        SetAreaPortalAreas_r(node->children[1]);
        return;
    }

    if (node->contents.native != Q2_CONTENTS_AREAPORTAL)
        return;

    if (node->area)
        return; // already set

    // grab the func_areanode entity
    mapentity_t *entity = AreanodeEntityForLeaf(node);

    if (!entity) {
        logging::print("WARNING: areaportal missing for node: {} -> {}\n", node->bounds.mins(), node->bounds.maxs());
        return;
    }

    node->area = entity->portalareas[0];
    if (!entity->portalareas[1]) {
        logging::print("WARNING: areaportal entity {} with targetname {} doesn't touch two areas\n  Node bounds: {} -> {}\n",
            entity - map.entities.data(), entity->epairs.get("targetname"), node->bounds.mins(), node->bounds.maxs());
        return;
    }
}

/*
=============
EmitAreaPortals

=============
*/
void EmitAreaPortals(node_t *headnode)
{
    logging::funcheader();

    map.bsp.dareaportals.emplace_back();
    map.bsp.dareas.emplace_back();

    for (size_t i = 1; i <= map.c_areas; i++) {
        darea_t &area = map.bsp.dareas.emplace_back();
        area.firstareaportal = map.bsp.dareaportals.size();

        for (auto &e : map.entities) {

            if (!e.areaportalnum)
                continue;
            dareaportal_t dp = {};

            if (e.portalareas[0] == i) {
                dp.portalnum = e.areaportalnum;
                dp.otherarea = e.portalareas[1];
            } else if (e.portalareas[1] == i) {
                dp.portalnum = e.areaportalnum;
                dp.otherarea = e.portalareas[0];
            }

            size_t j = 0;

            for (; j < map.bsp.dareaportals.size(); j++) {
                if (map.bsp.dareaportals[j] == dp)
                    break;
            }

            if (j == map.bsp.dareaportals.size())
                map.bsp.dareaportals.push_back(dp);
        }

        area.numareaportals = map.bsp.dareaportals.size() - area.firstareaportal;
    }

    logging::print(logging::flag::STAT, "{:5} numareas\n", map.bsp.dareas.size());
    logging::print(logging::flag::STAT, "{:5} numareaportals\n", map.bsp.dareaportals.size());
}

/*
=============
FloodAreas

Mark each leaf with an area, bounded by CONTENTS_AREAPORTAL
=============
*/
void FloodAreas(mapentity_t *entity, node_t *headnode)
{
    logging::funcheader();
    FindAreas(headnode);
    SetAreaPortalAreas_r(headnode);
    logging::print(logging::flag::STAT, "{:5} areas\n", map.c_areas);
}

//==============================================================

/*
============
FindPortalSide

Finds a brush side to use for texturing the given portal
============
*/
static void FindPortalSide(portal_t *p)
{
    // decide which content change is strongest
    // solid > lava > water, etc

    // if either is "_noclipfaces" then we don't require a content change
    contentflags_t viscontents =
        qbsp_options.target_game->portal_visible_contents(p->nodes[0]->contents, p->nodes[1]->contents);
    if (viscontents.is_empty(qbsp_options.target_game))
        return;

    // bestside[0] is the brushside visible on portal side[0] which is the positive side of the plane, always
    side_t *bestside[2] = {nullptr, nullptr};
    side_t *exactside[2] = {nullptr, nullptr};
    float bestdot = 0;
    const qbsp_plane_t &p1 = p->onnode->get_plane();

    // check brushes on both sides of the portal
    for (int j = 0; j < 2; j++) {
        node_t *n = p->nodes[j];

        // iterate the n->original_brushes vector in reverse order, so later brushes
        // in the map file order are prioritized
        for (auto it = n->original_brushes.rbegin(); it != n->original_brushes.rend(); ++it) {
            auto *brush = *it;
            const bool generate_outside_face =
                qbsp_options.target_game->portal_generates_face(viscontents, brush->contents, SIDE_FRONT);
            const bool generate_inside_face =
                qbsp_options.target_game->portal_generates_face(viscontents, brush->contents, SIDE_BACK);

            if (!(generate_outside_face || generate_inside_face)) {
                continue;
            }

            for (auto &side : brush->sides) {
                if (side.bevel)
                    continue;
                if ((side.planenum & ~1) == p->onnode->planenum) {
                    // exact match (undirectional)

                    // because the brush is on j of the positive plane, the brushside must be facing away from j
                    Q_assert((side.planenum & 1) == !j);

                    // see which way(s) we want to generate faces - we could be a brush on either side of
                    // the portal, generating either a outward face (common case) or an inward face (liquids) or both.
                    if (generate_outside_face) {
                        // since we are iterating the brushes from highest priority (last) to lowest, take the first exactside we find
                        if (!exactside[!j]) {
                            exactside[!j] = &side;
                        }
                    }
                    if (generate_inside_face) {
                        if (!exactside[j]) {
                            exactside[j] = &side;
                        }
                    }

                    break;
                }
                // see how close the match is
                const auto &p2 = side.get_positive_plane();
                double dot = qv::dot(p1.get_normal(), p2.get_normal());
                if (dot > bestdot) {
                    bestdot = dot;
                    if (generate_outside_face) {
                        bestside[!j] = &side;
                    }
                    if (generate_inside_face) {
                        bestside[j] = &side;
                    }
                }
            }
        }
    }

    // take exact sides over best sides
    for (int i = 0; i < 2; ++i) {
        if (exactside[i]) {
            bestside[i] = exactside[i];
        }
    }

    if (!bestside[0] && !bestside[1])
        logging::print("WARNING: side not found for portal\n");

    p->sidefound = true;

    for (int i = 0; i < 2; ++i) {
        p->sides[i] = bestside[i];
    }
}

/*
===============
MarkVisibleSides_r

===============
*/
static void MarkVisibleSides_r(node_t *node)
{
    if (!node->is_leaf) {
        MarkVisibleSides_r(node->children[0]);
        MarkVisibleSides_r(node->children[1]);
        return;
    }

    // empty leafs are never boundary leafs
    if (node->contents.is_empty(qbsp_options.target_game))
        return;

    // see if there is a visible face
    int s;
    for (portal_t *p = node->portals; p; p = p->next[!s]) {
        s = (p->nodes[0] == node);
        if (!p->onnode)
            continue; // edge of world
        if (!p->sidefound)
            FindPortalSide(p);
        for (int i = 0; i < 2; ++i) {
            if (p->sides[i] && p->sides[i]->source) {
                p->sides[i]->source->visible = true;
            }
        }
    }
}

/*
=============
MarkVisibleSides

=============
*/
void MarkVisibleSides(tree_t *tree, mapentity_t *entity, bspbrush_t::container &brushes)
{
    logging::funcheader();

    // clear all the visible flags
    for (auto &brush : brushes) {
        for (auto &face : brush->sides) {
            if (face.source) {
                face.source->visible = false;
            }
        }
    }

    // set visible flags on the sides that are used by portals
    MarkVisibleSides_r(tree->headnode);
}
