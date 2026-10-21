#include "NativeCompose.hpp"

#include "NativeMods.hpp"
#include "NativeSupport.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Support/SupportLayer.hpp"
#include "libslic3r/Support/SupportMaterial.hpp"
#include "libslic3r/Support/TreeSupport.hpp"

#include "../../../mods/sdk/include/magpie_slot_support_compose.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// One compose call: the object, the support sets the composer has made, and the first failure a host
// function caught (exceptions never cross into the mod; the core rethrows it once the mod returns).
struct mp_compose
{
    Slic3r::PrintObject                                  &object;
    std::map<mp_handle, Slic3r::SupportLayerPtrs>         sets;
    mp_handle                                             next = 1;
    bool                                                  merged = false;
    std::exception_ptr                                    error;
    std::string                                           mod_id; // whose texts host_status and host_warn translate
    explicit mp_compose(Slic3r::PrintObject &o) : object(o) {}
};

namespace Slic3r {
namespace ModHost {
namespace NativeCompose {

namespace {

void free_support_layers(SupportLayerPtrs &layers)
{
    for (SupportLayer *layer : layers)
        PrintObject::delete_support_layer(layer);
    layers.clear();
}

void cancel_check(const PrintObject &object)
{
    if (object.print()->canceled())
        throw CanceledException();
}

// Runs a host function's work; an exception is kept for the core to rethrow and becomes a status.
template<class Fn> int32_t guarded(mp_compose *c, Fn &&fn)
{
    if (c == nullptr)
        return MP_ERR_ARGUMENT;
    if (c->error)
        return MP_ERR_FAILED; // an earlier failure stops everything
    try {
        return fn();
    } catch (const CanceledException &) {
        c->error = std::current_exception();
        return MP_ERR_CANCELLED;
    } catch (...) {
        c->error = std::current_exception();
        return MP_ERR_FAILED;
    }
}

// A per-object-layer list the host returns, released by host_free_layers.
void to_layer_rings(const std::vector<Polygons> &layers, size_t layer_count, mp_layer_rings *out)
{
    auto *lists = static_cast<mp_rings *>(std::calloc(std::max<size_t>(layer_count, 1), sizeof(mp_rings)));
    for (size_t i = 0; i < layer_count && i < layers.size(); ++i) {
        const Polygons &polys = layers[i];
        auto *rings = static_cast<mp_ring *>(std::calloc(std::max<size_t>(polys.size(), 1), sizeof(mp_ring)));
        for (size_t r = 0; r < polys.size(); ++r) {
            const Points &pts = polys[r].points;
            auto         *dst = static_cast<mp_point *>(std::malloc(std::max<size_t>(pts.size(), 1) * sizeof(mp_point)));
            for (size_t k = 0; k < pts.size(); ++k)
                dst[k] = {pts[k].x(), pts[k].y()};
            rings[r] = {dst, uint32_t(pts.size())};
        }
        lists[i] = {rings, uint32_t(polys.size())};
    }
    *out = {lists, uint32_t(layer_count)};
}

// A mask the composer passes, one list per object layer.
std::vector<Polygons> from_layer_rings(const mp_rings *mask, size_t layer_count)
{
    std::vector<Polygons> out(layer_count);
    if (mask == nullptr)
        return out;
    for (size_t i = 0; i < layer_count; ++i) {
        out[i].reserve(mask[i].n);
        for (uint32_t r = 0; r < mask[i].n; ++r) {
            Polygon p;
            p.points.reserve(mask[i].rings[r].n);
            for (uint32_t k = 0; k < mask[i].rings[r].n; ++k)
                p.points.emplace_back(mask[i].rings[r].pts[k].x, mask[i].rings[r].pts[k].y);
            out[i].push_back(std::move(p));
        }
    }
    return out;
}

SupportMaterialStyle tree_style(const std::string &name)
{
    if (name == "tree_slim")
        return smsTreeSlim;
    if (name == "tree_strong")
        return smsTreeStrong;
    if (name == "tree_hybrid")
        return smsTreeHybrid;
    return smsTreeOrganic;
}

// Per object layer, the plastic of the normal support layers that overlap it in Z: the tree
// branches keep clear of it. Only extruded plastic counts; support_islands may span unfilled gaps.
std::vector<Polygons> tree_obstacles(const PrintObject &object, const SupportLayerPtrs &normal_layers)
{
    std::vector<Polygons> obstacles(object.layer_count());
    const coordf_t        raft_top = object.slicing_parameters().has_raft() ? object.slicing_parameters().raft_contact_top_z : 0.; // mm
    for (const SupportLayer *support_layer : normal_layers) {
        if (support_layer->print_z <= raft_top + EPSILON)
            continue;
        Polygons footprint = support_layer->support_fills.polygons_covered_by_width(float(SCALED_EPSILON));
        if (footprint.empty())
            continue;
        footprint = union_(footprint);
        const coordf_t bottom_z = support_layer->bottom_z();
        for (size_t layer_id = 0; layer_id < object.layer_count(); ++layer_id) {
            const Layer *object_layer = object.layers()[layer_id];
            if (object_layer->print_z + EPSILON < bottom_z)
                continue;
            if (object_layer->bottom_z() > support_layer->print_z + EPSILON)
                break;
            append(obstacles[layer_id], footprint);
        }
    }
    for (Polygons &layer : obstacles)
        if (!layer.empty())
            layer = union_(layer);
    return obstacles;
}

// Appends the parts of a tree path that do not run over the normal support's plastic. Both paths
// keep their full width: the owner coverage already includes the owner's width, and the cut is
// grown by this path's half width.
void append_residual_path(const ExtrusionPath &path, const ExPolygons &owner_coverage, std::unordered_map<coord_t, ExPolygons> &cuts,
                          ExtrusionEntityCollection &destination)
{
    if (owner_coverage.empty()) {
        destination.append(path);
        return;
    }
    // Half the residual path's width (scaled), at least SCALED_EPSILON.
    const coord_t half_width = std::max<coord_t>(SCALED_EPSILON, path.width > 0.f ? coord_t(scale_(0.5 * path.width)) : coord_t(0));
    auto [it, inserted]      = cuts.try_emplace(half_width);
    if (inserted)
        it->second = offset_ex(owner_coverage, float(half_width));
    path.subtract_expolygons(it->second, &destination);
}

void append_residual(const ExtrusionEntity &entity, const ExPolygons &owner_coverage, std::unordered_map<coord_t, ExPolygons> &cuts,
                     ExtrusionEntityCollection &destination)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            append_residual(*child, owner_coverage, cuts, destination);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        append_residual_path(*path, owner_coverage, cuts, destination);
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &p : multipath->paths)
            append_residual_path(p, owner_coverage, cuts, destination);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &p : loop->paths)
            append_residual_path(p, owner_coverage, cuts, destination);
    } else {
        throw Slic3r::SlicingError(_u8L("The support channels contain an extrusion that cannot be merged."));
    }
}

