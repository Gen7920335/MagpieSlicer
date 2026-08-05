#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <boost/filesystem.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {

namespace fs = boost::filesystem;

struct TempPresetDir {
    fs::path path;

    TempPresetDir()
    {
        path = fs::temp_directory_path() / fs::unique_path("orcaslicer-preset-%%%%-%%%%-%%%%");
        fs::create_directories(path);
    }

    ~TempPresetDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ScopedDataDir {
    std::string previous;

    explicit ScopedDataDir(const fs::path &path) : previous(data_dir())
    {
        set_data_dir(path.string());
    }

    ~ScopedDataDir()
    {
        set_data_dir(previous);
    }
};

void write_print_preset(const DynamicPrintConfig &default_config, const fs::path &file, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>("print_settings_id", true)->value = name;
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Write a preset json carrying a name and an "inherits" value, using the given collection's
// default config so it loads back into that collection. Works for any preset type.
void write_preset_with_inherits(const DynamicPrintConfig &default_config, const fs::path &file,
                                const std::string &name, const std::string &inherits)
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Add an in-memory preset (no file) with the given inherits value (empty => root preset).
Preset &add_inmemory_preset(PresetCollection &coll, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(coll.default_preset().config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;
    return coll.load_preset(std::string(), name, config, /*select=*/false);
}

// Mark an already-loaded preset as renamed from one or more former names.
void set_renamed_from(PresetCollection &coll, const std::string &preset_name, std::vector<std::string> old_names)
{
    for (auto it = coll.begin(); it != coll.end(); ++it)
        if (it->name == preset_name)
            it->renamed_from = std::move(old_names);
}

// A standalone print preset collection that exposes the protected rename-map builder, so a
// renamed_from scenario can be set up without the full system-profile load pipeline.
// (PresetCollection is non-copyable - it holds a mutex - so it is constructed directly with
// the same type/keys/defaults PresetBundle uses for its print collection.)
struct RenameTestCollection : public PresetCollection
{
    RenameTestCollection()
        : PresetCollection(Preset::TYPE_PRINT, Preset::print_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {}
    using PresetCollection::update_map_system_profile_renamed;
};

template<class Option>
Option &required_option(DynamicPrintConfig &config, const std::string &key)
{
    CAPTURE(key);
    Option *option = config.option<Option>(key);
    REQUIRE(option != nullptr);
    return *option;
}

void check_effective_option(const ConfigOption &expected,
                            const ConfigOption &actual,
                            const ConfigOption &parent)
{
    if (!actual.is_vector() || !actual.nullable()) {
        CHECK(actual.serialize() == expected.serialize());
        return;
    }

    const auto &expected_vector = static_cast<const ConfigOptionVectorBase &>(expected);
    const auto &actual_vector   = static_cast<const ConfigOptionVectorBase &>(actual);
    const auto &parent_vector   = static_cast<const ConfigOptionVectorBase &>(parent);
    const std::vector<std::string> expected_values = expected_vector.vserialize();
    const std::vector<std::string> actual_values   = actual_vector.vserialize();
    const std::vector<std::string> parent_values   = parent_vector.vserialize();

    REQUIRE(actual_values.size() == expected_values.size());
    REQUIRE(!parent_values.empty());
    for (size_t index = 0; index < expected_values.size(); ++index) {
        const std::string &effective = actual_vector.is_nil(index) ?
            parent_values[std::min(index, parent_values.size() - 1)] :
            actual_values[index];
        CHECK(effective == expected_values[index]);
    }
}

TEST_CASE("User preset import is recursive and never overwrites local files", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;

    const fs::path source_user      = source.path / PRESET_USER_DIR;
    const fs::path destination_user = destination.path / PRESET_USER_DIR;
    const fs::path nested_file      = fs::path("default") / "process" / "Imported.json";
    const fs::path collision_file   = fs::path("default") / "printer" / "Existing.json";

    fs::create_directories((source_user / nested_file).parent_path());
    fs::create_directories((source_user / collision_file).parent_path());
    fs::create_directories((destination_user / collision_file).parent_path());
    save_string_file((source_user / nested_file).string(), "imported");
    save_string_file((source_user / collision_file).string(), "source");
    save_string_file((destination_user / collision_file).string(), "destination");

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle bundle;
    const auto first = bundle.import_user_presets_from(source.path.string());

    CHECK(first.copied == 1);
    CHECK(first.skipped == 1);
    CHECK(first.failed == 0);
    std::string imported_contents;
    std::string existing_contents;
    load_string_file(destination_user / nested_file, imported_contents);
    load_string_file(destination_user / collision_file, existing_contents);
    CHECK(imported_contents == "imported");
    CHECK(existing_contents == "destination");

    const auto second = bundle.import_user_presets_from(source.path.string());

    CHECK(second.copied == 0);
    CHECK(second.skipped == 2);
    CHECK(second.failed == 0);
}

void check_json_roundtrip(const DynamicPrintConfig &config,
                          const DynamicPrintConfig &defaults,
                          PresetCollection &collection,
                          Preset::Type type,
                          const std::vector<std::string> &keys,
                          const fs::path &path)
{
    const std::vector<std::string> dirty = config.diff(defaults);
    for (const std::string &key : keys) {
        CAPTURE(key);
        REQUIRE(config.has(key));
    }

    DynamicPrintConfig parent = defaults;
    Preset             preset(type, "Roundtrip");
    preset.file   = path.string();
    preset.config = config;
    preset.save(&parent);

    DynamicPrintConfig                  loaded;
    std::map<std::string, std::string> metadata;
    std::string                         reason;
    const ConfigSubstitutions substitutions = loaded.load_from_json(
        path.string(), ForwardCompatibilitySubstitutionRule::Disable, metadata, reason);

    CHECK(reason.empty());
    CHECK(substitutions.empty());
    for (const std::string &key : keys) {
        CAPTURE(key);
        const ConfigOption *expected = config.option(key);
        const ConfigOption *actual   = loaded.option(key);
        REQUIRE(expected != nullptr);
        const bool should_be_stored = std::find(dirty.begin(), dirty.end(), key) != dirty.end();
        CHECK((actual != nullptr) == should_be_stored);
    }

    DynamicPrintConfig validated = loaded;
    CHECK(Preset::remove_invalid_keys(validated, defaults).empty());
    Preset &reloaded = collection.load_preset(path.string(), "Roundtrip reloaded", validated, false);
    for (const std::string &key : keys) {
        CAPTURE(key);
        const ConfigOption *expected = config.option(key);
        const ConfigOption *actual   = reloaded.config.option(key);
        const ConfigOption *parent   = defaults.option(key);
        REQUIRE(expected != nullptr);
        REQUIRE(actual != nullptr);
        REQUIRE(parent != nullptr);
        check_effective_option(*expected, *actual, *parent);
    }
}

} // namespace

TEST_CASE("Preset identity is canonicalized from load path", "[Preset][Identity]")
{
    TempPresetDir              temp_dir;
    PresetBundle               bundle;
    PresetsConfigSubstitutions substitutions;

    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_PRINT_NAME / "User.json", "User");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_LOCAL_DIR / "bundle-1" / PRESET_PRINT_NAME / "LocalBundle.json", "LocalBundle");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1" / PRESET_PRINT_NAME / "Subscribed.json", "Subscribed");

    bundle.prints.load_presets(temp_dir.path.string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_LOCAL_DIR / "bundle-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *root_user = bundle.prints.find_preset("User");
    REQUIRE(root_user != nullptr);
    CHECK(root_user->name == "User");
    CHECK_FALSE(root_user->is_from_bundle());

    const Preset *local_bundle = bundle.prints.find_preset("_local/bundle-1/LocalBundle");
    REQUIRE(local_bundle != nullptr);
    CHECK(local_bundle->name == "_local/bundle-1/LocalBundle");
    CHECK(local_bundle->is_from_bundle());

    const Preset *subscribed = bundle.prints.find_preset("_subscribed/remote-1/Subscribed");
    REQUIRE(subscribed != nullptr);
    CHECK(subscribed->name == "_subscribed/remote-1/Subscribed");
    CHECK(subscribed->is_from_bundle());
}

TEST_CASE("Legacy bundle import without bundle metadata stays in the user preset directory", "[Preset][Identity]")
{
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    PresetsConfigSubstitutions substitutions;
    std::vector<std::string>   result;
    int                        overwrite = 0;
    std::string                file      = (temp_dir.path / "legacy-bundle" / "Imported.json").string();
    const fs::path             user_root = temp_dir.path / "user";

    write_print_preset(bundle.prints.default_preset().config, file, "Imported");
    fs::create_directories(user_root);
    bundle.prints.update_user_presets_directory(user_root.string(), PRESET_PRINT_NAME);

    REQUIRE(bundle.import_json_presets(
        substitutions,
        file,
        [](std::string const &) { return 1; },
        ForwardCompatibilitySubstitutionRule::Disable,
        overwrite,
        result));

    const Preset *imported = bundle.prints.find_preset("Imported");
    REQUIRE(imported != nullptr);
    CHECK(imported->name == "Imported");
    CHECK(imported->bundle_id.empty());
    CHECK_FALSE(imported->is_from_bundle());
    // Detached user presets (no inherits) are saved in the "base" subfolder of the user preset root.
    CHECK(fs::equivalent(fs::path(imported->file).parent_path().parent_path(), user_root / PRESET_PRINT_NAME));
}

TEST_CASE("Current vendor type tolerates missing printer model", "[Preset][Bundle]")
{
    PresetBundle bundle;

    VendorProfile orca_vendor("ORCA");
    VendorProfile::PrinterModel model;
    model.name = "Orca Test";
    orca_vendor.models.emplace_back(model);
    bundle.vendors.emplace("ORCA", std::move(orca_vendor));

    bundle.printers.get_edited_preset().config.erase("printer_model");

    CHECK(bundle.get_current_vendor_type() == VendorType::Unknown);
}

TEST_CASE("Printer extruder count tolerates missing nozzle diameter", "[Preset][Bundle]")
{
    PresetBundle bundle;
    DynamicPrintConfig& config = bundle.printers.get_edited_preset().config;

    config.erase("nozzle_diameter");
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats());
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6 }));
    CHECK(bundle.get_printer_extruder_count() == 2);
}

