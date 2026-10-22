// MAGPIE: the support type dropdown with the support generators of the loaded mods (ModHost/SupportMenu.hpp). A mod
// adds its entries through its manifest alone, so the mods below are manifests only.
#include <catch2/catch_all.hpp>

#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/ModHost/SupportMenu.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::ModHost;
using Catch::Matchers::ContainsSubstring;

namespace {

// A support mod filling `slot` as `impl`, picked while its `enable` holds at `priority`, with `entries` as its menu
// entries ({label, support_type}).
std::string support_mod(const std::string &id, const std::string &slot, const std::string &impl, int priority,
                        const std::vector<std::pair<std::string, std::string>> &entries, bool enable_default = false)
{
    nlohmann::json menu = nlohmann::json::array();
    for (const auto &[label, type] : entries)
        menu.push_back({ { "label", label }, { "label_ko", label + " ko" }, { "support_type", type } });
    nlohmann::json slot_decl = { { "slot", slot }, { "version", 1 }, { "impl", impl }, { "when", id + ".enable" }, { "priority", priority } };
    if (!entries.empty())
        slot_decl["menu"] = { { "param", "enable" }, { "entries", menu } };
    const nlohmann::json mod = {
        { "id", id }, { "version", "1.0.0" }, { "api", { { "major", 1 }, { "min_minor", 0 } } }, { "binary", id },
        { "slots", nlohmann::json::array({ slot_decl }) },
        { "params", { { "enable", { { "type", "bool" }, { "default", enable_default }, { "invalidates", { "posSupportMaterial" } } } } } },
    };
    return mod.dump();
}

ModSet build(const std::vector<std::string> &manifests)
{
    std::vector<Mod> mods;
    for (const std::string &m : manifests)
        mods.push_back(ModSet::parse_mod(m));
    return ModSet::build(std::move(mods));
}

DynamicPrintConfig process(const std::string &support_type, const nlohmann::json &values = nlohmann::json::object())
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "support_type", support_type } });
    config.set_key_value("mod_settings", new ConfigOptionString(values.empty() ? std::string() : values.dump()));
    return config;
}

nlohmann::json stored(const DynamicPrintConfig &config)
{
    const std::string text = config.opt_string("mod_settings");
    return text.empty() ? nlohmann::json::object() : nlohmann::json::parse(text);
}

} // namespace

TEST_CASE("The support dropdown lists Orca's types, then each support mod's entries in load order", "[ModHost][SupportMenu]")
{
    const ModSet mods = build({ support_mod("alpha", "support.areas", "alpha", 0, { { "Alpha (auto)", "normal(auto)" }, { "Alpha (manual)", "normal(manual)" } }),
                                support_mod("beta", "support.compose", "beta", 2, { { "Beta", "normal(auto)" } }),
                                support_mod("quiet", "support.areas", "quiet", 1, {}) });
    const std::vector<SupportMenuItem> items = support_menu(&mods);
    REQUIRE(items.size() == 4 + 3);
    for (size_t i = 0; i < 4; ++i)
        CHECK(items[i].mod_id.empty());
    CHECK(items[0].support_type == "normal(auto)");
    CHECK(items[3].support_type == "tree(manual)");
    CHECK(items[4].label == "Alpha (auto)");
    CHECK(items[4].param_key == "alpha.enable");
    CHECK(items[5].support_type == "normal(manual)");
    CHECK(items[6].mod_id == "beta");
    CHECK(items[6].labels.at("ko") == "Beta ko");
    // A mod without a menu adds nothing, and its parameter stays on the Mods page.
    CHECK(is_support_menu_param(*mods.find_mod("alpha"), "enable"));
    CHECK_FALSE(is_support_menu_param(*mods.find_mod("quiet"), "enable"));
    // No mods: Orca's four.
    CHECK(support_menu(nullptr).size() == 4);
}