// Merges the two sets' layers by print Z into the object's support layers. Layers at the same Z (within
// EPSILON) become one multi-channel layer owned by the normal layer.
void merge_layers(PrintObject &object, SupportLayerPtrs normal_layers, SupportLayerPtrs tree_layers)
{
    SupportLayerPtrs &out = object.support_layers();
    const auto by_z = [](const SupportLayer *a, const SupportLayer *b) { return a->print_z < b->print_z; };
    std::stable_sort(normal_layers.begin(), normal_layers.end(), by_z);
    std::stable_sort(tree_layers.begin(), tree_layers.end(), by_z);
    size_t n = 0, t = 0;
    while (n < normal_layers.size() || t < tree_layers.size()) {
        if (t >= tree_layers.size() || (n < normal_layers.size() && normal_layers[n]->print_z + EPSILON < tree_layers[t]->print_z)) {
            out.push_back(normal_layers[n++]);
            continue;
        }
        if (n >= normal_layers.size() || tree_layers[t]->print_z + EPSILON < normal_layers[n]->print_z) {
            out.push_back(tree_layers[t++]);
            continue;
        }
        SupportLayer *normal = normal_layers[n++];
        SupportLayer *tree   = tree_layers[t++];
        normal->height       = std::min(normal->height, tree->height);
        normal->print_z      = 0.5 * (normal->print_z + tree->print_z);
        normal->support_type = stInnerMultiChannel;
        // H/2 support: the merged layer keeps both channels' source events.
        append(normal->half_layer_parent_bands, tree->half_layer_parent_bands);
        // The normal support owns overlaps: the composer gave the tree only what normal support cannot
        // reach, and letting a many-walled tree own them can erase the normal support entirely.
        const ExPolygons owner_coverage = union_ex(normal->support_fills.polygons_covered_by_width(float(SCALED_EPSILON)));
        std::unordered_map<coord_t, ExPolygons> cuts;
        for (const ExtrusionEntity *entity : tree->support_fills.entities)
            append_residual(*entity, owner_coverage, cuts, normal->support_fills);
        normal->support_fills.no_sort = normal->support_fills.no_sort || tree->support_fills.no_sort;
        append(normal->support_islands, std::move(tree->support_islands));
        if (!normal->support_islands.empty())
            normal->support_islands = union_ex(normal->support_islands);
        append(normal->lslices, std::move(tree->lslices));
        if (!normal->lslices.empty())
            normal->lslices = union_ex(normal->lslices);
        append(normal->base_areas, std::move(tree->base_areas));
        if (!normal->base_areas.empty())
            normal->base_areas = union_ex(normal->base_areas);
        PrintObject::delete_support_layer(tree);
        out.push_back(normal);
    }
    for (size_t i = 0; i < out.size(); ++i)
        out[i]->set_id(i);
}