TEST_CASE("find_preset resolves a system preset's renamed_from", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" is the current preset; it was renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // The rename map knows the old name...
    const std::string *renamed = coll.get_preset_name_renamed("Old Process");
    REQUIRE(renamed != nullptr);
    CHECK(*renamed == "New Process");

    // ...and plain find_preset() now follows it (the core of this PR; previously this
    // resolution lived only in find_preset2 and a few call sites).
    const Preset *resolved = coll.find_preset("Old Process");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->name == "New Process");

    // A genuinely unknown name still returns null (no spurious match).
    CHECK(coll.find_preset("Totally Unknown") == nullptr);

    // A child that still inherits the OLD name resolves through the runtime walker,
    // which uses plain find_preset().
    Preset       &child  = add_inmemory_preset(coll, "Child Process", "Old Process");
    const Preset *parent = coll.get_preset_parent(child);
    REQUIRE(parent != nullptr);
    CHECK(parent->name == "New Process");
}

TEST_CASE("find_preset resolves a preset renamed more than once", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" was renamed twice, so it carries both former names in renamed_from.
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Original Process", "Old Process" });
    coll.update_map_system_profile_renamed();

    // Each historical name resolves to the current preset.
    for (const char *old_name : { "Original Process", "Old Process" }) {
        INFO("resolving old name: " << old_name);
        const std::string *renamed = coll.get_preset_name_renamed(old_name);
        REQUIRE(renamed != nullptr);
        CHECK(*renamed == "New Process");

        const Preset *resolved = coll.find_preset(old_name);
        REQUIRE(resolved != nullptr);
        CHECK(resolved->name == "New Process");
    }

    // A child inheriting either former name resolves through the runtime walker.
    Preset &child = add_inmemory_preset(coll, "Child Process", "Original Process");
    REQUIRE(coll.get_preset_parent(child) != nullptr);
    CHECK(coll.get_preset_parent(child)->name == "New Process");
}