TEST_CASE("The dropdown shows the generator the slot choice picks", "[ModHost][SupportMenu]")
{
    const ModSet mods = build({ support_mod("alpha", "support.areas", "alpha", 0, { { "Alpha (auto)", "normal(auto)" }, { "Alpha (manual)", "normal(manual)" } }),
                                support_mod("beta", "support.compose", "beta", 2, { { "Beta", "normal(auto)" } }) });
    const std::vector<SupportMenuItem> items = support_menu(&mods);
    // No mod on: Orca's entry of support_type.
    CHECK(support_menu_selection(&mods, items, process("normal(auto)")) == 0);
    CHECK(support_menu_selection(&mods, items, process("tree(manual)")) == 3);
    // A mod on: its entry standing on support_type, else its first.
    CHECK(support_menu_selection(&mods, items, process("normal(manual)", { { "alpha.enable", true } })) == 5);
    CHECK(support_menu_selection(&mods, items, process("tree(auto)", { { "alpha.enable", true } })) == 4);
    // Both on (a file, the CLI): the one the slot choice picks, by priority, as the slice does.
    CHECK(support_menu_selection(&mods, items, process("normal(auto)", { { "alpha.enable", true }, { "beta.enable", true } })) == 6);
}

TEST_CASE("Picking an entry turns its generator on and every other support generator off", "[ModHost][SupportMenu]")
{
    const ModSet mods = build({ support_mod("alpha", "support.areas", "alpha", 0, { { "Alpha (auto)", "normal(auto)" }, { "Alpha (manual)", "normal(manual)" } }),
                                support_mod("beta", "support.compose", "beta", 2, { { "Beta", "normal(auto)" } }),
                                support_mod("gamma", "support.contacts", "gamma", 1, { { "Gamma", "normal(auto)" } }, /*enable_default=*/true) });
    const std::vector<SupportMenuItem> items = support_menu(&mods);
    DynamicPrintConfig config = process("tree(auto)", { { "beta.enable", true }, { "other_mod.value", 3 } });

    SECTION("a mod's entry") {
        const std::vector<std::string> changed = apply_support_menu_item(&mods, items, 5, config); // Alpha (manual)
        CHECK(changed == std::vector<std::string>{ "support_type", "mod_settings" });
        CHECK(config.opt_enum<SupportType>("support_type") == stNormal);
        const nlohmann::json values = stored(config);
        CHECK(values.at("alpha.enable") == true);
        CHECK_FALSE(values.contains("beta.enable"));   // off is its default: removed
        CHECK(values.at("gamma.enable") == false);     // on by default: written off
        CHECK(values.at("other_mod.value") == 3);      // values of other mods are kept
        CHECK(support_menu_selection(&mods, items, config) == 5);
    }
    SECTION("an Orca type") {
        apply_support_menu_item(&mods, items, 1, config); // Tree (auto)
        CHECK(config.opt_enum<SupportType>("support_type") == stTreeAuto);
        const nlohmann::json values = stored(config);
        CHECK_FALSE(values.contains("alpha.enable"));
        CHECK_FALSE(values.contains("beta.enable"));
        CHECK(values.at("gamma.enable") == false);
        CHECK(support_menu_selection(&mods, items, config) == 1);
    }
    SECTION("the entry already shown changes nothing") {
        DynamicPrintConfig shown = process("normal(auto)", { { "beta.enable", true }, { "gamma.enable", false } });
        CHECK(apply_support_menu_item(&mods, items, support_menu_selection(&mods, items, shown), shown).empty());
    }
}

