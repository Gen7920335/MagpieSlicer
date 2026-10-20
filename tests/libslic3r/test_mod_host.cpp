#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libslic3r/ModHost/Expr.hpp>
#include <libslic3r/ModHost/ModSet.hpp>
#include <libslic3r/ModHost/SlotRegistry.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <map>
#include <set>
#include <string>

using namespace Slic3r;
using namespace Slic3r::ModHost;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

// Values the expression tests read: a scalar, a two-element list and a mod parameter.
std::optional<ExprValue> fixture_lookup(const std::string &name, std::optional<size_t> index)
{
    static const std::map<std::string, std::vector<ExprValue>> values = {
        {"layer_height", {0.2}},
        {"nozzle_diameter", {0.4, 0.6}},
        {"support_type", {std::string("tree(auto)")}},
        {"enable_support", {true}},
        {"demo.gap", {1.5}},
    };
    auto it = values.find(name);
    if (it == values.end())
        return std::nullopt;
    const bool is_list = name == "nozzle_diameter";
    if (is_list != index.has_value())
        return std::nullopt;
    const size_t i = index.value_or(0);
    return i < it->second.size() ? std::optional<ExprValue>(it->second[i]) : std::nullopt;
}

double eval_number(const std::string &text) { return std::get<double>(Expr::parse(text).evaluate(fixture_lookup)); }
bool   eval_bool(const std::string &text) { return expr_truthy(Expr::parse(text).evaluate(fixture_lookup)); }

DynamicPrintConfig full_config()
{
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    return config;
}

ModSet build_one(const std::string &json) { return ModSet::build({ModSet::parse_mod(json)}); }

} // namespace

TEST_CASE("Expressions follow arithmetic precedence and read settings", "[ModHost]")
{
    CHECK_THAT(eval_number("1 + 2 * 3"), WithinAbs(7., 1e-12));
    CHECK_THAT(eval_number("(1 + 2) * 3"), WithinAbs(9., 1e-12));
    CHECK_THAT(eval_number("-layer_height * 2"), WithinAbs(-0.4, 1e-12));
    CHECK_THAT(eval_number("nozzle_diameter[1] * 1.1"), WithinAbs(0.66, 1e-12));
    CHECK_THAT(eval_number("demo.gap + 1"), WithinAbs(2.5, 1e-12));
    CHECK_THAT(eval_number("7 % 4"), WithinAbs(3., 1e-12));
}

TEST_CASE("Expressions compare, combine conditions and call functions", "[ModHost]")
{
    CHECK(eval_bool("support_type == \"tree(auto)\""));
    CHECK_FALSE(eval_bool("support_type != \"tree(auto)\""));
    CHECK(eval_bool("enable_support and layer_height < 0.3"));
    CHECK(eval_bool("not (layer_height > 1) && true"));
    CHECK(eval_bool("false || nozzle_diameter[0] >= 0.4"));
    CHECK_THAT(eval_number("if(layer_height > 0.1, 10, 20)"), WithinAbs(10., 1e-12));
    CHECK_THAT(eval_number("max(1, nozzle_diameter[1], 0.5)"), WithinAbs(1., 1e-12));
    CHECK_THAT(eval_number("min(3, 2)"), WithinAbs(2., 1e-12));
    CHECK_THAT(eval_number("round(2.5) + floor(1.9) + ceil(0.1) + abs(-1)"), WithinAbs(3. + 1. + 1. + 1., 1e-12));
}

TEST_CASE("An and that is false does not read its right side", "[ModHost]")
{
    // nozzle_diameter[5] is past the end of the list; short-circuiting must keep it unread.
    CHECK_FALSE(eval_bool("false and nozzle_diameter[5] > 0"));
}

TEST_CASE("Expressions list the names they read", "[ModHost]")
{
    const Expr e = Expr::parse("nozzle_diameter[0] * demo.gap + nozzle_diameter[1]");
    REQUIRE(e.names().size() == 2);
    CHECK(e.names()[0] == "nozzle_diameter");
    CHECK(e.names()[1] == "demo.gap");
}

TEST_CASE("Malformed expressions are rejected with a position", "[ModHost]")
{
    CHECK_THROWS_WITH(Expr::parse("1 +"), ContainsSubstring("position"));
    CHECK_THROWS_AS(Expr::parse("(1 + 2"), ExprError);
    CHECK_THROWS_AS(Expr::parse("1 $ 2"), ExprError);
    CHECK_THROWS_AS(Expr::parse("frobnicate(1)"), ExprError);
    CHECK_THROWS_AS(Expr::parse("if(1, 2)"), ExprError);
    CHECK_THROWS_AS(Expr::parse("\"open"), ExprError);
}