TEST_CASE("find_preset2 auto-matches removed Generic vendor profiles to the library", "[Preset][Rename]")
{
    PresetBundle bundle;

    // The OrcaFilamentLibrary replacement that removed empty "<vendor> Generic" profiles map to.
    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // Plain lookups do NOT fuzzy-match a removed vendor profile.
    CHECK(bundle.filaments.find_preset("Voron Generic PLA") == nullptr);
    CHECK(bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/false) == nullptr);

    // With auto_match, the removed "Voron Generic PLA" resolves to "Generic PLA @System".
    const Preset *matched = bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/true);
    REQUIRE(matched != nullptr);
    CHECK(matched->name == "Generic PLA @System");

    // No library preset exists for an unrelated material => still no match.
    CHECK(bundle.filaments.find_preset2("BrandX Generic PETG", /*auto_match=*/true) == nullptr);
}

TEST_CASE("Renamed parent is normalized into a loaded preset's inherits", "[Preset][Rename]")
{
    TempPresetDir        temp_dir;
    RenameTestCollection coll;

    // Current parent, renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // A user preset on disk that still inherits the OLD name.
    write_preset_with_inherits(coll.default_preset().config,
                               temp_dir.path / PRESET_PRINT_NAME / "Child.json", "Child", "Old Process");

    PresetsConfigSubstitutions substitutions;
    coll.load_presets(temp_dir.path.string(), PRESET_PRINT_NAME, substitutions,
                      ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = coll.find_preset("Child");
    REQUIRE(child != nullptr);
    // The dangling "Old Process" was rewritten to the resolved parent name at load time,
    // so the runtime walker (plain find_preset) can resolve the chain.
    CHECK(child->inherits() == "New Process");
    REQUIRE(coll.get_preset_parent(*child) != nullptr);
    CHECK(coll.get_preset_parent(*child)->name == "New Process");
}

TEST_CASE("Removed Generic parent is normalized into a loaded filament's inherits", "[Preset][Rename]")
{
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // A user filament that still inherits a removed "<vendor> Generic PLA" profile.
    write_preset_with_inherits(bundle.filaments.default_preset().config,
                               temp_dir.path / PRESET_FILAMENT_NAME / "MyPLA.json", "MyPLA", "Voron Generic PLA");

    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(temp_dir.path.string(), PRESET_FILAMENT_NAME, substitutions,
                                  ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = bundle.filaments.find_preset("MyPLA");
    REQUIRE(child != nullptr);
    CHECK(child->inherits() == "Generic PLA @System");
    REQUIRE(bundle.filaments.get_preset_parent(*child) != nullptr);
    CHECK(bundle.filaments.get_preset_parent(*child)->name == "Generic PLA @System");
}

namespace {

// A live reference to a preset's compatible_printers / compatible_prints list. Fetches the *stored*
// preset (real=true) so writes and reads hit the same object; creates the option if absent.
std::vector<std::string> &compatible_list(PresetCollection &coll, const std::string &preset_name, const char *field_key)
{
    Preset *preset = coll.find_preset(preset_name, /*first_visible_if_not_found=*/false, /*real=*/true);
    REQUIRE(preset != nullptr);
    return preset->config.option<ConfigOptionStrings>(field_key, true)->values;
}

} // namespace

TEST_CASE("Renamed printer/process names are normalized into compatible lists on load", "[Preset][Rename]")
{
    PresetBundle bundle;

    // Current printer + process, each renamed from an older name.
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });
    add_inmemory_preset(bundle.prints, "New Process");
    set_renamed_from(bundle.prints, "New Process", { "Old Process" });

    // A user process still compatible with the OLD printer name.
    add_inmemory_preset(bundle.prints, "My Process");
    compatible_list(bundle.prints, "My Process", "compatible_printers") = { "Old Printer" };

    // A user filament referencing the OLD printer AND OLD process names, plus an unknown printer.
    add_inmemory_preset(bundle.filaments, "My Filament");
    compatible_list(bundle.filaments, "My Filament", "compatible_printers") = { "Old Printer", "Unknown Printer" };
    compatible_list(bundle.filaments, "My Filament", "compatible_prints")   = { "Old Process" };

    // Build the rename maps (done during system load in the real pipeline), then normalize.
    AppConfig app_config;
    bundle.load_installed_printers(app_config); // rebuilds every collection's rename map
    bundle.normalize_compatible_presets();

    // The stale printer name in a process' compatible_printers is rewritten to the current name.
    CHECK(compatible_list(bundle.prints, "My Process", "compatible_printers") == std::vector<std::string>{ "New Printer" });

    // The stale process name in a filament's compatible_prints is rewritten (this field has no
    // runtime rename fallback, so load-time normalization is the only fix).
    CHECK(compatible_list(bundle.filaments, "My Filament", "compatible_prints") == std::vector<std::string>{ "New Process" });

    // The renamed printer is rewritten while the unknown/deleted name is preserved as-is.
    CHECK(compatible_list(bundle.filaments, "My Filament", "compatible_printers") ==
          (std::vector<std::string>{ "New Printer", "Unknown Printer" }));

    // Normalizing rewrites config in place without flagging the preset dirty.
    CHECK_FALSE(bundle.prints.find_preset("My Process", false, true)->is_dirty);

    // A system preset that already references the current name is left untouched (idempotent no-op).
    bundle.normalize_compatible_presets();
    CHECK(compatible_list(bundle.prints, "My Process", "compatible_printers") == std::vector<std::string>{ "New Printer" });
}

