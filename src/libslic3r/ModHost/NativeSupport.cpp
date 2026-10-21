#include "NativeSupport.hpp"

#include "NativeMods.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LineWidths.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/SupportLayer.hpp"
#include "libslic3r/Support/SupportParameters.hpp"

#include "../../../mods/sdk/include/magpie_support.h"

#include <algorithm>
#include <memory>

namespace Slic3r {
namespace ModHost {
namespace NativeSupport {

namespace {

// The object's layers as the slot reads them, viewing the core's own polygons where it can.
struct Input
{
    std::vector<Polygons>             enforcers, blockers, mask;
    Polygon                           printable_area;
    // Views: one mp_ring per polygon, then one list per layer.
    std::vector<std::vector<mp_ring>> slice_rings;
    std::vector<mp_expoly>            slice_items;
    std::vector<mp_expolys>           slices;
    std::vector<std::vector<mp_ring>> enforcer_rings, blocker_rings, mask_rings;
    std::vector<mp_rings>             enforcer_lists, blocker_lists, mask_lists;
    mp_support_input_v1               c{};
};

const mp_point *view(const Polygon &p) { return reinterpret_cast<const mp_point *>(p.points.data()); }

void rings_of(const std::vector<Polygons> &layers, std::vector<std::vector<mp_ring>> &rings, std::vector<mp_rings> &lists)
{
    rings.assign(layers.size(), {});
    lists.assign(layers.size(), {});
    for (size_t i = 0; i < layers.size(); ++i) {
        for (const Polygon &p : layers[i])
            rings[i].push_back({view(p), uint32_t(p.points.size())});
        lists[i] = {rings[i].data(), uint32_t(rings[i].size())};
    }
}

// How far the first layer's adhesion reaches past the object, in mm: the raft's expansion, else the outer brim,
// with the skirt's loops around that. An automatic brim is counted at the configured brim width, the width it
// is sized from before the object's own estimate.
double adhesion_margin_mm(const PrintObject &object)
{
    const PrintObjectConfig &config = object.config();
    const Print             &print  = *object.print();
    if (object.has_raft())
        return config.raft_expansion.value + std::max(0., config.raft_first_layer_expansion.value);
    double reach = 0.;
    if (object.has_brim() && config.brim_type != btInnerOnly)
        reach = config.brim_object_gap.value + config.brim_width.value;
    if (print.has_skirt() && print.config().skirt_loops.value > 0) {
        // The outermost loop's centreline sits skirt_distance - spacing / 2 + loops * spacing past the brim
        // (Print::_make_skirt), and its outer edge half a line width further. On a printer whose nozzles differ in
        // size the skirt's nozzle comes from the tool ordering, which does not exist yet (or is a previous slice's)
        // while support is generated, so the widest nozzle's skirt is counted.
        auto skirt_reach = [&print](const Flow &flow) { return (print.config().skirt_loops.value - 0.5) * flow.spacing() + flow.width() / 2.; };
        double loops = 0.;
        if (print.line_width_reference_mm() > 0.)
            for (size_t nozzle = 0; nozzle < print.config().nozzle_diameter.size(); ++nozzle)
                loops = std::max(loops, skirt_reach(skirt_flow_on_nozzle(print, nozzle)));
        else
            loops = skirt_reach(print.skirt_flow());
        reach += print.config().skirt_distance.value + loops;
    }
    return reach;
}

void gather(const PrintObject &object, const std::vector<Polygons> *mask, Input &in)
{
    const size_t layer_count = object.layer_count();
    // Enforcers and blockers as Orca's normal support reads them: modifier volumes, then painting.
    in.enforcers = object.slice_support_enforcers();
    in.blockers  = object.slice_support_blockers();
    object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, in.enforcers);
    object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, in.blockers);
    in.enforcers.resize(layer_count);
    in.blockers.resize(layer_count);
    rings_of(in.enforcers, in.enforcer_rings, in.enforcer_lists);
    rings_of(in.blockers, in.blocker_rings, in.blocker_lists);