TEST_CASE("Evaluation errors name the problem", "[ModHost]")
{
    CHECK_THROWS_WITH(Expr::parse("1 / 0").evaluate(fixture_lookup), ContainsSubstring("division by zero"));
    CHECK_THROWS_WITH(Expr::parse("missing_key + 1").evaluate(fixture_lookup), ContainsSubstring("unknown name"));
    CHECK_THROWS_WITH(Expr::parse("nozzle_diameter[7]").evaluate(fixture_lookup), ContainsSubstring("unknown name"));
    CHECK_THROWS_AS(Expr::parse("support_type * 2").evaluate(fixture_lookup), ExprError);
}

TEST_CASE("A mod definition accepts comments and reads params and rules", "[ModHost]")
{
    const Mod mod = ModSet::parse_mod(R"({
        // A data-only mod.
        "id": "demo", "name": "Demo", "version": "0.1.0",
        "params": { "gap": { "type": "float", "default": 1.5, "unit": "mm", "page": "Support", "invalidates": ["posSupportMaterial"] } },
        /* one rule */
        "rules": [ { "target": "wall_loops", "value": "3", "when": "layer_height < 0.3", "priority": 10 } ]
    })");
    CHECK(mod.id == "demo");
    REQUIRE(mod.params.size() == 1);
    CHECK(mod.params[0].name == "gap");
    CHECK(std::get<double>(mod.params[0].default_value) == 1.5);
    CHECK(mod.params[0].invalidates == std::vector<std::string>{"posSupportMaterial"});
    REQUIRE(mod.rules.size() == 1);
    CHECK(mod.rules[0].target == "wall_loops");
    CHECK(mod.rules[0].priority == 10);
    CHECK(mod.rules[0].when.has_value());
}

TEST_CASE("Mod definitions with bad fields are rejected", "[ModHost]")
{
    CHECK_THROWS_AS(ModSet::parse_mod("not json"), ModError);
    CHECK_THROWS_AS(ModSet::parse_mod(R"({"version": "1"})"), ModError);
    CHECK_THROWS_AS(ModSet::parse_mod(R"({"id": "Bad-Id", "version": "1"})"), ModError);
    CHECK_THROWS_AS(ModSet::parse_mod(R"({"id": "m", "version": "1", "params": {"p": {"type": "float", "default": "x"}}})"), ModError);
    CHECK_THROWS_AS(ModSet::parse_mod(R"({"id": "m", "version": "1", "rules": [{"target": "wall_loops", "value": "1 +"}]})"), ModError);
}

TEST_CASE("Loading rejects rules on unknown settings and unknown names", "[ModHost]")
{
    CHECK_THROWS_WITH(build_one(R"({"id": "m", "version": "1", "rules": [{"target": "no_such_key", "value": "1"}]})"),
                      ContainsSubstring("is not a setting"));
    CHECK_THROWS_WITH(build_one(R"({"id": "m", "version": "1", "rules": [{"target": "wall_loops", "value": "other.param"}]})"),
                      ContainsSubstring("neither a setting nor a mod parameter"));
}

TEST_CASE("Loading rejects two rules on one setting with the same priority", "[ModHost]")
{
    const Mod a = ModSet::parse_mod(R"({"id": "a", "version": "1", "rules": [{"target": "wall_loops", "value": "2", "priority": 5}]})");
    const Mod b = ModSet::parse_mod(R"({"id": "b", "version": "1", "rules": [{"target": "wall_loops", "value": "3", "priority": 5}]})");
    CHECK_THROWS_WITH(ModSet::build({a, b}), ContainsSubstring("priority 5"));
}

TEST_CASE("Loading rejects rules that depend on each other in a cycle", "[ModHost]")
{
    CHECK_THROWS_WITH(build_one(R"({"id": "m", "version": "1", "rules": [
                                    {"target": "wall_loops", "value": "top_shell_layers"},
                                    {"target": "top_shell_layers", "value": "wall_loops"}]})"),
                      ContainsSubstring("cycle"));
}

TEST_CASE("With no mods the config is left exactly as it came", "[ModHost]")
{
    DynamicPrintConfig       config = full_config();
    const DynamicPrintConfig before = config;
    const ModSet             empty  = ModSet::build({});
    CHECK(empty.empty());
    CHECK(empty.resolve(config).empty());
    CHECK(config == before);
}