TEST_CASE("A support menu is checked when its manifest loads", "[ModHost][SupportMenu]")
{
    auto with = [](const nlohmann::json &slot_patch, const nlohmann::json &param) {
        nlohmann::json mod = nlohmann::json::parse(support_mod("alpha", "support.areas", "alpha", 0, { { "Alpha", "normal(auto)" } }));
        mod["slots"][0].merge_patch(slot_patch);
        if (!param.is_null())
            mod["params"]["enable"] = param;
        return mod.dump();
    };
    CHECK_THROWS_WITH(ModSet::parse_mod(with({ { "slot", "wall.plan" } }, nullptr)), ContainsSubstring("support slots"));
    CHECK_THROWS_WITH(ModSet::parse_mod(with({ { "menu", { { "param", "enable" }, { "entries", { { { "label", "A" }, { "support_type", "sideways" } } } } } } }, nullptr)),
                      ContainsSubstring("unknown support_type"));
    CHECK_THROWS_WITH(ModSet::parse_mod(with({}, { { "type", "float" }, { "default", 0. }, { "invalidates", { "posSupportMaterial" } } })),
                      ContainsSubstring("bool parameter"));
    CHECK_NOTHROW(ModSet::parse_mod(with({}, nullptr)));
}

TEST_CASE("A parameter's choices can be the implementations the loaded mods register for a slot", "[ModHost][SupportMenu]")
{
    nlohmann::json composer = nlohmann::json::parse(support_mod("beta", "support.compose", "beta", 2, { { "Beta", "normal(auto)" } }));
    composer["params"]["normal_generator"] = { { "type", "string" }, { "default", "orca" }, { "choices_from", "support.areas" },
                                               { "invalidates", { "posSupportMaterial" } } };
    const ModSet mods = build({ support_mod("alpha", "support.areas", "alpha", 0, { { "Alpha", "normal(auto)" } }), composer.dump(),
                                support_mod("later", "support.areas", "later_gen", 1, {}) });
    CHECK(mods.find_param("beta.normal_generator")->choices_from == "support.areas");
    // Orca's own first, then each registered area generator in load order: one installed later is listed too.
    CHECK(support_choices(&mods, "support.areas") == std::vector<std::string>{ "orca", "alpha", "later_gen" });
    CHECK(slot_provider(&mods, "support.areas", "later_gen") == mods.find_mod("later"));
    CHECK(slot_provider(&mods, "support.areas", "orca") == nullptr);
    composer["params"]["normal_generator"]["options"] = { "orca", "alpha" };
    CHECK_THROWS_WITH(ModSet::parse_mod(composer.dump()), ContainsSubstring("choices_from"));
}

#ifdef MAGPIE_OFFICIAL_MODS_DIR
TEST_CASE("The official support mods add Cura, Mixed and Resin to the support dropdown", "[ModHost][SupportMenu][OfficialMods]")
{
    std::vector<std::string> errors;
    const ModSet             mods = ModSet::load_dir(MAGPIE_OFFICIAL_MODS_DIR, errors);
    CAPTURE(errors);
    REQUIRE(errors.empty());
    std::vector<std::string> labels;
    for (const SupportMenuItem &item : support_menu(&mods))
        if (!item.mod_id.empty()) {
            labels.push_back(item.label);
            CHECK_FALSE(item.labels.at("ko").empty());
        }
    // Load order is the folder order: cura_support, mixed_support, resin_support.
    CHECK(labels == std::vector<std::string>{ "Cura (auto)", "Cura (manual)", "Mixed (auto)", "Resin (auto)" });
    for (const char *id : { "cura_support", "mixed_support", "resin_support" })
        CHECK(is_support_menu_param(*mods.find_mod(id), "enable"));
    // Line 1's support types load as these entries (LegacySettings): normal_cura(manual) is Cura (manual).
    const std::vector<SupportMenuItem> items = support_menu(&mods);
    CHECK(items[support_menu_selection(&mods, items, process("normal(manual)", { { "cura_support.enable", true } }))].label == "Cura (manual)");
    // Mixed's normal support generator lists the area generators: Orca's and Cura's.
    CHECK(support_choices(&mods, mods.find_param("mixed_support.normal_generator")->choices_from) == std::vector<std::string>{ "orca", "cura" });
}
#endif