// Orca's normal support's own overhang detection, support_type ignored: per object layer, the overhangs
// of its top contacts (or the contacts themselves).
std::vector<Polygons> orca_demand(const PrintObject &object)
{
    std::vector<Polygons>        demand(object.layer_count());
    SupportGeneratorLayerStorage storage;
    SupportMaterialOverrides     overrides;
    overrides.ignore_support_type = true;
    PrintObjectSupportMaterial detector(&object, object.slicing_parameters(), overrides);
    for (const SupportGeneratorLayer *contact : detector.detect_top_contact_layers(object, storage)) {
        if (contact == nullptr || contact->idx_object_layer_above >= demand.size())
            continue;
        if (contact->overhang_polygons && !contact->overhang_polygons->empty())
            append(demand[contact->idx_object_layer_above], *contact->overhang_polygons);
        else
            append(demand[contact->idx_object_layer_above], contact->polygons);
    }
    for (Polygons &layer : demand)
        if (!layer.empty())
            layer = union_(layer);
    return demand;
}

// The support.areas implementation `generator`; "orca" has none.
const NativeEntry *areas_generator(const std::string &generator)
{
    const NativeEntry *entry = NativeSupport::find(generator);
    if (entry == nullptr)
        throw Slic3r::SlicingError(Slic3r::format(_u8L("Support generator \"%1%\" is not installed."), generator));
    return entry;
}

mp_handle add_set(mp_compose *c, SupportLayerPtrs layers, uint32_t *support_layers)
{
    if (support_layers != nullptr)
        *support_layers = uint32_t(layers.size());
    const mp_handle h = c->next++;
    c->sets.emplace(h, std::move(layers));
    return h;
}

SupportLayerPtrs take_set(mp_compose *c, mp_handle h)
{
    if (h == 0)
        return {};
    auto it = c->sets.find(h);
    if (it == c->sets.end())
        throw Slic3r::SlicingError("support.compose: unknown support set");
    SupportLayerPtrs layers = std::move(it->second);
    c->sets.erase(it);
    return layers;
}

// ---- the host table ----

int32_t host_has_generator(mp_compose *c, const char *generator)
{
    if (c == nullptr || generator == nullptr)
        return 0;
    return std::string(generator) == "orca" || NativeSupport::find(generator) != nullptr ? 1 : 0;
}

int32_t host_demand(mp_compose *c, const char *generator, mp_layer_rings *out)
{
    if (generator == nullptr || out == nullptr)
        return MP_ERR_ARGUMENT;
    return guarded(c, [&]() {
        const std::string name(generator);
        const std::vector<Polygons> demand = name == "orca" ? orca_demand(c->object) :
                                                              NativeSupport::overhang_demand(c->object, *areas_generator(name));
        to_layer_rings(demand, c->object.layer_count(), out);
        return int32_t(MP_OK);
    });
}

int32_t host_buildplate_shadow(mp_compose *c, mp_layer_rings *out)
{
    if (out == nullptr)
        return MP_ERR_ARGUMENT;
    return guarded(c, [&]() {
        to_layer_rings(buildplate_covered_by_object(c->object), c->object.layer_count(), out);
        return int32_t(MP_OK);
    });
}

int32_t host_paint_channel(mp_compose *c, int32_t channel, mp_layer_rings *out)
{
    if (out == nullptr || (channel != 0 && channel != 1))
        return MP_ERR_ARGUMENT;
    return guarded(c, [&]() {
        const PrintObject &object  = c->object;
        const auto        &volumes = object.model_object()->volumes;
        if (std::none_of(volumes.begin(), volumes.end(), [](const ModelVolume *v) { return v->is_model_part() && !v->support_channel_facets.empty(); }))
            return int32_t(MP_ERR_NOT_FOUND);
        std::vector<Polygons> painted(object.layer_count());
        std::vector<Polygons> enforced(object.layer_count());
        object.project_and_append_support_channel_facets(channel == 0 ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::BLOCKER, painted);
        object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforced);
        for (size_t i = 0; i < object.layer_count(); ++i)
            // A channel means something only where the surface is still painted as a support enforcer, so
            // stale channel painting (old or hand-edited files) does nothing.
            painted[i] = i < enforced.size() ? intersection(painted[i], enforced[i]) : Polygons{};
        to_layer_rings(painted, object.layer_count(), out);
        return int32_t(MP_OK);
    });
}