TEST_CASE("A rule sets its setting from an expression over other settings and parameters", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    const ModSet mods = build_one(R"({"id": "demo", "version": "1",
        "params": {"extra": {"type": "int", "default": 2}},
        "rules": [{"target": "wall_loops", "value": "round(1 / layer_height) - demo.extra"}]})");

    REQUIRE(mods.resolve(config).empty());
    // 1 / 0.2 mm = 5, minus the parameter's default of 2.
    CHECK(config.opt_int("wall_loops") == 3);

    // A parameter value given by the caller replaces the default.
    REQUIRE(mods.resolve(config, {{"demo.extra", 4.}}).empty());
    CHECK(config.opt_int("wall_loops") == 1);
}

TEST_CASE("A rule whose condition is false leaves its setting alone", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    config.set_key_value("wall_loops", new ConfigOptionInt(2));
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    const ModSet mods = build_one(R"({"id": "m", "version": "1",
        "rules": [{"target": "wall_loops", "value": "6", "when": "enable_support"}]})");
    REQUIRE(mods.resolve(config).empty());
    CHECK(config.opt_int("wall_loops") == 2);
}

TEST_CASE("A value the user changed in a preset wins over a mod rule", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    config.set_key_value("wall_loops", new ConfigOptionInt(2));
    config.set_key_value("top_shell_layers", new ConfigOptionInt(4));
    config.set_key_value("different_settings_to_system", new ConfigOptionStrings({"wall_loops;sparse_infill_density", "", ""}));
    const ModSet mods = build_one(R"({"id": "m", "version": "1", "rules": [
        {"target": "wall_loops", "value": "6"},
        {"target": "top_shell_layers", "value": "7"}]})");
    REQUIRE(mods.resolve(config).empty());
    CHECK(config.opt_int("wall_loops") == 2);
    CHECK(config.opt_int("top_shell_layers") == 7);
}

TEST_CASE("The higher priority rule on a setting wins", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    const Mod low  = ModSet::parse_mod(R"({"id": "low", "version": "1", "rules": [{"target": "wall_loops", "value": "2", "priority": 1}]})");
    const Mod high = ModSet::parse_mod(R"({"id": "high", "version": "1", "rules": [{"target": "wall_loops", "value": "wall_loops + 3", "priority": 9}]})");
    // Listed high first: the order comes from priority, not from load order.
    const ModSet mods = ModSet::build({high, low});
    REQUIRE(mods.resolve(config).empty());
    // low sets 2, then high reads that and adds 3.
    CHECK(config.opt_int("wall_loops") == 5);
}

TEST_CASE("A rule reading another rule's setting runs after it", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    // Declared reader first; the dependency must still run first.
    const ModSet mods = build_one(R"({"id": "m", "version": "1", "rules": [
        {"target": "top_shell_layers", "value": "wall_loops * 2"},
        {"target": "wall_loops", "value": "3"}]})");
    REQUIRE(mods.resolve(config).empty());
    CHECK(config.opt_int("wall_loops") == 3);
    CHECK(config.opt_int("top_shell_layers") == 6);
}

TEST_CASE("Rules write list settings whole or by index", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    const ModSet all = build_one(R"({"id": "m", "version": "1", "rules": [{"target": "nozzle_diameter", "value": "0.6"}]})");
    REQUIRE(all.resolve(config).empty());
    CHECK(config.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.6, 0.6});

    const ModSet one = build_one(R"({"id": "m", "version": "1", "rules": [{"target": "nozzle_diameter", "value": "0.25", "index": 1}]})");
    REQUIRE(one.resolve(config).empty());
    CHECK(config.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.6, 0.25});
}

TEST_CASE("Rules write enum and percentage settings", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    const ModSet mods = build_one(R"({"id": "m", "version": "1", "rules": [
        {"target": "wall_generator", "value": "\"classic\""},
        {"target": "sparse_infill_density", "value": "\"35%\""}]})");
    REQUIRE(mods.resolve(config).empty());
    CHECK(config.option("wall_generator")->serialize() == "classic");
    CHECK(config.option("sparse_infill_density")->serialize() == "35%");
}

TEST_CASE("A rule that cannot be evaluated is reported and not applied", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    config.set_key_value("wall_loops", new ConfigOptionInt(2));
    const ModSet mods = build_one(R"({"id": "m", "version": "1", "rules": [{"target": "wall_loops", "value": "1 / 0"}]})");
    const std::vector<std::string> errors = mods.resolve(config);
    REQUIRE(errors.size() == 1);
    CHECK_THAT(errors.front(), ContainsSubstring("division by zero"));
    CHECK(config.opt_int("wall_loops") == 2);
}