    // Islands: every layer's lslices, contour then holes.
    size_t items = 0;
    for (size_t i = 0; i < layer_count; ++i)
        items += object.get_layer(int(i))->lslices.size();
    in.slice_rings.reserve(items);
    in.slice_items.reserve(items);
    in.slices.resize(layer_count);
    for (size_t i = 0; i < layer_count; ++i) {
        const size_t first = in.slice_items.size();
        for (const ExPolygon &e : object.get_layer(int(i))->lslices) {
            std::vector<mp_ring> rings;
            rings.push_back({view(e.contour), uint32_t(e.contour.points.size())});
            for (const Polygon &h : e.holes)
                rings.push_back({view(h), uint32_t(h.points.size())});
            in.slice_rings.push_back(std::move(rings));
            in.slice_items.push_back({in.slice_rings.back().data(), uint32_t(in.slice_rings.back().size())});
        }
        in.slices[i] = {in.slice_items.data() + first, uint32_t(in.slice_items.size() - first)};
    }

    if (mask != nullptr) {
        // One list per object layer; layers past the mask's end allow nothing.
        in.mask.assign(layer_count, Polygons{});
        for (size_t i = 0; i < layer_count && i < mask->size(); ++i)
            in.mask[i] = (*mask)[i];
        rings_of(in.mask, in.mask_rings, in.mask_lists);
    }

    // The printable area is in bed coordinates; the object's layers are in object coordinates, which the
    // first instance's shift moves onto the bed (as PrintObject moves the unprintable areas the other way).
    for (const Vec2d &p : object.print()->config().printable_area.values)
        in.printable_area.points.emplace_back(scale_(p.x()), scale_(p.y()));
    in.printable_area.translate(-object.instances().front().shift_without_plate_offset());

