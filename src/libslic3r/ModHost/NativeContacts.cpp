#include "NativeContacts.hpp"

#include "NativeMods.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/SupportMaterial.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "../../../mods/sdk/include/magpie_slot_support_contacts.h"

#include <algorithm>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace Slic3r {
namespace ModHost {
namespace NativeContacts {

namespace {

static_assert(sizeof(Vec3f) == 3 * sizeof(float) && std::is_same_v<Vec3f::Scalar, float>, "mesh vertices are handed over as float triples");
static_assert(sizeof(stl_triangle_vertex_indices) == 3 * sizeof(int32_t) && sizeof(stl_triangle_vertex_indices::Scalar) == sizeof(int32_t),
              "mesh triangles are handed over as int32 triples");

const mp_point *view(const Polygon &p) { return reinterpret_cast<const mp_point *>(p.points.data()); }

// The object as the slot reads it, viewing the core's own polygons where it can.
struct Input
{
    std::vector<Polygons>             enforcers, blockers;
    std::vector<double>               slice_z, layer_height;
    TriangleMesh                      mesh;
    std::vector<std::vector<mp_ring>> slice_rings;
    std::vector<mp_expoly>            slice_items;
    std::vector<mp_expolys>           slices;
    std::vector<std::vector<mp_ring>> enforcer_rings, blocker_rings;
    std::vector<mp_rings>             enforcer_lists, blocker_lists;
    mp_support_contacts_input_v1      c{};
};

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

void gather(const PrintObject &object, Input &in)
{
    const size_t             layer_count    = object.layer_count();
    const SlicingParameters &slicing_params = object.slicing_parameters();

    // The object mesh in the frame the layers were sliced in, moved to its print Z (raft and lift included).
    in.mesh = object.model_object()->raw_mesh();
    in.mesh.transform(object.trafo_centered(), true);
    in.mesh.translate(0.f, 0.f, float(slicing_params.object_print_z_min));

    // Islands: every layer's lslices, contour then holes.
    size_t items = 0;
    for (size_t i = 0; i < layer_count; ++i)
        items += object.get_layer(int(i))->lslices.size();
    in.slice_rings.reserve(items);
    in.slice_items.reserve(items);
    in.slices.resize(layer_count);
    for (size_t i = 0; i < layer_count; ++i) {
        const Layer &layer = *object.get_layer(int(i));
        const size_t first = in.slice_items.size();
        for (const ExPolygon &e : layer.lslices) {
            std::vector<mp_ring> rings;
            rings.push_back({view(e.contour), uint32_t(e.contour.points.size())});
            for (const Polygon &h : e.holes)
                rings.push_back({view(h), uint32_t(h.points.size())});
            in.slice_rings.push_back(std::move(rings));
            in.slice_items.push_back({in.slice_rings.back().data(), uint32_t(in.slice_rings.back().size())});
        }
        in.slices[i] = {in.slice_items.data() + first, uint32_t(in.slice_items.size() - first)};
        in.slice_z.push_back(layer.slice_z);
        in.layer_height.push_back(layer.height);
    }

    // Enforcers and blockers as Orca's normal support reads them: modifier volumes, then painting.
    in.enforcers = object.slice_support_enforcers();
    in.blockers  = object.slice_support_blockers();
    in.enforcers.resize(layer_count);
    in.blockers.resize(layer_count);
    object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, in.enforcers);
    object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, in.blockers);
    rings_of(in.enforcers, in.enforcer_rings, in.enforcer_lists);
    rings_of(in.blockers, in.blocker_rings, in.blocker_lists);

    const Flow support_flow = support_material_flow(&object);