TEST_CASE("Renamed names are normalized into a SYSTEM preset's compatible lists", "[Preset][Rename]")
{
    PresetBundle bundle;

    // Current printer + process, each renamed from an older name.
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });
    add_inmemory_preset(bundle.prints, "New Process");
    set_renamed_from(bundle.prints, "New Process", { "Old Process" });

    // A *system* (vendor) filament whose own compatible lists still reference the OLD names. A vendor
    // profile can point at a sibling preset that was later renamed, so system presets must be
    // normalized too (they are skipped by neither collection walk).
    add_inmemory_preset(bundle.filaments, "System Filament").is_system = true;
    compatible_list(bundle.filaments, "System Filament", "compatible_printers") = { "Old Printer" };
    compatible_list(bundle.filaments, "System Filament", "compatible_prints")   = { "Old Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config); // build the rename maps
    bundle.normalize_compatible_presets();

    // The stale references in the system preset are rewritten to the current names.
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_printers") ==
          std::vector<std::string>{ "New Printer" });
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_prints") ==
          std::vector<std::string>{ "New Process" });

    // The rewrite does not flag the system preset dirty, and is idempotent.
    CHECK_FALSE(bundle.filaments.find_preset("System Filament", false, true)->is_dirty);
    bundle.normalize_compatible_presets();
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_printers") ==
          std::vector<std::string>{ "New Printer" });
}