void host_free_layers(mp_compose *, mp_layer_rings *layers)
{
    if (layers == nullptr || layers->layers == nullptr)
        return;
    for (uint32_t i = 0; i < layers->n; ++i) {
        mp_rings &list = layers->layers[i];
        for (uint32_t r = 0; list.rings != nullptr && r < list.n; ++r)
            std::free(const_cast<mp_point *>(list.rings[r].pts));
        std::free(const_cast<mp_ring *>(list.rings));
    }
    std::free(layers->layers);
    *layers = {nullptr, 0};
}

int32_t host_run_normal(mp_compose *c, const char *generator, const mp_rings *mask, mp_handle *out, uint32_t *support_layers)
{
    if (generator == nullptr || mask == nullptr || out == nullptr)
        return MP_ERR_ARGUMENT;
    return guarded(c, [&]() {
        PrintObject                &object = c->object;
        const std::string           name(generator);
        const std::vector<Polygons> layer_mask = from_layer_rings(mask, object.layer_count());
        object.clear_support_layers();
        if (name == "orca") {
            SupportMaterialOverrides overrides;
            overrides.contact_mask        = &layer_mask;
            overrides.ignore_support_type = true;
            PrintObjectSupportMaterial(&object, object.slicing_parameters(), overrides).generate(object);
        } else {
            NativeSupport::generate(object, *areas_generator(name), &layer_mask);
        }
        SupportLayerPtrs layers = object.take_support_layers();
        for (SupportLayer *layer : layers)
            layer->support_type = stInnerNormal;
        *out = add_set(c, std::move(layers), support_layers);
        return int32_t(MP_OK);
    });
}

int32_t host_run_tree(mp_compose *c, const char *style, const mp_rings *mask, int32_t avoid_set, mp_handle avoid, mp_handle *out,
                      uint32_t *support_layers)
{
    if (style == nullptr || mask == nullptr || out == nullptr)
        return MP_ERR_ARGUMENT;
    return guarded(c, [&]() {
        PrintObject                &object     = c->object;
        const std::vector<Polygons> layer_mask = from_layer_rings(mask, object.layer_count());
        std::vector<Polygons>       obstacles;
        if (avoid_set != 0) {
            auto it = avoid == 0 ? c->sets.end() : c->sets.find(avoid);
            if (avoid != 0 && it == c->sets.end())
                throw Slic3r::SlicingError("support.compose: unknown support set to avoid");
            obstacles = tree_obstacles(object, it == c->sets.end() ? SupportLayerPtrs{} : it->second);
        }
        TreeSupportOverrides overrides;
        overrides.demand_mask     = &layer_mask;
        overrides.style           = tree_style(style);
        overrides.extra_obstacles = avoid_set != 0 ? &obstacles : nullptr;
        object.clear_support_layers();
        TreeSupport tree(object, object.slicing_parameters(), overrides);
        tree.throw_on_cancel = [&object]() { cancel_check(object); };
        tree.generate();
        SupportLayerPtrs layers = object.take_support_layers();
        for (SupportLayer *layer : layers)
            layer->support_type = stInnerTree;
        *out = add_set(c, std::move(layers), support_layers);
        return int32_t(MP_OK);
    });
}

void host_release(mp_compose *c, mp_handle set)
{
    if (c == nullptr)
        return;
    auto it = c->sets.find(set);
    if (it != c->sets.end()) {
        free_support_layers(it->second);
        c->sets.erase(it);
    }
}

int32_t host_merge(mp_compose *c, mp_handle first, mp_handle second)
{
    return guarded(c, [&]() {
        if (c->merged)
            throw Slic3r::SlicingError("support.compose: the support sets were merged twice");
        if (first != 0 && first == second)
            throw Slic3r::SlicingError("support.compose: a support set cannot be merged with itself");
        SupportLayerPtrs normal = take_set(c, first);
        SupportLayerPtrs tree;
        try {
            tree = take_set(c, second);
        } catch (...) {
            free_support_layers(normal);
            throw;
        }
        c->object.clear_support_layers();
        c->merged = true;
        merge_layers(c->object, std::move(normal), std::move(tree));
        return int32_t(MP_OK);
    });
}

