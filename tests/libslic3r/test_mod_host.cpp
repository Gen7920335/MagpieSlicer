#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libslic3r/ModHost/Expr.hpp>
#include <libslic3r/ModHost/ModSet.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <map>
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
        "params": { "gap": { "type": "float", "default": 1.5, "unit": "mm", "page": "Support", "invalidates": ["support"] } },
        /* one rule */
        "rules": [ { "target": "wall_loops", "value": "3", "when": "layer_height < 0.3", "priority": 10 } ]
    })");
    CHECK(mod.id == "demo");
    REQUIRE(mod.params.size() == 1);
    CHECK(mod.params[0].name == "gap");
    CHECK(std::get<double>(mod.params[0].default_value) == 1.5);
    CHECK(mod.params[0].invalidates == std::vector<std::string>{"support"});
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