    in.c.size                 = sizeof(mp_support_input_v1);
    in.c.layer_count          = uint32_t(layer_count);
    in.c.slices               = in.slices.data();
    in.c.enforcers            = in.enforcer_lists.data();
    in.c.blockers             = in.blocker_lists.data();
    in.c.mask                 = mask != nullptr ? in.mask_lists.data() : nullptr;
    in.c.printable_area       = {view(in.printable_area), uint32_t(in.printable_area.points.size())};
    in.c.support_line_width   = int64_t(support_material_flow(&object).scaled_width());
    in.c.interface_line_width = int64_t(support_material_interface_flow(&object).scaled_width());
    const Flow support_flow   = support_material_flow(&object);
    in.c.support_nozzle_diameter = int64_t(scale_(support_flow.nozzle_diameter()));
    in.c.object_nozzle_diameter  = object.num_printing_regions() > 0 ?
        int64_t(scale_(object.printing_region(0).flow(object, frPerimeter, object.config().layer_height.value).nozzle_diameter())) :
        in.c.support_nozzle_diameter;
    in.c.adhesion_margin = int64_t(scale_(adhesion_margin_mm(object)));
}

Polygons polygons_of(const mp_rings &rings)
{
    Polygons out;
    out.reserve(rings.n);
    for (uint32_t r = 0; r < rings.n; ++r) {
        Polygon p;
        p.points.reserve(rings.rings[r].n);
        for (uint32_t k = 0; k < rings.rings[r].n; ++k)
            p.points.emplace_back(rings.rings[r].pts[k].x, rings.rings[r].pts[k].y);
        out.push_back(std::move(p));
    }
    return out;
}

// The areas a call returned, copied out of the mod's memory.
struct Areas
{
    std::vector<Polygons> body, roof, bottom;
    // Parts of the body and roof printed fractional_drop lower (mp_support_output_v1), scaled.
    std::vector<Polygons> fractional_body, fractional_roof;
    coord_t               fractional_drop   = 0;
    bool                  first_layer_dense = false;
};

using Function = mp_status (*)(void *, mp_ctx *, const mp_support_input_v1 *, mp_support_output_v1 *);

Areas call(const PrintObject &object, const NativeEntry &entry, Function fn, const std::vector<Polygons> *mask)
{
    const auto *table = static_cast<const mp_support_areas_v1 *>(entry.table);
    object.note_native_mod(entry.mod->id());
    Input       in;
    gather(object, mask, in);

    mp_ctx ctx;
    ctx.mod           = entry.mod;
    ctx.print_config  = &object.print()->full_print_config();
    ctx.object_config = &object.config();
    ctx.cancelled     = [&object]() { return object.print()->canceled(); };

    mp_support_output_v1 out{};
    out.size = sizeof(mp_support_output_v1);
    Areas       areas;
    mp_status   st{MP_OK, nullptr};
    std::string message, status_message;
    const int32_t rc = entry.mod->guarded_call([&]() {
        st = fn(entry.user, &ctx, &in.c, &out);
        if (st.code != MP_OK) {
            status_message = st.message ? st.message : "";
            table->release(entry.user, &out);
            return st.code;
        }
        const size_t n = std::min<size_t>(out.layer_count, in.c.layer_count);
        areas.body.resize(in.c.layer_count);
        areas.roof.resize(in.c.layer_count);
        areas.bottom.resize(in.c.layer_count);
        for (size_t i = 0; i < n; ++i) {
            if (out.body)
                areas.body[i] = polygons_of(out.body[i]);
            if (out.roof)
                areas.roof[i] = polygons_of(out.roof[i]);
            if (out.bottom)
                areas.bottom[i] = polygons_of(out.bottom[i]);
        }
        if (out.fractional_drop > 0 && (out.fractional_body || out.fractional_roof)) {
            areas.fractional_drop = coord_t(out.fractional_drop);
            areas.fractional_body.resize(in.c.layer_count);
            areas.fractional_roof.resize(in.c.layer_count);
            for (size_t i = 0; i < n; ++i) {
                if (out.fractional_body)
                    areas.fractional_body[i] = polygons_of(out.fractional_body[i]);
                if (out.fractional_roof)
                    areas.fractional_roof[i] = polygons_of(out.fractional_roof[i]);
            }
        }
        areas.first_layer_dense = out.first_layer_dense != 0;
        table->release(entry.user, &out);
        return int32_t(MP_OK);
    }, &message);
    if (rc == MP_ERR_CANCELLED || (rc != MP_OK && object.print()->canceled()))
        throw CanceledException();
    if (rc != MP_OK)
        throw Slic3r::SlicingError(entry.mod->id() + ": " + (!message.empty() ? message : !status_message.empty() ? mod_text(entry.mod->id(), status_message) :
                                                             "support generation failed (code " + std::to_string(rc) + ")"));
    for (const auto &[level, text] : ctx.warnings) {
        if (level >= MP_LOG_ERROR)
            throw Slic3r::SlicingError(entry.mod->id() + ": " + mod_text(entry.mod->id(), text));
        if (level == MP_LOG_WARNING)
            const_cast<PrintObject &>(object).add_module_warning(PrintStateBase::WarningLevel::NON_CRITICAL, mod_text(entry.mod->id(), text));
    }
    return areas;
}

} // namespace

const NativeEntry *find(const std::string &name)
{
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    const NativeEntry *entry = mods ? mods->find_slot(MP_SUPPORT_AREAS_SLOT, name, MP_SUPPORT_AREAS_VERSION) : nullptr;
    return entry != nullptr && entry->table_size >= sizeof(mp_support_areas_v1) ? entry : nullptr;
}

std::vector<Polygons> overhang_demand(const PrintObject &object, const NativeEntry &entry)
{
    const auto *table = static_cast<const mp_support_areas_v1 *>(entry.table);
    return call(object, entry, table->overhang_demand, nullptr).body;
}

void generate(PrintObject &object, const NativeEntry &entry, const std::vector<Polygons> *mask)
{
    if (object.layer_count() == 0)
        return;
    const auto *table = static_cast<const mp_support_areas_v1 *>(entry.table);
    Areas       areas = call(object, entry, table->compute, mask);

    SupportParameters support_params(object);
    // A generator that prints its first support layer with the same lines as the rest (as CuraEngine
    // does by default) gets it aligned and not widened; one that asks for a dense first layer, solid.
    if (areas.first_layer_dense)
        support_params.first_layer_density = 1.f;
    else
        support_params.aligned_first_layer = true;
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayersPtr    base_layers, interface_layers, empty;
    auto make_layer = [&](size_t idx, SupporLayerType type, Polygons &&polygons) {
        const Layer           &object_layer = *object.get_layer(int(idx));
        SupportGeneratorLayer &layer        = storage.allocate(type);
        layer.print_z  = object_layer.print_z;
        layer.bottom_z = object_layer.bottom_z();
        layer.height   = object_layer.height;
        layer.polygons = std::move(polygons);
        return &layer;
    };
    // The fractional parts sit on the layer below and end fractional_drop under the layer's top, so the gap to
    // the object above is the top Z distance. Thinner than the thinnest printable layer (the slicing parameters'
    // min_layer_height, mm), they are left out and the gap is a whole layer larger by less than that.
    const double fractional_drop_mm = unscale<double>(areas.fractional_drop);
    auto make_fractional_layer = [&](size_t idx, SupporLayerType type, Polygons &&polygons) -> SupportGeneratorLayer * {
        const Layer &object_layer = *object.get_layer(int(idx));
        const double print_z      = object_layer.print_z - fractional_drop_mm;
        if (print_z - object_layer.bottom_z() < object.slicing_parameters().min_layer_height - EPSILON)
            return nullptr;
        SupportGeneratorLayer &layer = storage.allocate(type);
        layer.print_z  = print_z;
        layer.bottom_z = object_layer.bottom_z();
        layer.height   = print_z - layer.bottom_z;
        layer.polygons = std::move(polygons);
        return &layer;
    };
    for (size_t i = 0; i < areas.fractional_body.size(); ++i) {
        if (!areas.fractional_body[i].empty())
            if (SupportGeneratorLayer *layer = make_fractional_layer(i, SupporLayerType::Base, std::move(areas.fractional_body[i])))
                base_layers.push_back(layer);
        if (!areas.fractional_roof[i].empty())
            if (SupportGeneratorLayer *layer = make_fractional_layer(i, SupporLayerType::TopInterface, std::move(areas.fractional_roof[i])))
                interface_layers.push_back(layer);
    }
    for (size_t i = 0; i < areas.body.size(); ++i) {
        if (!areas.body[i].empty())
            base_layers.push_back(make_layer(i, SupporLayerType::Base, std::move(areas.body[i])));
        if (!areas.roof[i].empty())
            interface_layers.push_back(make_layer(i, SupporLayerType::TopInterface, std::move(areas.roof[i])));
        if (!areas.bottom[i].empty())
            interface_layers.push_back(make_layer(i, SupporLayerType::BottomInterface, std::move(areas.bottom[i])));
    }
    std::sort(interface_layers.begin(), interface_layers.end(), [](const SupportGeneratorLayer *a, const SupportGeneratorLayer *b) { return *a < *b; });
    std::stable_sort(base_layers.begin(), base_layers.end(), [](const SupportGeneratorLayer *a, const SupportGeneratorLayer *b) { return a->print_z < b->print_z; });

    // As in the classic generator, the raft contact layer carries the first object layer (expanded by
    // raft_expansion) and the feet of the columns; generate_raft_base builds the raft base and
    // interface below it. Without it the raft comes out empty and the object, lifted by the raft
    // height, prints in the air.
    SupportGeneratorLayersPtr top_contacts;
    const SlicingParameters  &slicing_params = object.slicing_parameters();
    if (slicing_params.has_raft()) {
        const Layer &first_layer  = *object.layers().front();
        Polygons     raft_contact = to_polygons(first_layer.lslices);
        if (object.config().raft_expansion.value > 0)
            raft_contact = expand(raft_contact, scaled<float>(object.config().raft_expansion.value));
        if (!base_layers.empty() && is_approx(base_layers.front()->print_z, first_layer.print_z))
            raft_contact = union_(raft_contact, base_layers.front()->polygons);
        if (!raft_contact.empty()) {
            SupportGeneratorLayer &contact = storage.allocate(SupporLayerType::TopContact);
            contact.print_z                = slicing_params.raft_contact_top_z;
            contact.bottom_z               = slicing_params.raft_interface_top_z;
            contact.height                 = slicing_params.contact_raft_layer_height;
            contact.idx_object_layer_above = 0;
            contact.contact_polygons       = std::make_unique<Polygons>(raft_contact);
            contact.overhang_polygons      = std::make_unique<Polygons>(raft_contact);
            contact.polygons               = std::move(raft_contact);
            top_contacts.push_back(&contact);
        }
    }
    if (base_layers.empty() && interface_layers.empty() && top_contacts.empty())
        return;

    SupportGeneratorLayersPtr raft_layers = generate_raft_base(object, support_params, slicing_params, top_contacts, interface_layers, empty,
                                                               base_layers, storage);
    if (object.print()->canceled()) throw CanceledException();
    // H/2 support refines these layers into half-height bands, so each role gets its own list.
    SupportGeneratorLayersPtr bottom_contacts, base_interface_layers;
    generate_support_layers(object, raft_layers, bottom_contacts, top_contacts, base_layers, interface_layers, base_interface_layers, &storage);
    if (object.print()->canceled()) throw CanceledException();
    generate_support_toolpaths(object.support_layers(), object.config(), support_params, object.slicing_parameters(), raft_layers, bottom_contacts,
                               top_contacts, base_layers, interface_layers, base_interface_layers);
}

} // namespace NativeSupport
} // namespace ModHost
} // namespace Slic3r