TEST_CASE("Parameter values stored in the presets replace the defaults", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.walls": 4, "other.kept": 1})"));
    config.set_key_value("printer_mod_settings", new ConfigOptionString(R"({"demo.shells": 6})"));
    config.set_key_value("filament_mod_settings", new ConfigOptionStrings({R"({"demo.flow": 90})", ""}));
    const ModSet mods = build_one(R"({"id": "demo", "version": "1",
        "params": {"walls": {"type": "int", "default": 2},
                   "shells": {"type": "int", "default": 3, "scope": "printer"},
                   "flow": {"type": "float", "default": 100, "scope": "filament"}},
        "rules": [{"target": "wall_loops", "value": "demo.walls"},
                  {"target": "top_shell_layers", "value": "demo.shells"},
                  {"target": "filament_flow_ratio", "value": "demo.flow[0] / 100", "index": 0}]})");
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats({0.98, 0.98}));

    REQUIRE(mods.resolve(config).empty());
    CHECK(config.opt_int("wall_loops") == 4);
    CHECK(config.opt_int("top_shell_layers") == 6);
    CHECK_THAT(config.option<ConfigOptionFloats>("filament_flow_ratio")->values[0], WithinAbs(0.9, 1e-12));
    // A value of a mod that is not loaded stays where it was stored.
    CHECK(config.opt_string("mod_settings") == R"({"demo.walls": 4, "other.kept": 1})");
}

TEST_CASE("A filament parameter takes the default for a filament with no stored value", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    config.set_key_value("filament_mod_settings", new ConfigOptionStrings({R"({"demo.flow": 90})", ""}));
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats({0.98, 0.98}));
    const ModSet mods = build_one(R"({"id": "demo", "version": "1",
        "params": {"flow": {"type": "float", "default": 100, "scope": "filament"}},
        "rules": [{"target": "filament_flow_ratio", "value": "demo.flow[1] / 100", "index": 1}]})");
    REQUIRE(mods.resolve(config).empty());
    CHECK_THAT(config.option<ConfigOptionFloats>("filament_flow_ratio")->values[1], WithinAbs(1.0, 1e-12));
}

TEST_CASE("A stored parameter value of the wrong type is reported", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    config.set_key_value("wall_loops", new ConfigOptionInt(2));
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.walls": "many"})"));
    const ModSet mods = build_one(R"({"id": "demo", "version": "1",
        "params": {"walls": {"type": "int", "default": 2}},
        "rules": [{"target": "wall_loops", "value": "demo.walls"}]})");
    const std::vector<std::string> errors = mods.resolve(config);
    REQUIRE_FALSE(errors.empty());
    CHECK_THAT(errors.front(), ContainsSubstring("demo.walls"));
    CHECK(config.opt_int("wall_loops") == 2);
}

TEST_CASE("The built-in orca support generator is registered", "[ModHost]")
{
    const SupportGenerator *orca = find_support_generator("orca");
    REQUIRE(orca != nullptr);
    CHECK(orca->name == "orca");
    CHECK(find_support_generator("") == nullptr);
    CHECK(find_support_generator("no_such_generator") == nullptr);
}

TEST_CASE("A registered support generator replaces one of the same name", "[ModHost]")
{
    int calls = 0;
    register_support_generator({"test_gen", "Test", [&calls](PrintObject &) { ++calls; }});
    register_support_generator({"test_gen", "Test 2", [&calls](PrintObject &) { calls += 10; }});
    const SupportGenerator *g = find_support_generator("test_gen");
    REQUIRE(g != nullptr);
    CHECK(g->label == "Test 2");
    bool listed = false;
    for (const SupportGenerator *s : support_generators())
        listed |= s->name == "test_gen";
    CHECK(listed);
}

