#pragma once

#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

namespace ModHost {

// MAGPIE: settings written by Magpie line 1 (2.5.0.0.x) that this build drives through mods.
//
// Loading a line 1 project or preset passes every key through stash_legacy_setting() before the
// key is looked up. It records the line 1 value of each key a mod now owns, drops the keys this
// build does not define, and rewrites the enum values only line 1 had (support_type
// "normal_cura(auto)", "mixed(auto)", "resin(auto)"; support_interface_pattern "triangles") to
// their Orca base value. After the whole file is loaded, migrate_legacy_settings() turns the
// recorded values into mod parameters in mod_settings, so the mods reproduce what line 1 printed.

// Returns true when `opt_key` was a line 1 setting; `opt_key` is cleared when this build has no
// such setting, and `value` is rewritten when line 1 used an enum value this build lacks.
bool stash_legacy_setting(std::string &opt_key, std::string &value, std::map<std::string, std::string> &stash);

struct LegacyMigration
{
    std::vector<std::string> migrated; // "<mod>.<param>" values written to mod_settings
    std::vector<std::string> dropped;  // line 1 settings this build cannot reproduce
};

// Writes the mod parameters the stashed values imply into the config's mod_settings, keeping any
// value mod_settings already has, and only where the value differs from the parameter's default.
// Clears `stash`.
LegacyMigration migrate_legacy_settings(DynamicPrintConfig &config, std::map<std::string, std::string> &stash);

} // namespace ModHost
} // namespace Slic3r