TEST_CASE("compatible_prints on SLA materials resolves against sla_prints, not prints", "[Preset][Rename]")
{
    PresetBundle bundle;

    // A renamed SLA process, and a same-named FFF process that must NOT be picked up: resolving the
    // SLA material's compatible_prints against `prints` would wrongly rewrite to "Wrong FFF Process".
    add_inmemory_preset(bundle.sla_prints, "New SLA Process");
    set_renamed_from(bundle.sla_prints, "New SLA Process", { "Old SLA Process" });
    add_inmemory_preset(bundle.prints, "Wrong FFF Process");
    set_renamed_from(bundle.prints, "Wrong FFF Process", { "Old SLA Process" });

    add_inmemory_preset(bundle.sla_materials, "My SLA Material");
    compatible_list(bundle.sla_materials, "My SLA Material", "compatible_prints") = { "Old SLA Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config);
    bundle.normalize_compatible_presets();

    CHECK(compatible_list(bundle.sla_materials, "My SLA Material", "compatible_prints") ==
          std::vector<std::string>{ "New SLA Process" });
}

TEST_CASE("Profile validator flags dangling and renamed preset references", "[Preset][Validate]")
{
    PresetBundle bundle;

    // Current printers: a real one, and a renamed one (its old name resolves via renamed_from).
    add_inmemory_preset(bundle.printers, "Real Printer");
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });

    // A real process, referenced from a filament's compatible_prints.
    add_inmemory_preset(bundle.prints, "Real Process").is_system = true;

    // A fully valid system filament: references only current names.
    add_inmemory_preset(bundle.filaments, "Good Filament").is_system = true;
    compatible_list(bundle.filaments, "Good Filament", "compatible_printers") = { "Real Printer" };
    compatible_list(bundle.filaments, "Good Filament", "compatible_prints")   = { "Real Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config); // build the rename maps

    // With only valid references, the validator is clean.
    CHECK_FALSE(bundle.check_preset_references());

    SECTION("deleted compatible_printers is flagged") {
        add_inmemory_preset(bundle.filaments, "Ghost Ref Filament").is_system = true;
        compatible_list(bundle.filaments, "Ghost Ref Filament", "compatible_printers") = { "Ghost Printer" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("renamed compatible_printers (old name) is flagged") {
        add_inmemory_preset(bundle.filaments, "Old Ref Filament").is_system = true;
        compatible_list(bundle.filaments, "Old Ref Filament", "compatible_printers") = { "Old Printer" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("deleted compatible_prints is flagged") {
        add_inmemory_preset(bundle.filaments, "Bad Process Ref").is_system = true;
        compatible_list(bundle.filaments, "Bad Process Ref", "compatible_prints") = { "Ghost Process" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("deleted inherits parent is flagged") {
        add_inmemory_preset(bundle.filaments, "Orphan Filament", "Ghost Parent").is_system = true;
        CHECK(bundle.check_preset_references());
    }

    SECTION("non-system preset with a dangling reference is ignored") {
        add_inmemory_preset(bundle.filaments, "User Filament"); // is_system stays false
        compatible_list(bundle.filaments, "User Filament", "compatible_printers") = { "Ghost Printer" };
        CHECK_FALSE(bundle.check_preset_references());
    }
}

TEST_CASE("Multi-nozzle settings remain owned by printer and process presets", "[Preset][Project][MultiNozzle]")
{
    PresetBundle bundle;
    bundle.filament_presets = { bundle.filaments.get_selected_preset_name() };
    DynamicPrintConfig &project = bundle.project_config;

    REQUIRE_FALSE(project.has("nozzle_diameter"));
    REQUIRE_FALSE(project.has("toolhead_outer_wall_line_width"));
    REQUIRE_FALSE(project.has("crisp_corner_large_nozzle_override_regions"));

    DynamicPrintConfig &printer = bundle.printers.get_edited_preset().config;
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.15 }));
    printer.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width", true)->values = {
        FloatOrPercent(0.42, false), FloatOrPercent(0.16, false)
    };
    DynamicPrintConfig &process = bundle.prints.get_edited_preset().config;
    process.set_key_value("use_smaller_nozzles_in_crisp_corners", new ConfigOptionBool(true));
    process.set_key_value("crisp_corner_small_nozzle_wall_count", new ConfigOptionInt(4));
    process.set_key_value("crisp_corner_small_nozzle_wall_speed",
                          new ConfigOptionFloatsOrPercentsNullable({ FloatOrPercent(35, false) }));
    process.set_key_value("crisp_corner_large_nozzle_override_regions",
                          new ConfigOptionStrings({ "5:12:1", "10:20:2" }));

    // Legacy projects may contain copies of preset-owned values. They must not
    // override the currently selected and saved printer/process presets.
    project.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.8 }));
    project.set_key_value("toolhead_outer_wall_line_width",
                          new ConfigOptionFloatsOrPercents({ FloatOrPercent(0.9, false) }));
    project.set_key_value("use_smaller_nozzles_in_crisp_corners", new ConfigOptionBool(false));
    project.set_key_value("crisp_corner_small_nozzle_wall_count", new ConfigOptionInt(1));
    project.set_key_value("crisp_corner_large_nozzle_override_regions",
                          new ConfigOptionStrings({ "1:999:1" }));

    const DynamicPrintConfig full = bundle.full_config();
    CHECK(full.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{ 0.4, 0.15 });
    CHECK(full.option<ConfigOptionBool>("use_smaller_nozzles_in_crisp_corners")->value);
    CHECK(full.option<ConfigOptionInt>("crisp_corner_small_nozzle_wall_count")->value == 4);
    CHECK(full.option<ConfigOptionFloatsOrPercentsNullable>("crisp_corner_small_nozzle_wall_speed")->values ==
          std::vector<FloatOrPercent>{ FloatOrPercent(35, false) });
    CHECK(full.option<ConfigOptionStrings>("crisp_corner_large_nozzle_override_regions")->values ==
          std::vector<std::string>{ "5:12:1", "10:20:2" });
}

