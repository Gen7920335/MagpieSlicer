#include "CalibrationProject.hpp"

#include "ModSet.hpp"
#include "NativeMods.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "../../../mods/sdk/include/magpie_slot_calibration_project.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <memory>
#include <optional>

namespace Slic3r {
namespace ModHost {

namespace {

using json = nlohmann::json;

// The calibration.project implementation of the active native mod `mod_id`, or nullptr.
const NativeEntry *calibration_entry(const NativeModSet &mods, const std::string &mod_id)
{
    for (const NativeEntry *entry : mods.slots(MP_CALIBRATION_PROJECT_SLOT))
        if (entry->mod->id() == mod_id && entry->version >= MP_CALIBRATION_PROJECT_VERSION &&
            entry->table_size >= sizeof(mp_calibration_project_v1))
            return entry;
    return nullptr;
}

// A stored value of `param` for the text `value` (as a form or a mod writes it), or nothing when the type does not
// take it.
std::optional<json> param_json(const ModParam &param, const std::string &value)
{
    try {
        if (param.type == "bool") {
            if (value == "1" || value == "true")
                return json(true);
            if (value == "0" || value == "false")
                return json(false);
            return std::nullopt;
        }
        if (param.type == "int" || param.type == "float" || param.type == "float_or_percent") {
            if (param.type == "float_or_percent" && boost::ends_with(value, "%"))
                return json(value);
            size_t      used = 0;
            const double v   = std::stod(value, &used);
            if (used != value.size() || !std::isfinite(v))
                return std::nullopt;
            if (param.type == "int") {
                if (v != std::floor(v))
                    return std::nullopt;
                return json(int64_t(v));
            }
            return json(v);
        }
        if (param.type == "string" && param_allows(param, ExprValue(value)))
            return json(value);
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

// `values` (a stored-values JSON object) with `key` set to `value`.
std::string with_value(const std::string &values, const std::string &key, const json &value)
{
    json merged = values.empty() ? json::object() : json::parse(values, nullptr, false);
    if (!merged.is_object())
        merged = json::object();
    merged[key] = value;
    return merged.dump();
}

const ModParam &active_param(const std::string &key)
{
    std::shared_ptr<const ModSet> mods = active_mods();
    const ModParam               *param = mods ? mods->find_param(key) : nullptr;
    if (param == nullptr)
        throw ModError("the calibration sets \"" + key + "\", which no loaded mod has");
    return *param;
}

json checked_param_json(const ModParam &param, const std::string &key, const std::string &value)
{
    std::optional<json> v = param_json(param, value);
    if (!v)
        throw ModError("the calibration sets \"" + key + "\" to \"" + value + "\", which it does not take");
    return *v;
}

// `key` set to `value` in `config`.
void set_orca_option(DynamicPrintConfig &config, const std::string &key, const std::string &value)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def == nullptr)
        throw ModError("the calibration sets \"" + key + "\", which is no setting");
    std::unique_ptr<ConfigOption> option(def->create_default_option());
    if (!option->deserialize(value, false))
        throw ModError("the calibration sets \"" + key + "\" to \"" + value + "\", which it does not take");
    // One value for a list: every element of the list the config has.
    if (option->is_vector()) {
        auto *list = static_cast<ConfigOptionVectorBase *>(option.get());
        if (const ConfigOption *current = config.option(key); current != nullptr && current->is_vector() && list->size() == 1) {
            const size_t n = static_cast<const ConfigOptionVectorBase *>(current)->size();
            if (n > 1) {
                std::unique_ptr<ConfigOption> one(option->clone());
                list->resize(n, one.get());
            }
        }
    }
    config.set_key_value(key, option.release());
}

// Builds the project for build(), collects reports for both calls.
struct Builder
{
    const NativeMod                   *mod = nullptr;
    CalibrationCheck                   check;
    CalibrationProject                 project;
    // Per object: each volume's mesh as the core makes the primitive (at the origin), and where the centre of its
    // bounding box goes on the plate (mm). The meshes are never moved in single precision, so a part far from the
    // origin keeps the vertices a loaded mesh would have.
    std::vector<std::vector<TriangleMesh>>             parts;
    std::vector<std::vector<Vec3d>>                    centers;
    std::vector<std::vector<ModelVolumeType>>          types;
    std::vector<std::vector<std::pair<std::string, std::string>>> object_options;
    std::vector<t_layer_config_ranges>                 layer_ranges;
    std::string                        error; // the first refused call

    void refuse(const std::string &message)
    {
        if (error.empty())
            error = message;
    }