TEST_CASE("Mod names and parameter labels carry their own translations", "[ModHost]")
{
    const ModHost::Mod mod = ModHost::ModSet::parse_mod(R"({
        "id": "demo", "name": "Demo", "name_ko": "데모", "version": "1.0.0",
        "params": { "speed": { "type": "float", "default": 1, "label": "Speed", "label_ko": "속도", "label_pt_BR": "Velocidade" } }
    })");
    REQUIRE(mod.params.size() == 1);
    const ModHost::ModParam &param = mod.params.front();
    CHECK(ModHost::localized_text(param.labels, "ko_KR", param.label) == "속도"); // language part of a locale
    CHECK(ModHost::localized_text(param.labels, "ko", param.label) == "속도");
    CHECK(ModHost::localized_text(param.labels, "pt_BR", param.label) == "Velocidade"); // exact locale
    CHECK(ModHost::localized_text(param.labels, "de_DE", param.label) == "Speed");    // no translation: the label
    CHECK(ModHost::localized_text(mod.names, "ko_KR", mod.name) == "데모");
}

TEST_CASE("A string parameter with options is a dropdown that takes only its options", "[ModHost]")
{
    const Mod mod = ModSet::parse_mod(R"({"id": "demo", "version": "1",
        "params": {"kind": {"type": "string", "default": "off",
            "options": [{"value": "off", "label": "Off", "label_ko": "끔"}, "linear"]}}})");
    REQUIRE(mod.params.size() == 1);
    const ModParam &p = mod.params.front();
    REQUIRE(p.options.size() == 2);
    CHECK(p.options[0].value == "off");
    CHECK(localized_text(p.options[0].labels, "ko_KR", p.options[0].label) == "끔");
    CHECK(p.options[1].label == "linear"); // a plain string option is its own label
    CHECK(param_allows(p, std::string("linear")));
    CHECK_FALSE(param_allows(p, std::string("log")));

    CHECK_THROWS_WITH(ModSet::parse_mod(R"({"id": "m", "version": "1",
        "params": {"k": {"type": "string", "default": "x", "options": ["a", "b"]}}})"), ContainsSubstring("not one of the options"));
    CHECK_THROWS_WITH(ModSet::parse_mod(R"({"id": "m", "version": "1",
        "params": {"k": {"type": "int", "default": 1, "options": ["1"]}}})"), ContainsSubstring("only for a string"));
    CHECK_THROWS_WITH(ModSet::parse_mod(R"({"id": "m", "version": "1",
        "params": {"k": {"type": "string", "default": "a", "options": ["a", "a"]}}})"), ContainsSubstring("twice"));
    CHECK_THROWS_AS(ModSet::parse_mod(R"({"id": "m", "version": "1",
        "params": {"k": {"type": "string", "default": "a", "options": []}}})"), ModError);
}

TEST_CASE("A stored value outside a parameter's options is reported and not applied", "[ModHost]")
{
    DynamicPrintConfig config = full_config();
    config.set_key_value("wall_sequence", new ConfigOptionEnum<WallSequence>(WallSequence::InnerOuter));
    const ModSet mods = build_one(R"json({"id": "demo", "version": "1",
        "params": {"order": {"type": "string", "default": "inner", "options": ["inner", "outer"]}},
        "rules": [{"target": "wall_sequence", "value": "if(demo.order == \"outer\", \"outer wall/inner wall\", \"inner wall/outer wall\")"}]})json");
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.order": "outer"})"));
    REQUIRE(mods.resolve(config).empty());
    CHECK(config.opt_enum<WallSequence>("wall_sequence") == WallSequence::OuterInner);

    config.set_key_value("wall_sequence", new ConfigOptionEnum<WallSequence>(WallSequence::InnerOuter));
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.order": "sideways"})"));
    const std::vector<std::string> errors = mods.resolve(config);
    REQUIRE_FALSE(errors.empty());
    CHECK_THAT(errors.front(), ContainsSubstring("not one of its options"));
    CHECK(config.opt_enum<WallSequence>("wall_sequence") == WallSequence::InnerOuter);
}