TEST_CASE("Custom printer settings survive JSON save and reload", "[Preset][Roundtrip][CustomSettings]")
{
    const size_t level = GENERATE(0u, 1u, 2u, 3u);
    DYNAMIC_SECTION("boundary level " << level) {
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    const DynamicPrintConfig defaults = bundle.printers.default_preset().config;
    DynamicPrintConfig       printer  = defaults;

    const std::array<std::array<double, 4>, 4> nozzle_sets = {{
        {{ 0.15, 0.20, 0.40, 0.60 }},
        {{ 0.20, 0.40, 0.60, 0.80 }},
        {{ 0.40, 0.60, 0.80, 1.00 }},
        {{ 1.00, 0.80, 0.60, 0.40 }}
    }};
    required_option<ConfigOptionFloats>(printer, "nozzle_diameter").values =
        std::vector<double>(nozzle_sets[level].begin(), nozzle_sets[level].end());

    const std::array<const char *, 9> width_keys = {
        "toolhead_line_width",
        "toolhead_initial_layer_line_width",
        "toolhead_outer_wall_line_width",
        "toolhead_inner_wall_line_width",
        "toolhead_top_surface_line_width",
        "toolhead_sparse_infill_line_width",
        "toolhead_internal_solid_infill_line_width",
        "toolhead_support_line_width",
        "toolhead_bridge_line_width"
    };
    std::vector<std::string> keys = { "nozzle_diameter" };
    for (size_t index = 0; index < width_keys.size(); ++index) {
        std::vector<FloatOrPercent> widths;
        widths.reserve(nozzle_sets[level].size());
        for (size_t tool = 0; tool < nozzle_sets[level].size(); ++tool) {
            const double width = level == 0 ? 0.0 :
                                 level == 3 ? 10.0 :
                                 nozzle_sets[level][tool] * (1.0 + 0.05 * double(index + level));
            widths.emplace_back(width, false);
        }
        required_option<ConfigOptionFloatsOrPercents>(printer, width_keys[index]).values = std::move(widths);
        keys.emplace_back(width_keys[index]);
    }

    const std::array<Vec2d, 4> cooling_positions = {
        Vec2d(-1.0, -1.0), Vec2d(0.0, 0.0), Vec2d(4.0, 268.0), Vec2d(500.0, 500.0)
    };
    const std::array<Vec2d, 4> brush_starts = {
        Vec2d(-1.0, -1.0), Vec2d(0.0, 0.0), Vec2d(15.0, 270.0), Vec2d(500.0, 499.0)
    };
    const std::array<Vec2d, 4> brush_ends = {
        Vec2d(-1.0, -1.0), Vec2d(1.0, 0.0), Vec2d(45.0, 270.0), Vec2d(500.0, 500.0)
    };
    required_option<ConfigOptionPoint>(printer, "support_interface_cooling_position").value = cooling_positions[level];
    required_option<ConfigOptionPoint>(printer, "support_interface_brush_start").value = brush_starts[level];
    required_option<ConfigOptionPoint>(printer, "support_interface_brush_end").value = brush_ends[level];
    required_option<ConfigOptionInt>(printer, "support_interface_brush_repetitions").value =
        std::array<int, 4>{ 0, 1, 5, 10 }[level];
    required_option<ConfigOptionFloat>(printer, "support_interface_brush_speed").value =
        std::array<double, 4>{ 1.0, 80.0, 250.0, 500.0 }[level];
    keys.insert(keys.end(), {
        "support_interface_cooling_position",
        "support_interface_brush_start",
        "support_interface_brush_end",
        "support_interface_brush_repetitions",
        "support_interface_brush_speed"
    });

    check_json_roundtrip(printer, defaults, bundle.printers, Preset::TYPE_PRINTER, keys,
                         temp_dir.path / "printer.json");
    }
}