    in.c.size                    = sizeof(mp_support_contacts_input_v1);
    in.c.layer_count             = uint32_t(layer_count);
    in.c.slices                  = in.slices.data();
    in.c.slice_z                 = in.slice_z.data();
    in.c.layer_height            = in.layer_height.data();
    in.c.enforcers               = in.enforcer_lists.data();
    in.c.blockers                = in.blocker_lists.data();
    in.c.mesh.vertices           = in.mesh.its.vertices.empty() ? nullptr : in.mesh.its.vertices.front().data();
    in.c.mesh.vertex_count       = uint32_t(in.mesh.its.vertices.size());
    in.c.mesh.indices            = in.mesh.its.indices.empty() ? nullptr : in.mesh.its.indices.front().data();
    in.c.mesh.triangle_count     = uint32_t(in.mesh.its.indices.size());
    in.c.print_z_min             = slicing_params.object_print_z_min;
    in.c.raft_interface_top_z    = slicing_params.raft_interface_top_z;
    in.c.support_nozzle_diameter = double(support_flow.nozzle_diameter());
    in.c.support_line_width      = double(support_flow.width());
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

ExPolygons expolygons_of(const mp_expolys &in)
{
    ExPolygons out;
    out.reserve(in.n);
    for (uint32_t i = 0; i < in.n; ++i) {
        ExPolygon e;
        for (uint32_t r = 0; r < in.items[i].n; ++r) {
            const mp_ring &ring = in.items[i].rings[r];
            Polygon        p;
            p.points.reserve(ring.n);
            for (uint32_t k = 0; k < ring.n; ++k)
                p.points.emplace_back(ring.pts[k].x, ring.pts[k].y);
            if (r == 0)
                e.contour = std::move(p);
            else
                e.holes.push_back(std::move(p));
        }
        out.push_back(std::move(e));
    }
    return out;
}

// Runs one call into the mod; a failure or a crash stops the slice naming the mod.
// MAGPIE: a mod's message, translated by its catalog. A message may carry values after its text, each after a 0x1F
// (unit separator) byte, which fill the translated text's %1%, %2%, ... placeholders.
std::string message_text(const std::string &mod_id, const std::string &message)
{
    const size_t split = message.find('\x1f');
    if (split == std::string::npos)
        return mod_text(mod_id, message);
    boost::format text(mod_text(mod_id, message.substr(0, split)));
    for (size_t begin = split + 1; begin <= message.size();) {
        const size_t end = std::min(message.find('\x1f', begin), message.size());
        text % message.substr(begin, end - begin);
        begin = end + 1;
    }
    return text.str();
}

void call(const PrintObject &object, const NativeEntry &entry, mp_ctx &ctx, const std::function<mp_status()> &fn)
{
    std::string   status_message, guard_message;
    const int32_t rc = entry.mod->guarded_call([&]() {
        const mp_status st = fn();
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &guard_message);
    if (rc == MP_ERR_CANCELLED || (rc != MP_OK && object.print()->canceled()))
        throw CanceledException();
    if (rc != MP_OK)
        throw SlicingError(entry.mod->id() + ": " + (!status_message.empty() ? message_text(entry.mod->id(), status_message) :
                                                     !guard_message.empty()  ? guard_message :
                                                                               "support generation failed (code " + std::to_string(rc) + ")"));
    std::lock_guard<std::mutex> lock(ctx.warnings_mutex);
    for (const auto &[level, text] : ctx.warnings) {
        if (level >= MP_LOG_ERROR)
            throw SlicingError(entry.mod->id() + ": " + message_text(entry.mod->id(), text));
        if (level == MP_LOG_WARNING)
            const_cast<PrintObject &>(object).add_module_warning(PrintStateBase::WarningLevel::NON_CRITICAL, message_text(entry.mod->id(), text));
    }
    ctx.warnings.clear();
}

} // namespace

const NativeEntry *find(const std::string &name)
{
    std::shared_ptr<const NativeModSet> mods  = active_native_mods();
    const NativeEntry                  *entry = mods ? mods->find_slot(MP_SUPPORT_CONTACTS_SLOT, name, MP_SUPPORT_CONTACTS_VERSION) : nullptr;
    return entry != nullptr && entry->table_size >= sizeof(mp_support_contacts_v1) ? entry : nullptr;
}

void generate(PrintObject &object, const NativeEntry &entry)
{
    if (object.layers().empty())
        return;
    const auto &table = *static_cast<const mp_support_contacts_v1 *>(entry.table);
    object.note_native_mod(entry.mod->id());
    Input in;
    gather(object, in);

    mp_ctx ctx;
    ctx.mod           = entry.mod;
    ctx.print_config  = &object.print()->full_print_config();
    ctx.object_config = &object.config();
    ctx.cancelled     = [&object]() { return object.print()->canceled(); };

    mp_support_contacts_output_v1 out{};
    out.size = sizeof(mp_support_contacts_output_v1);
    // Releases the mod's output however this leaves.
    std::unique_ptr<mp_support_contacts_output_v1, std::function<void(mp_support_contacts_output_v1 *)>> releaser(
        &out, [&entry, &table](mp_support_contacts_output_v1 *o) {
            entry.mod->guarded_call([&]() {
                table.release(entry.user, o);
                return int32_t(MP_OK);
            });
        });
    call(object, entry, ctx, [&]() { return table.compute(entry.user, &ctx, &in.c, &out); });

    // Per object layer; layers past the mod's output get no contacts.
    std::vector<Polygons> contacts(object.layer_count());
    for (size_t i = 0; i < contacts.size() && i < out.layer_count; ++i)
        if (out.contacts != nullptr)
            contacts[i] = polygons_of(out.contacts[i]);
    // MAGPIE: a generator that places no contacts still owes the object its raft, which the support generator builds
    // (seed 2218 of the U1 random test lost the raft and the object floated).
    const bool no_contacts = std::all_of(contacts.begin(), contacts.end(), [](const Polygons &c) { return c.empty(); });
    if (no_contacts && !object.slicing_parameters().has_raft() && object.slicing_parameters().object_z_offset <= 0.)
        return;

    SupportMaterialOverrides overrides;
    overrides.contact_demand = &contacts;
    overrides.body_at        = [&](const std::vector<coordf_t> &print_z) {
        std::vector<ExPolygons> bodies(print_z.size());
        if (print_z.empty() || no_contacts)
            return bodies;
        std::vector<mp_expolys> lists(print_z.size());
        call(object, entry, ctx, [&]() {
            const mp_status st = table.body(entry.user, &ctx, &out, uint32_t(print_z.size()), print_z.data(), lists.data());
            if (st.code == MP_OK)
                for (size_t i = 0; i < lists.size(); ++i)
                    bodies[i] = expolygons_of(lists[i]);
            return st;
        });
        return bodies;
    };
    overrides.keep_narrow_islands = out.keep_narrow_islands != 0;
    // The contacts are given: support_type does not choose this generator.
    overrides.ignore_support_type = true;
    PrintObjectSupportMaterial support(&object, object.slicing_parameters(), overrides);
    support.generate(object);
    // MAGPIE: an object raised for its support (object_z_offset) with nothing printed under it hangs in the air, and
    // the slice stopped later on "empty layers" with no word of why. Contacts alone do not decide it: seeds 2234 and
    // 2358 of the U1 random test had contacts, yet the resin tree built no pillar (100 % small pillar, 0.6 mm
    // branching pillars on a 0.8 mm nozzle), and the contact layer under the object was printed with nothing below. So the check reads the built support layers: one must stand on the bed (its
    // bottom, print_z - height, at z = 0 within EPSILON mm).
    const bool supported = std::any_of(object.support_layers().begin(), object.support_layers().end(),
                                       [](const SupportLayer *l) { return l->print_z - l->height <= EPSILON && l->has_extrusions(); });
    if (object.slicing_parameters().object_z_offset > 0. && !supported) {
        // PERMANENT DIAGNOSTIC: the lowest support layers, to tell support that was never built (no body under the
        // contacts, as in seed 6828 of the U1 random test) from support that stops short of the bed.
        size_t listed = 0;
        for (const SupportLayer *l : object.support_layers()) {
            BOOST_LOG_TRIVIAL(error) << entry.mod->id() << ": no support on the bed; support layer print_z " << l->print_z
                                     << " height " << l->height << " extrusions " << l->has_extrusions();
            if (++listed == 6)
                break;
        }
        BOOST_LOG_TRIVIAL(error) << entry.mod->id() << ": " << object.support_layers().size() << " support layers, object_z_offset "
                                 << object.slicing_parameters().object_z_offset;
        throw SlicingError(entry.mod->id() + ": " +
            Slic3r::format(_u8L("No support was generated for \"%1%\", which is raised %2% mm for its support. Lower the "
                                "object elevation, or change the support settings so that support is generated."),
                           object.model_object()->name, object.slicing_parameters().object_z_offset));
    }
}

} // namespace NativeContacts
} // namespace ModHost
} // namespace Slic3r