TEST_CASE("A visible condition reads parameters and leaves slicing alone", "[ModHost]")
{
    const char *def = R"json({"id": "demo", "version": "1",
        "params": {
            "kind": {"type": "string", "default": "off", "options": ["off", "linear", "log"]},
            "gap":  {"type": "float", "default": 5, "visible": "demo.kind == \"linear\""}},
        "rules": [{"target": "wall_loops", "value": "round(demo.gap)"}]})json";
    const ModSet mods = build_one(def);
    // Parameters keep the file's order.
    const ModParam &gap = mods.mods().front().params[1];
    REQUIRE(gap.name == "gap");
    REQUIRE(gap.visible.has_value());
    auto lookup_kind = [](const std::string &kind) {
        return ExprLookup([kind](const std::string &name, std::optional<size_t>) -> std::optional<ExprValue> {
            if (name == "demo.kind")
                return kind;
            return std::nullopt;
        });
    };
    CHECK(param_visible(gap, lookup_kind("linear")));
    CHECK_FALSE(param_visible(gap, lookup_kind("log")));
    // A condition that cannot be evaluated shows the row.
    CHECK(param_visible(gap, [](const std::string &, std::optional<size_t>) { return std::optional<ExprValue>(); }));
    // The rule reads the hidden parameter's stored value all the same.
    DynamicPrintConfig config = full_config();
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.kind": "off", "demo.gap": 3})"));
    REQUIRE(mods.resolve(config).empty());
    CHECK(config.opt_int("wall_loops") == 3);

    CHECK_THROWS_WITH(build_one(R"({"id": "m", "version": "1",
        "params": {"g": {"type": "float", "default": 1, "visible": "layer_height > 0.1"}}})"), ContainsSubstring("not a mod parameter"));
    CHECK_THROWS_AS(ModSet::parse_mod(R"({"id": "m", "version": "1",
        "params": {"g": {"type": "float", "default": 1, "visible": "m.g >"}}})"), ModError);
}

TEST_CASE("Mod parameters keep the order of the definition", "[ModHost]")
{
    const Mod mod = ModSet::parse_mod(R"json({"id": "demo", "version": "1",
        "params": {"zeta": {"type": "bool", "default": false}, "alpha": {"type": "int", "default": 1}, "mid": {"type": "float", "default": 2}}})json");
    REQUIRE(mod.params.size() == 3);
    CHECK(mod.params[0].name == "zeta");
    CHECK(mod.params[1].name == "alpha");
    CHECK(mod.params[2].name == "mid");
}

TEST_CASE("A float_or_percent parameter takes a number or a percentage and passes it on", "[ModHost]")
{
    const ModSet mods = build_one(R"({"id": "demo", "version": "1",
        "params": {"speed": {"type": "float_or_percent", "default": 0, "invalidates": ["none"]}},
        "rules": [ {"target": "crisp_corner_small_nozzle_wall_speed", "value": "demo.speed"} ]})");
    DynamicPrintConfig config = full_config();
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.speed": "50%"})"));
    REQUIRE(mods.resolve(config).empty());
    const auto *speed = config.option<ConfigOptionFloatOrPercent>("crisp_corner_small_nozzle_wall_speed");
    CHECK(speed->percent);
    CHECK(speed->value == 50.);
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.speed": 30})"));
    REQUIRE(mods.resolve(config).empty());
    CHECK_FALSE(speed->percent);
    CHECK(speed->value == 30.);
    // Not a number and not a percentage: reported, never read as 0.
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.speed": "fast"})"));
    CHECK_THAT(mods.resolve(config).front(), ContainsSubstring("not a float_or_percent"));
    CHECK_THROWS_AS(ModSet::parse_mod(R"({"id": "m", "version": "1", "params": {"s": {"type": "float_or_percent", "default": "50"}}})"), ModError);
}

TEST_CASE("Invalidates lists name known steps, and none stands alone", "[ModHost]")
{
    auto param = [](const std::string &def) {
        return ModSet::parse_mod(R"({"id": "m", "version": "1", "params": {"p": )" + def + "}}");
    };
    CHECK_NOTHROW(param(R"({"type": "int", "default": 1, "invalidates": ["posPerimeters", "psGCodeExport"]})"));
    CHECK_THROWS_WITH(param(R"({"type": "int", "default": 1, "invalidates": ["support"]})"), ContainsSubstring("unknown step"));
    CHECK_THROWS_WITH(param(R"({"type": "int", "default": 1, "invalidates": ["none", "posSlice"]})"), ContainsSubstring("cannot be listed"));
    // Rules read the process value of an object or region parameter, so "none" would make its values dead.
    CHECK_THROWS_WITH(param(R"({"type": "int", "default": 1, "scope": "region", "invalidates": ["none"]})"), ContainsSubstring("name the steps"));
}

