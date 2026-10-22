// Mixed support: one object supported by a normal generator (Orca's or Cura's) where support can stand on the
// bed, and by a tree generator where it cannot. Ported from Magpie line 1 (MixedSupportPlan and
// PrintObject::_generate_support_material) and then from the core's built-in module onto the support.compose
// slot: this mod splits the normal generator's own support demand into two masks (plan.cpp) and tells the core
// which generator runs on which mask; the core runs them, keeps the tree clear of the normal support's plastic
// and merges the two by Z, the normal support owning any overlap. Built with the SDK only.

#include "mixed_support_plan.h"
#include "plan.hpp"

#include <magpie_mod.hpp>
#include <magpie_slot_support_compose.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define MIXED_MOD_ID "mixed_support"

MAGPIE_MOD_DEFINE(MIXED_MOD_ID, "2.1.0")

namespace mixed {

// The mod's parameters.
struct Settings
{
    std::string normal_generator   = "orca";    // "orca" (Orca's normal support) or "cura"
    std::string tree_style         = "organic"; // "organic", "tree_slim", "tree_strong" or "tree_hybrid"
    double      coverage_threshold = 100.;      // percent of an island reachable from the bed
    bool        selective_merge    = false;
};

Settings read_settings(mp_ctx *ctx)
{
    Settings s;
    if (auto v = magpie::param_str(ctx, "normal_generator"))
        s.normal_generator = *v;
    if (auto v = magpie::param_str(ctx, "tree_style"))
        s.tree_style = *v;
    if (auto v = magpie::param_f64(ctx, "coverage_threshold"))
        s.coverage_threshold = *v;
    if (auto v = magpie::param_i64(ctx, "selective_merge"))
        s.selective_merge = *v != 0;
    return s;
}

// Slicing was cancelled: the core's own exception is waiting on its side.
struct Cancelled {};
// A host call failed; the core keeps its reason and reports it.
struct HostFailed {};

void check(int32_t rc)
{
    if (rc == MP_ERR_CANCELLED)
        throw Cancelled{};
    if (rc != MP_OK)
        throw HostFailed{};
}

std::vector<Polygons> layers_of(const mp_compose_host_v1 &h, mp_compose *c, mp_layer_rings &list)
{
    std::vector<Polygons> out(list.n);
    for (uint32_t i = 0; i < list.n; ++i)
        out[i] = to_polygons(list.layers[i]);
    h.free_layers(c, &list);
    return out;
}

// A borrowed C view of a per-layer list, layer_count long (missing layers empty).
class LayersView
{
public:
    LayersView(const std::vector<Polygons> &layers, uint32_t layer_count)
    {
        m_views.reserve(layer_count);
        for (uint32_t i = 0; i < layer_count; ++i)
            m_views.emplace_back(i < layers.size() ? layers[i] : m_empty);
        for (const RingsView &v : m_views)
            m_lists.push_back(*v.get());
    }
    const mp_rings *get() const { return m_lists.data(); }

private:
    const Polygons         m_empty;
    std::vector<RingsView> m_views;
    std::vector<mp_rings>  m_lists;
};

void compose(mp_ctx *ctx, const mp_compose_host_v1 &h, mp_compose *c)
{
    double belt = 0.;
    if (magpie::host()->config_f64(ctx, "belt_printer", -1, &belt) == MP_OK && belt != 0.)
        throw std::runtime_error("Mixed support does not support belt printers yet.");

    const Settings s = read_settings(ctx);
    // "orca" (or empty) is Orca's normal support; any other value names a support.areas implementation a loaded mod
    // registers (the parameter's dropdown lists them, choices_from), so a generator installed later can be picked.
    const std::string normal_name = s.normal_generator.empty() ? std::string("orca") : s.normal_generator;
    if (normal_name != "orca" && !h.has_generator(c, normal_name.c_str()))
        throw std::runtime_error("Mixed support's normal support generator is not installed.");
    const uint32_t layer_count = h.layer_count;
    const char    *normal_generator = normal_name.c_str();
    auto cancel_check = [ctx]() {
        if (magpie::host()->cancelled(ctx))
            throw Cancelled{};
    };

    h.status(c, 51, "Mixed support: analyzing overhangs");
    mp_layer_rings list{};
    check(h.demand(c, normal_generator, &list));
    const std::vector<Polygons> demand = layers_of(h, c, list);
    cancel_check();
    h.status(c, 52, "Mixed support: classifying normal and tree regions");
    std::vector<Polygons> painted_normal, painted_tree;
    const int32_t         painted_rc = h.paint_channel(c, 0, &list);
    const bool            painted    = painted_rc == MP_OK;
    if (painted) {
        painted_normal = layers_of(h, c, list);
        check(h.paint_channel(c, 1, &list));
        painted_tree = layers_of(h, c, list);
    } else if (painted_rc != MP_ERR_NOT_FOUND) {
        check(painted_rc);
    }
    check(h.buildplate_shadow(c, &list));
    const std::vector<Polygons> shadow = layers_of(h, c, list);
    const Plan p = plan(demand, shadow, s.coverage_threshold, s.selective_merge, painted ? &painted_normal : nullptr,
                        painted ? &painted_tree : nullptr);
    if (any_polygons(p.normal_paint_fallback))
        h.warn(c, 0, "Some areas painted for normal support are not reachable from the build plate and were assigned to tree support.");
    cancel_check();

    mp_handle normal = 0, tree = 0;
    uint32_t  normal_layers = 0, tree_layers = 0;
    if (p.has_normal() || h.has_raft) {
        h.status(c, 53, "Mixed support: generating normal support");
        check(h.run_normal(c, normal_generator, LayersView(p.normal_mask, layer_count).get(), &normal, &normal_layers));
    }
    cancel_check();

    if (p.has_tree()) {
        h.status(c, 56, "Mixed support: generating tree support");
        // Organic trees can lose every branch when sparse support lines become hard obstacles; their overlap with
        // normal support is settled in the merge instead.
        const int32_t avoid = s.tree_style == "organic" || (s.tree_style != "tree_slim" && s.tree_style != "tree_strong" &&
                                                            s.tree_style != "tree_hybrid") ? 0 : 1;
        check(h.run_tree(c, s.tree_style.c_str(), LayersView(p.tree_mask, layer_count).get(), avoid, normal, &tree, &tree_layers));
        if (tree_layers == 0) {
            // The tree generator found no printable branch for its demand. Give that demand to the normal
            // generator too, so it is supported wherever normal support can reach.
            h.release(c, tree);
            tree = 0;
            h.discard_tree_preview(c);
            std::vector<Polygons> fallback = p.normal_mask;
            fallback.resize(std::max(fallback.size(), p.tree_mask.size()));
            for (size_t i = 0; i < p.tree_mask.size(); ++i)
                if (!p.tree_mask[i].empty())
                    fallback[i] = union_(fallback[i], p.tree_mask[i]);
            h.release(c, normal);
            normal = 0;
            check(h.run_normal(c, normal_generator, LayersView(fallback, layer_count).get(), &normal, &normal_layers));
            if (normal_layers == 0)
                throw std::runtime_error("Mixed support could not generate a printable route for the assigned support demand.");
            // PERMANENT DIAGNOSTIC: layers from the fallback may hold only the original normal demand; they do
            // not prove the tree's demand got printable support.
            h.warn(c, 1, "Tree support could not route the assigned Mixed support demand. Normal support was "
                         "retried, but some areas may remain unsupported. Check the preview before printing.");
        }
    }
    cancel_check();

    h.status(c, 69, "Mixed support: merging support channels");
    check(h.merge(c, normal, tree));
}

mp_status compose_entry(void *, mp_ctx *ctx, const mp_compose_host_v1 *host, mp_compose *c)
{
    static thread_local std::string message;
    if (host == nullptr || host->size < sizeof(mp_compose_host_v1))
        return {MP_ERR_ARGUMENT, "the core's support.compose host table is too old"};
    try {
        compose(ctx, *host, c);
        return {MP_OK, nullptr};
    } catch (const Cancelled &) {
        return {MP_ERR_CANCELLED, nullptr};
    } catch (const HostFailed &) {
        return {MP_ERR_FAILED, nullptr};
    } catch (const std::exception &e) {
        message = e.what();
    } catch (...) {
        message = "unknown exception";
    }
    return {MP_ERR_FAILED, message.c_str()};
}

const mp_support_compose_v1 compose_table = {sizeof(mp_support_compose_v1), compose_entry};

// ---- service mixed_support.plan ----

std::vector<Polygons> layers_in(const mp_rings *lists, uint32_t n)
{
    std::vector<Polygons> out(n);
    for (uint32_t i = 0; lists != nullptr && i < n; ++i)
        out[i] = to_polygons(lists[i]);
    return out;
}

struct PlanData
{
    std::vector<std::vector<mp_ring>> rings;
    std::vector<mp_rings>             normal, tree, fallback;
    std::vector<int32_t>              decisions;
    Plan                              plan;
};

void view_layers(PlanData &d, const std::vector<Polygons> &layers, std::vector<mp_rings> &lists)
{
    for (const Polygons &layer : layers) {
        std::vector<mp_ring> r;
        for (const Polygon &poly : layer)
            r.push_back({reinterpret_cast<const mp_point *>(poly.points.data()), uint32_t(poly.points.size())});
        d.rings.push_back(std::move(r));
        lists.push_back({d.rings.back().data(), uint32_t(d.rings.back().size())});
    }
}

mp_status plan_entry(const mixed_plan_input_v1 *in, mixed_plan_output_v1 *out)
{
    return magpie::guarded([&] {
        if (in == nullptr || out == nullptr || in->demand == nullptr || in->buildplate_shadow == nullptr)
            throw std::invalid_argument("mixed_support.plan: missing input");
        const std::vector<Polygons> demand = layers_in(in->demand, in->layer_count);
        const std::vector<Polygons> shadow = layers_in(in->buildplate_shadow, in->layer_count);
        const std::vector<Polygons> pn     = layers_in(in->painted_normal, in->layer_count);
        const std::vector<Polygons> pt     = layers_in(in->painted_tree, in->layer_count);
        const bool                  paint  = in->painted_normal != nullptr || in->painted_tree != nullptr;
        auto d  = std::make_unique<PlanData>();
        d->plan = plan(demand, shadow, in->coverage_threshold_percent, in->selective_merge != 0, paint ? &pn : nullptr, paint ? &pt : nullptr);
        d->rings.reserve(3 * size_t(in->layer_count));
        view_layers(*d, d->plan.normal_mask, d->normal);
        view_layers(*d, d->plan.tree_mask, d->tree);
        view_layers(*d, d->plan.normal_paint_fallback, d->fallback);
        for (const Channel ch : d->plan.decisions)
            d->decisions.push_back(ch == Channel::Normal ? MIXED_CHANNEL_NORMAL : ch == Channel::Tree ? MIXED_CHANNEL_TREE : MIXED_CHANNEL_SPLIT);
        out->layer_count           = uint32_t(d->normal.size());
        out->normal_mask           = d->normal.data();
        out->tree_mask             = d->tree.data();
        out->normal_paint_fallback = d->fallback.data();
        out->decisions             = d->decisions.data();
        out->decision_count        = uint32_t(d->decisions.size());
        out->mod_data              = d.release();
    });
}

void plan_release(mixed_plan_output_v1 *out)
{
    if (out == nullptr)
        return;
    delete static_cast<PlanData *>(out->mod_data);
    *out = mixed_plan_output_v1{out->size};
}

const mixed_support_plan_v1 plan_table = {sizeof(mixed_support_plan_v1), plan_entry, plan_release};

} // namespace mixed

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    uint32_t size = 0;
    mixed::geometry() = static_cast<const mp_geometry_v1 *>(registrar.import_service(&registrar, MP_GEOMETRY_SERVICE, MP_GEOMETRY_VERSION, &size, nullptr));
    // boolean_ex is the last entry the plan needs.
    if (mixed::geometry() == nullptr || size < offsetof(mp_geometry_v1, boolean_ex) + sizeof(void *)) {
        magpie::log(MP_LOG_ERROR, MIXED_MOD_ID, "the core's geometry service is too old for Mixed support");
        return MP_ERR_NOT_FOUND;
    }
    if (int rc = registrar.register_slot(&registrar, MP_SUPPORT_COMPOSE_SLOT, MP_SUPPORT_COMPOSE_VERSION, "mixed", &mixed::compose_table,
                                         sizeof(mixed::compose_table), nullptr);
        rc != MP_OK)
        return rc;
    return registrar.export_service(&registrar, MIXED_SUPPORT_PLAN_SERVICE, MIXED_SUPPORT_PLAN_VERSION, &mixed::plan_table,
                                    sizeof(mixed::plan_table), nullptr);
}

void magpie::mod_shutdown() {}