void host_discard_tree_preview(mp_compose *c)
{
    if (c != nullptr)
        c->object.clear_tree_support_preview_cache();
}

void host_status(mp_compose *c, int32_t percent, const char *text)
{
    if (c == nullptr || text == nullptr)
        return;
    try {
        c->object.print()->set_status(int(percent), Slic3r::ModHost::mod_text(c->mod_id, text));
    } catch (...) {
        if (!c->error)
            c->error = std::current_exception();
    }
}

void host_warn(mp_compose *c, int32_t critical, const char *text)
{
    if (c == nullptr || text == nullptr)
        return;
    c->object.add_module_warning(critical != 0 ? PrintStateBase::WarningLevel::CRITICAL : PrintStateBase::WarningLevel::NON_CRITICAL,
                                 Slic3r::ModHost::mod_text(c->mod_id, text));
}

} // namespace

const NativeEntry *find(const std::string &name)
{
    std::shared_ptr<const NativeModSet> mods  = active_native_mods();
    const NativeEntry                  *entry = mods ? mods->find_slot(MP_SUPPORT_COMPOSE_SLOT, name, MP_SUPPORT_COMPOSE_VERSION) : nullptr;
    return entry != nullptr && entry->table_size >= sizeof(mp_support_compose_v1) ? entry : nullptr;
}

void generate(PrintObject &object, const NativeEntry &entry)
{
    if (object.layer_count() == 0)
        return;
    const auto *table = static_cast<const mp_support_compose_v1 *>(entry.table);
    object.note_native_mod(entry.mod->id());

    mp_compose_host_v1 host{};
    host.size                 = sizeof(host);
    host.layer_count          = uint32_t(object.layer_count());
    host.has_raft             = object.slicing_parameters().has_raft() ? 1 : 0;
    host.has_generator        = host_has_generator;
    host.demand               = host_demand;
    host.buildplate_shadow    = host_buildplate_shadow;
    host.paint_channel        = host_paint_channel;
    host.free_layers          = host_free_layers;
    host.run_normal           = host_run_normal;
    host.run_tree             = host_run_tree;
    host.release              = host_release;
    host.merge                = host_merge;
    host.discard_tree_preview = host_discard_tree_preview;
    host.status               = host_status;
    host.warn                 = host_warn;

    mp_ctx ctx;
    ctx.mod           = entry.mod;
    ctx.print_config  = &object.print()->full_print_config();
    ctx.object_config = &object.config();
    ctx.cancelled     = [&object]() { return object.print()->canceled(); };

    mp_compose  c(object);
    c.mod_id = entry.mod->id();
    mp_status   st{MP_OK, nullptr};
    std::string message, status_message;
    const int32_t rc = entry.mod->guarded_call([&]() {
        st = table->compose(entry.user, &ctx, &host, &c);
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &message);

    // Whatever the outcome, no set outlives the call; on failure the object keeps no support layers.
    auto drop_all = [&c, &object]() {
        for (auto &[handle, layers] : c.sets)
            free_support_layers(layers);
        c.sets.clear();
        object.clear_support_layers();
        object.clear_tree_support_preview_cache();
    };
    if (c.error) {
        drop_all();
        std::rethrow_exception(c.error);
    }
    if (rc == MP_ERR_CANCELLED || (rc != MP_OK && object.print()->canceled())) {
        drop_all();
        throw CanceledException();
    }
    if (rc != MP_OK) {
        drop_all();
        throw Slic3r::SlicingError(entry.mod->id() + ": " +
                                   (!message.empty()        ? message :
                                    !status_message.empty() ? mod_text(entry.mod->id(), status_message) :
                                                              "support generation failed (code " + std::to_string(rc) + ")"));
    }
    for (const auto &[level, text] : ctx.warnings) {
        if (level >= MP_LOG_ERROR) {
            drop_all();
            throw Slic3r::SlicingError(entry.mod->id() + ": " + mod_text(entry.mod->id(), text));
        }
        if (level == MP_LOG_WARNING)
            object.add_module_warning(PrintStateBase::WarningLevel::NON_CRITICAL, mod_text(entry.mod->id(), text));
    }
    if (!c.merged) {
        drop_all();
        throw Slic3r::SlicingError(entry.mod->id() + ": the support composer did not merge its support sets");
    }
    // Sets the composer neither merged nor released.
    for (auto &[handle, layers] : c.sets)
        free_support_layers(layers);
    c.sets.clear();
}

} // namespace NativeCompose
} // namespace ModHost
} // namespace Slic3r