TEST_CASE("A changed parameter redoes the steps it declares, posSlice when it declares none", "[ModHost]")
{
    const ModSet mods = build_one(R"({"id": "demo", "version": "1", "params": {
        "support": {"type": "float", "default": 1, "invalidates": ["posSupportMaterial"]},
        "walls":   {"type": "float", "default": 1, "invalidates": ["posPerimeters", "psSkirtBrim"]},
        "rule":    {"type": "float", "default": 1, "invalidates": ["none"]},
        "silent":  {"type": "float", "default": 1} }})");
    using Steps = std::set<std::string>;
    CHECK(mods.invalidated_steps(R"({"demo.support": 1})", R"({"demo.support": 1})") == Steps{});
    CHECK(mods.invalidated_steps("", R"({"demo.support": 2})") == Steps{"posSupportMaterial"});
    CHECK(mods.invalidated_steps(R"({"demo.walls": 2})", "") == Steps{"posPerimeters", "psSkirtBrim"});
    CHECK(mods.invalidated_steps("", R"({"demo.rule": 2})") == Steps{});
    CHECK(mods.invalidated_steps("", R"({"demo.silent": 2})") == Steps{"posSlice"});
    // A value of a mod that is not loaded changes nothing that is sliced.
    CHECK(mods.invalidated_steps("", R"({"absent.x": 2})") == Steps{});
    CHECK(mods.invalidated_steps(R"({"demo.support": 2, "demo.rule": 1})", R"({"demo.support": 3, "demo.walls": 4})") ==
          Steps{"posSupportMaterial", "posPerimeters", "psSkirtBrim"});
}

TEST_CASE("An object or region parameter takes the most specific value stored", "[ModHost]")
{
    const ModSet mods = build_one(R"({"id": "demo", "version": "1", "params": {
        "o": {"type": "float", "default": 1, "scope": "object", "invalidates": ["posSlice"]},
        "r": {"type": "float", "default": 1, "scope": "region", "invalidates": ["posSlice"]},
        "p": {"type": "float", "default": 1} }})");
    DynamicPrintConfig config = full_config();
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.o": 2, "demo.r": 2, "demo.p": 2})"));
    const std::string object = R"({"demo.o": 3, "demo.r": 3, "demo.p": 3})";
    const std::string region = R"({"demo.o": 4, "demo.r": 4, "demo.p": 4})";
    ModSet::ValueSources sources;
    sources.print = &config;
    CHECK(std::get<double>(*mods.value_of("demo.r", sources)) == 2.);
    sources.object_values = &object;
    sources.region_values = &region;
    CHECK(std::get<double>(*mods.value_of("demo.r", sources)) == 4.);
    CHECK(std::get<double>(*mods.value_of("demo.o", sources)) == 3.); // an object parameter has no region level
    CHECK(std::get<double>(*mods.value_of("demo.p", sources)) == 2.); // a process parameter is the print's
    // A malformed level is an error, not a fall back to the less specific value.
    const std::string broken = "{not json";
    sources.region_values    = &broken;
    std::string error;
    CHECK_FALSE(mods.value_of("demo.r", sources, {}, &error).has_value());
    CHECK_THAT(error, ContainsSubstring("region_mod_settings is not a JSON object"));
    // Rules read the process value.
    const ModSet with_rule = build_one(R"({"id": "demo", "version": "1",
        "params": {"r": {"type": "int", "default": 1, "scope": "region", "invalidates": ["posSlice"]}},
        "rules": [ {"target": "wall_loops", "value": "demo.r"} ]})");
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"demo.r": 5})"));
    REQUIRE(with_rule.resolve(config).empty());
    CHECK(config.opt_int("wall_loops") == 5);
}

TEST_CASE("Region mod values join per parameter, the more specific level winning", "[ModHost]")
{
    CHECK(merge_mod_values("", "") == "");
    CHECK(merge_mod_values(R"({"a.x": 1, "a.y": 1})", R"({"a.y": 2, "b.z": 3})") == R"({"a.x":1,"a.y":2,"b.z":3})");
    CHECK(merge_mod_values(R"({"a.x": 1})", "") == R"({"a.x": 1})");
}

TEST_CASE("Only official mods write printer G-code settings", "[ModHost]")
{
    const char *json = R"({"id": "demo", "version": "1", "rules": [ {"target": "machine_start_gcode", "value": "\"G28\""} ]})";
    Mod mod = ModSet::parse_mod(json);
    CHECK_THROWS_WITH(ModSet::build({mod}), ContainsSubstring("only official mods may change \"machine_start_gcode\""));
    mod.official = true;
    CHECK_NOTHROW(ModSet::build({mod}));
    CHECK(is_gcode_setting("layer_change_gcode"));
    CHECK(is_gcode_setting("filament_start_gcode"));
    CHECK(is_gcode_setting("post_process"));
    CHECK_FALSE(is_gcode_setting("emit_machine_limits_to_gcode")); // a switch, not G-code
    CHECK_FALSE(is_gcode_setting("wall_loops"));
}