TEST_CASE("Custom process settings survive JSON save and reload", "[Preset][Roundtrip][CustomSettings]")
{
    const size_t level = GENERATE(0u, 1u, 2u, 3u);
    DYNAMIC_SECTION("boundary level " << level) {
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    const DynamicPrintConfig defaults = bundle.prints.default_preset().config;
    DynamicPrintConfig       process  = defaults;

    const bool enabled = level != 0;
    required_option<ConfigOptionBool>(process, "use_smaller_nozzles_in_crisp_corners").value = enabled;
    required_option<ConfigOptionInt>(process, "crisp_corner_detail_toolhead").value =
        std::array<int, 4>{ 0, 1, 8, 16 }[level];
    required_option<ConfigOptionInt>(process, "crisp_corner_small_nozzle_wall_count").value =
        std::array<int, 4>{ 0, 1, 50, 100 }[level];
    required_option<ConfigOptionPercent>(process, "crisp_corner_nozzle_wall_overlap").value =
        std::array<double, 4>{ 0.0, 15.0, 40.0, 80.0 }[level];
    required_option<ConfigOptionBool>(process, "crisp_corner_interlace_small_nozzle_walls").value = enabled;
    const std::array<std::vector<std::string>, 4> override_regions = {{
        {}, { "1:1:1" }, { "1:20:1", "18:44:3" }, { "1:999999:16", "1:999999:1" }
    }};
    required_option<ConfigOptionStrings>(process, "crisp_corner_large_nozzle_override_regions").values =
        override_regions[level];
    const std::array<FloatOrPercent, 4> detail_speeds = {
        FloatOrPercent(0, false), FloatOrPercent(10, false),
        FloatOrPercent(50, true), FloatOrPercent(500, false)
    };
    required_option<ConfigOptionFloatsOrPercentsNullable>(process, "crisp_corner_small_nozzle_wall_speed").values =
        std::vector<FloatOrPercent>(4, detail_speeds[level]);

    required_option<ConfigOptionBool>(process, "single_nozzle_low_temperature_interface").value = enabled;
    required_option<ConfigOptionBool>(process, "support_interface_auxiliary_fan_cooling_on_temperature_change").value = enabled;
    required_option<ConfigOptionBool>(process, "support_interface_nozzle_wiping_on_temperature_change").value = enabled;
    required_option<ConfigOptionBool>(process, "support_interface_temperature_drop_tower").value = enabled;
    required_option<ConfigOptionInt>(process, "support_interface_temperature").value =
        std::array<int, 4>{ 1, 120, 220, 350 }[level];
    required_option<ConfigOptionInt>(process, "support_interface_auxiliary_fan_speed").value =
        std::array<int, 4>{ 0, 25, 50, 100 }[level];
    required_option<ConfigOptionFloat>(process, "support_interface_heating_time").value =
        std::array<double, 4>{ 0.0, 5.0, 30.0, 60.0 }[level];
    required_option<ConfigOptionBool>(process, "support_interface_sublayer_pattern").value = enabled;
    required_option<ConfigOptionInt>(process, "support_interface_sublayer_start_layer").value =
        std::array<int, 4>{ 2, 3, 8, 100 }[level];
    required_option<ConfigOptionInt>(process, "support_interface_sublayer_end_layer").value =
        std::array<int, 4>{ 2, 5, 16, 100 }[level];
    const std::array<SupportMaterialInterfacePattern, 4> patterns = {
        smipAuto, smipRectilinear, smipGrid, smipTriangles
    };
    required_option<ConfigOptionEnum<SupportMaterialInterfacePattern>>(
        process, "support_interface_sublayer_pattern_type").value = patterns[level];
    required_option<ConfigOptionFloat>(process, "support_interface_sublayer_angle").value =
        std::array<double, 4>{ 0.0, 30.0, 90.0, 180.0 }[level];
    required_option<ConfigOptionInt>(process, "support_interface_sublayer_temperature").value =
        std::array<int, 4>{ 0, 120, 220, 300 }[level];
    required_option<ConfigOptionFloat>(process, "support_interface_spacing").value =
        std::array<double, 4>{ 0.0, 0.5, 2.5, 10.0 }[level];
    required_option<ConfigOptionBool>(process, "cura_solid_support_raft").value = enabled;
    required_option<ConfigOptionInt>(process, "tree_support_wall_count").value =
        std::array<int, 4>{ 0, 1, 5, 10 }[level];

    const std::vector<std::string> keys = {
        "use_smaller_nozzles_in_crisp_corners",
        "crisp_corner_detail_toolhead",
        "crisp_corner_small_nozzle_wall_count",
        "crisp_corner_nozzle_wall_overlap",
        "crisp_corner_interlace_small_nozzle_walls",
        "crisp_corner_large_nozzle_override_regions",
        "crisp_corner_small_nozzle_wall_speed",
        "single_nozzle_low_temperature_interface",
        "support_interface_auxiliary_fan_cooling_on_temperature_change",
        "support_interface_nozzle_wiping_on_temperature_change",
        "support_interface_temperature_drop_tower",
        "support_interface_temperature",
        "support_interface_auxiliary_fan_speed",
        "support_interface_heating_time",
        "support_interface_sublayer_pattern",
        "support_interface_sublayer_start_layer",
        "support_interface_sublayer_end_layer",
        "support_interface_sublayer_pattern_type",
        "support_interface_sublayer_angle",
        "support_interface_sublayer_temperature",
        "support_interface_spacing",
        "cura_solid_support_raft",
        "tree_support_wall_count"
    };

    check_json_roundtrip(process, defaults, bundle.prints, Preset::TYPE_PRINT, keys,
                         temp_dir.path / "process.json");
    }
}