    int32_t add_volume(int32_t object, int32_t type, TriangleMesh &&mesh, const Vec3d &center)
    {
        if (object < 0 || size_t(object) >= parts.size()) {
            refuse("a volume of an object the calibration did not add");
            return MP_ERR_ARGUMENT;
        }
        ModelVolumeType t;
        switch (type) {
        case MP_CALIB_PART: t = ModelVolumeType::MODEL_PART; break;
        case MP_CALIB_SUPPORT_ENFORCER: t = ModelVolumeType::SUPPORT_ENFORCER; break;
        case MP_CALIB_SUPPORT_BLOCKER: t = ModelVolumeType::SUPPORT_BLOCKER; break;
        default: refuse("a volume of an unknown type"); return MP_ERR_ARGUMENT;
        }
        if (mesh.empty()) {
            refuse("an empty volume");
            return MP_ERR_ARGUMENT;
        }
        parts[size_t(object)].push_back(std::move(mesh));
        centers[size_t(object)].push_back(center);
        types[size_t(object)].push_back(t);
        return MP_OK;
    }

    static Builder &of(void *p) { return *static_cast<Builder *>(p); }

    static void report(void *p, int32_t kind, const char *text, const double *args, uint32_t n_args)
    {
        Builder    &b = of(p);
        std::string t = text != nullptr ? mod_text(b.mod->id(), text) : std::string();
        // %1%.. from the last, so %1 never eats the start of %10.
        for (uint32_t i = n_args; args != nullptr && i > 0; --i)
            boost::replace_all(t, "%" + std::to_string(i) + "%", format_decimal(args[i - 1]));
        if (kind == MP_CALIB_REPORT_ERROR)
            b.check.errors.push_back(t);
        else if (kind == MP_CALIB_REPORT_WARNING)
            b.check.warnings.push_back(t);
        else if (kind == MP_CALIB_REPORT_SUMMARY)
            b.check.summary = t;
    }
    static int32_t add_object(void *p, const char *name)
    {
        Builder &b = of(p);
        b.parts.emplace_back();
        b.centers.emplace_back();
        b.types.emplace_back();
        b.object_options.emplace_back();
        b.layer_ranges.emplace_back();
        ModelObject *object = b.project.model.add_object();
        object->name        = name != nullptr ? name : "";
        return int32_t(b.parts.size() - 1);
    }
    static int32_t add_box(void *p, int32_t object, int32_t type, double x, double y, double z, double sx, double sy, double sz)
    {
        if (!(sx > 0. && sy > 0. && sz > 0.) || !std::isfinite(x + y + z + sx + sy + sz)) {
            of(p).refuse("a box without a positive size");
            return MP_ERR_ARGUMENT;
        }
        return of(p).add_volume(object, type, TriangleMesh(its_make_cube(sx, sy, sz)), Vec3d(x + 0.5 * sx, y + 0.5 * sy, z + 0.5 * sz));
    }
    static int32_t add_cylinder(void *p, int32_t object, int32_t type, double cx, double cy, double z, double r, double h, double fa)
    {
        // A facet angle under 0.001 rad would make over 6000 corners.
        constexpr double min_facet_angle_rad = 1e-3;
        if (!(r > 0. && h > 0. && fa >= min_facet_angle_rad) || !std::isfinite(cx + cy + z + r + h + fa)) {
            of(p).refuse("a cylinder without a positive size");
            return MP_ERR_ARGUMENT;
        }
        return of(p).add_volume(object, type, TriangleMesh(its_make_cylinder(r, h, fa)), Vec3d(cx, cy, z + 0.5 * h));
    }
    static int32_t set_object_option(void *p, int32_t object, const char *key, const char *value)
    {
        Builder &b = of(p);
        if (object < 0 || size_t(object) >= b.object_options.size() || key == nullptr || value == nullptr) {
            b.refuse("a setting of an object the calibration did not add");
            return MP_ERR_ARGUMENT;
        }
        b.object_options[size_t(object)].emplace_back(key, value);
        return MP_OK;
    }
    static int32_t add_layer_range(void *p, int32_t object, double min_z, double max_z, double layer_height)
    {
        Builder &b = of(p);
        if (object < 0 || size_t(object) >= b.layer_ranges.size()) {
            b.refuse("a height range of an object the calibration did not add");
            return MP_ERR_ARGUMENT;
        }
        if (!(min_z >= 0. && max_z > min_z && layer_height > 0.) || !std::isfinite(min_z + max_z + layer_height)) {
            b.refuse("a height range without a positive height and layer height");
            return MP_ERR_ARGUMENT;
        }
        t_layer_config_ranges &ranges = b.layer_ranges[size_t(object)];
        for (const auto &[range, config] : ranges)
            if (min_z < range.second - EPSILON && range.first < max_z - EPSILON) {
                b.refuse("overlapping height ranges of an object");
                return MP_ERR_ARGUMENT;
            }
        // As ObjectList::get_default_layer_config makes a range: its layer height, the object's extruder (0).
        ModelConfig config;
        config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
        config.set_key_value("extruder", new ConfigOptionInt(0));
        ranges[{ min_z, max_z }] = std::move(config);
        return MP_OK;
    }
    static int32_t set_option(void *p, int32_t preset, const char *key, const char *value)
    {
        Builder &b = of(p);
        if (preset < MP_CALIB_PRINT || preset > MP_CALIB_PRINTER || key == nullptr || value == nullptr) {
            b.refuse("a setting of an unknown preset");
            return MP_ERR_ARGUMENT;
        }
        b.project.options.push_back({ CalibrationPreset(preset), key, value });
        return MP_OK;
    }
    static int32_t set_value(void *p, const char *name, double value)
    {
        Builder &b = of(p);
        if (name == nullptr || !std::isfinite(value)) {
            b.refuse("a calibration value that is not a number");
            return MP_ERR_ARGUMENT;
        }
        b.project.values[name] = value;
        return MP_OK;
    }
};

// Calls check() or build() of `mod_id`'s implementation into `builder`; the failure, or "".
std::string call(const std::string &mod_id, const std::string &calibration, const DynamicPrintConfig &config, const CalibrationFields &fields,
                 bool build, Builder &builder)
{
    std::shared_ptr<const NativeModSet> mods  = active_native_mods();
    const NativeEntry                  *entry = mods ? calibration_entry(*mods, mod_id) : nullptr;
    if (entry == nullptr)
        return "the mod " + mod_id + " is not loaded or has no calibration.project implementation";
    builder.mod = entry->mod;
    std::vector<mp_calib_field_v1> in;
    in.reserve(fields.size());
    for (const auto &[id, value] : fields)
        in.push_back({ id.c_str(), value.c_str() });
    mp_calib_project_v1 project{};
    project.size              = sizeof(project);
    project.p                 = &builder;
    project.report            = &Builder::report;
    project.add_object        = &Builder::add_object;
    project.add_box           = &Builder::add_box;
    project.add_cylinder      = &Builder::add_cylinder;
    project.set_object_option = &Builder::set_object_option;
    project.set_option        = &Builder::set_option;
    project.set_value         = &Builder::set_value;
    project.add_layer_range   = &Builder::add_layer_range;
    const auto &table = *static_cast<const mp_calibration_project_v1 *>(entry->table);
    mp_ctx      ctx;
    ctx.mod          = entry->mod;
    ctx.print_config = &config;
    ctx.cancelled    = []() { return false; };
    std::string   status_message, guard_message;
    const int32_t rc = entry->mod->guarded_call([&]() {
        const mp_status st = (build ? table.build : table.check)(entry->user, &ctx, calibration.c_str(), in.data(), uint32_t(in.size()), &project);
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &guard_message);
    if (rc != MP_OK)
        return mod_id + ": " + (!status_message.empty() ? mod_text(mod_id, status_message) :
                                !guard_message.empty()  ? guard_message : std::string("the calibration failed"));
    for (const auto &[level, text] : ctx.warnings)
        if (level >= MP_LOG_ERROR)
            return mod_id + ": " + mod_text(mod_id, text);
    if (!builder.error.empty())
        return mod_id + ": " + builder.error;
    return std::string();
}

} // namespace

CalibrationCheck check_calibration(const std::string &mod_id, const std::string &calibration, const DynamicPrintConfig &config,
                                   const CalibrationFields &fields)
{
    Builder builder;
    builder.check.failure = call(mod_id, calibration, config, fields, false, builder);
    return builder.check;
}

CalibrationProject build_calibration(const std::string &mod_id, const std::string &calibration, const DynamicPrintConfig &config,
                                     const CalibrationFields &fields)
{
    Builder builder;
    if (std::string failure = call(mod_id, calibration, config, fields, true, builder); !failure.empty())
        throw ModError(failure);
    if (!builder.check.errors.empty())
        throw ModError(mod_id + ": " + builder.check.errors.front());
    CalibrationProject &project = builder.project;
    project.calibration         = mod_id + "." + calibration;
    for (size_t i = 0; i < project.model.objects.size(); ++i) {
        ModelObject *object = project.model.objects[i];
        if (builder.parts[i].empty())
            throw ModError(mod_id + ": the calibration's object \"" + object->name + "\" has no volume");
        // As ObjectList::load_mesh_object and Plater's centring place a loaded mesh: add_volume() centres each part on
        // its own geometry and its offset puts it where the mod asked (in double precision); the object is then
        // centred on its parts and the instance carries the object's place.
        BoundingBoxf3 bb;
        for (size_t v = 0; v < builder.parts[i].size(); ++v) {
            const Vec3d half = 0.5 * builder.parts[i][v].bounding_box().size();
            bb.merge(builder.centers[i][v] - half);
            bb.merge(builder.centers[i][v] + half);
        }
        object->add_instance();
        for (size_t v = 0; v < builder.parts[i].size(); ++v) {
            const ModelVolumeType type   = builder.types[i][v];
            ModelVolume          *volume = type == ModelVolumeType::MODEL_PART ? object->add_volume(builder.parts[i][v]) :
                                                                                  object->add_volume(TriangleMesh(builder.parts[i][v]), type);
            volume->name = object->name;
            volume->set_offset(builder.centers[i][v]);
        }
        object->sort_volumes(true);
        object->config.set_key_value("extruder", new ConfigOptionInt(1));
        object->invalidate_bounding_box();
        object->translate(-bb.center());
        object->instances[0]->set_offset(bb.center());
        object->ensure_on_bed();
        object->instances[0]->set_assemble_transformation(object->instances[0]->get_transformation());
        for (const auto &[key, value] : builder.object_options[i])
            set_calibration_object_option(*object, key, value);
        // The object stands on the bed, so heights above the bed are the object's own.
        object->layer_config_ranges = builder.layer_ranges[i];
    }
    return std::move(project);
}

void set_calibration_option(DynamicPrintConfig &config, const std::string &key, const std::string &value)
{
    if (key.find('.') == std::string::npos) {
        set_orca_option(config, key, value);
        return;
    }
    const ModParam &param = active_param(key);
    const json      v     = checked_param_json(param, key, value);
    if (param.scope == "printer") {
        const ConfigOptionString *stored = config.option<ConfigOptionString>("printer_mod_settings");
        config.set_key_value("printer_mod_settings", new ConfigOptionString(with_value(stored ? stored->value : std::string(), key, v)));
    } else if (param.scope == "filament") {
        const ConfigOptionStrings *stored = config.option<ConfigOptionStrings>("filament_mod_settings");
        std::vector<std::string>   values = stored ? stored->values : std::vector<std::string>();
        if (values.empty())
            values.emplace_back();
        for (std::string &one : values)
            one = with_value(one, key, v);
        config.set_key_value("filament_mod_settings", new ConfigOptionStrings(values));
    } else {
        const ConfigOptionString *stored = config.option<ConfigOptionString>("mod_settings");
        config.set_key_value("mod_settings", new ConfigOptionString(with_value(stored ? stored->value : std::string(), key, v)));
    }
}

void set_calibration_object_option(ModelObject &object, const std::string &key, const std::string &value)
{
    if (key.find('.') == std::string::npos) {
        if (!PrintObjectConfig::defaults().has(key) && !PrintRegionConfig::defaults().has(key))
            throw ModError("the calibration sets \"" + key + "\" on an object, which takes it only in a preset");
        DynamicPrintConfig one;
        set_orca_option(one, key, value);
        object.config.set_key_value(key, one.option(key)->clone());
        return;
    }
    const ModParam &param = active_param(key);
    if (param.scope != "object" && param.scope != "region")
        throw ModError("the calibration sets \"" + key + "\" on an object, which takes it only in a preset");
    const json         v      = checked_param_json(param, key, value);
    const ConfigOption *stored = object.config.option("object_mod_settings");
    const std::string   text   = stored != nullptr ? static_cast<const ConfigOptionString *>(stored)->value : std::string();
    object.config.set_key_value("object_mod_settings", new ConfigOptionString(with_value(text, key, v)));
}

void apply_calibration_options(const CalibrationProject &project, DynamicPrintConfig &print, DynamicPrintConfig &filament,
                               DynamicPrintConfig &printer)
{
    for (const CalibrationOption &option : project.options)
        set_calibration_option(option.preset == CalibrationPreset::Filament ? filament :
                               option.preset == CalibrationPreset::Printer  ? printer : print,
                               option.key, option.value);
}

} // namespace ModHost
} // namespace Slic3r