TEST_CASE("When two mods set one setting differently, the one used is reported", "[ModHost]")
{
    const Mod a = ModSet::parse_mod(R"({"id": "aaa", "version": "1", "params": {"on": {"type": "bool", "default": true, "invalidates": ["none"]}},
        "rules": [ {"target": "support_generator", "value": "\"cura\"", "when": "aaa.on", "priority": 0} ]})");
    const Mod b = ModSet::parse_mod(R"({"id": "bbb", "version": "1", "params": {"on": {"type": "bool", "default": true, "invalidates": ["none"]}},
        "rules": [ {"target": "support_generator", "value": "\"mixed\"", "when": "bbb.on", "priority": 2} ]})");
    const ModSet mods = ModSet::build({a, b});
    DynamicPrintConfig config = full_config();
    std::vector<ModSet::RuleClash> clashes;
    REQUIRE(mods.resolve(config, {}, &clashes).empty());
    CHECK(config.opt_string("support_generator") == "mixed");
    REQUIRE(clashes.size() == 1);
    CHECK(clashes[0].setting == "support_generator");
    CHECK(clashes[0].used_mod == "bbb");
    CHECK(clashes[0].used_value == "mixed");
    CHECK(clashes[0].other_mods == std::vector<std::string>{"aaa"});
    // Only one of them on: nothing to report.
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"aaa.on": false})"));
    clashes.clear();
    REQUIRE(mods.resolve(config, {}, &clashes).empty());
    CHECK(clashes.empty());
}

TEST_CASE("A package manifest is read and checked", "[ModHost]")
{
    const Mod mod = ModSet::parse_mod(R"({"id": "pkg", "version": "2.0.0",
        "api": {"major": 1, "min_minor": 0}, "binary": "pkg",
        "slots": [ {"slot": "layer.bands", "version": 1, "impl": "half"} ],
        "depends": [ {"id": "core", "min": "1.0"}, {"id": "other"} ], "conflicts": ["rival"], "thread_safe": true,
        "params": {"outer": {"type": "bool", "default": false, "scope": "object", "invalidates": ["posSlice"]}},
        "legacy_keys": {"outer_wall_half_layer_height": "outer"},
        "gui": {"paint_channels": [ {"id": "normal", "color": "#22AA44", "label": "Normal", "label_ko": "Ilban"} ],
                "object_params": ["outer"]}})");
    CHECK(mod.api_major == 1);
    CHECK(mod.binary == "pkg");
    REQUIRE(mod.slots.size() == 1);
    CHECK(mod.slots[0].slot == "layer.bands");
    CHECK(mod.depends.size() == 2);
    CHECK(mod.conflicts == std::vector<std::string>{"rival"});
    CHECK(mod.thread_safe);
    CHECK(mod.legacy_keys.at("outer_wall_half_layer_height") == "outer");
    REQUIRE(mod.gui.paint_channels.size() == 1);
    CHECK(mod.gui.paint_channels[0].labels.at("ko") == "Ilban");
    CHECK(mod.gui.object_params == std::vector<std::string>{"outer"});

    auto bad = [](const std::string &fields) { return ModSet::parse_mod(R"({"id": "m", "version": "1", )" + fields + "}"); };
    CHECK_THROWS_WITH(bad(R"("binary": "m")"), ContainsSubstring("must give the \"api\""));
    CHECK_THROWS_WITH(bad(R"("slots": [ {"slot": "s", "version": 1, "impl": "i"} ])"), ContainsSubstring("needs a \"binary\""));
    CHECK_THROWS_WITH(bad(R"("legacy_keys": {"old": "missing"})"), ContainsSubstring("must name a parameter"));
    CHECK_THROWS_WITH(bad(R"("gui": {"paint_channels": [ {"id": "c", "color": "green"} ]})"), ContainsSubstring("needs a \"color\""));
    CHECK_THROWS_WITH(bad(R"("params": {"p": {"type": "int", "default": 1}}, "gui": {"object_params": ["p"]})"),
                      ContainsSubstring("not an object or region parameter"));
    CHECK_THROWS_WITH(bad(R"("depends": [ {"id": "m"} ])"), ContainsSubstring("cannot depend on itself"));

    const Mod rival = ModSet::parse_mod(R"({"id": "rival", "version": "1"})");
    CHECK_THROWS_WITH(ModSet::build({mod, rival}), ContainsSubstring("conflicts with mod 'rival'"));
    CHECK(compare_versions("1.10.0", "1.9") > 0);
    CHECK(compare_versions("2", "2.0.0") == 0);
}
