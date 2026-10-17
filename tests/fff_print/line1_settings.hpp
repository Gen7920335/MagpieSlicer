#pragma once

// MAGPIE: tests ported from Magpie line 1 set line 1's settings (support_type "resin(auto)",
// resin_support_tree_type, cura_support_join_distance, ...) through the same path a line 1
// project or preset takes when it loads: handle_legacy() records them, handle_legacy_composite()
// turns them into mod parameters, and the bundled mods apply them when the Print is set up.

#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

namespace Slic3r::Test {

// BundledMods (test_helpers.hpp) makes the bundled mods active.

// Sets one line 1 setting as loading a line 1 file would.
inline void line1(DynamicPrintConfig &config, const std::string &key, const std::string &value)
{
    ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Disable);
    config.set_deserialize(key, value, context);
    config.handle_legacy_composite();
}

inline void line1(DynamicPrintConfig &config, const std::string &key, const char *value) { line1(config, key, std::string(value)); }
inline void line1(DynamicPrintConfig &config, const std::string &key, bool value) { line1(config, key, std::string(value ? "1" : "0")); }

template<typename Number>
inline void line1(DynamicPrintConfig &config, const std::string &key, Number value)
{
    std::ostringstream text;
    text << value;
    line1(config, key, text.str());
}

// Sets a mod parameter in mod_settings, for line 1 behaviour that no line 1 key carries here (line 1
// read some Orca keys, such as tree_support_wall_count, differently from Orca).
template<typename Value>
inline void mod_param(DynamicPrintConfig &config, const std::string &param, const Value &value)
{
    nlohmann::json stored = nlohmann::json::object();
    if (const auto *opt = config.option<ConfigOptionString>("mod_settings"); opt && !opt->value.empty())
        stored = nlohmann::json::parse(opt->value);
    stored[param] = value;
    config.set_key_value("mod_settings", new ConfigOptionString(stored.dump()));
}

// Line 1's resin tree strategies, as its tests name them.
enum Line1ResinTree { rstDefault, rstBranching };
inline const char *line1_name(Line1ResinTree tree) { return tree == rstBranching ? "branching" : "default"; }

// Line 1's Mixed support choices.
enum Line1MixedNormal { mnsgPrusa, mnsgCura };
enum Line1MixedTree { mtssOrganic, mtssSlim, mtssStrong, mtssTreeHybrid };
inline const char *line1_name(Line1MixedNormal normal) { return normal == mnsgCura ? "cura" : "prusa"; }
inline const char *line1_name(Line1MixedTree tree)
{
    switch (tree) {
    case mtssSlim: return "tree_slim";
    case mtssStrong: return "tree_strong";
    case mtssTreeHybrid: return "tree_hybrid";
    default: return "organic";
    }
}

} // namespace Slic3r::Test
