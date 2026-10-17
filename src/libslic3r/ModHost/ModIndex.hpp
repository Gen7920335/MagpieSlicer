#pragma once

#include <string>
#include <vector>

namespace boost { namespace filesystem { class path; } }

namespace Slic3r {

class DynamicPrintConfig;

namespace ModHost {

// How far an index vouches for a mod. Data mods can only set existing settings, so every level
// is installable; the level is shown so the user can judge who stands behind the mod.
enum class ModTrust { Community, Verified, Official };

const char *trust_name(ModTrust trust);

// One mod a mod index offers: where to get its definition and the SHA-256 of that file.
struct ModIndexEntry
{
    std::string              id;
    std::string              name;
    std::string              version;
    std::string              description;
    std::string              author;
    std::string              url;    // the mod definition (.json); http(s) or file
    std::string              sha256; // lower-case hex of the file at `url`
    ModTrust                 trust = ModTrust::Community;
    std::vector<std::string> tags;
};

// Parses an index: {"version": 1, "mods": [entry, ...]}. Throws ModError on a malformed index;
// an entry with a bad id, url or hash makes the whole index invalid rather than silently shorter.
std::vector<ModIndexEntry> parse_mod_index(const std::string &json_text);

// Entries whose id, name, description, author or a tag contains every word of `query`
// (case-insensitive), in index order. An empty query matches everything.
std::vector<ModIndexEntry> search_mod_index(const std::vector<ModIndexEntry> &entries, const std::string &query);

// Lower-case hex SHA-256 of `data`.
std::string sha256_hex(const std::string &data);

// Checks a downloaded definition against its index entry and installs it as <mods_dir>/<id>.json:
// the hash must match, the file must parse as a mod with the entry's id, and it must load together
// with the mods already in `checked_dirs` (the bundled and installed mods; the installed copy of
// this id is replaced, not checked against). Throws ModError and leaves the directory unchanged
// on any failure. Returns the installed path.
std::string install_mod(const ModIndexEntry &entry, const std::string &definition,
                        const boost::filesystem::path &mods_dir,
                        const std::vector<boost::filesystem::path> &checked_dirs);

// Mod ids a config's mod_settings, printer_mod_settings or filament_mod_settings refer to
// ("<id>.<param>" keys) that `loaded_ids` does not contain, sorted and unique. A project saved
// with a mod the user does not have.
std::vector<std::string> missing_mod_ids(const DynamicPrintConfig &config, const std::vector<std::string> &loaded_ids);

} // namespace ModHost
} // namespace Slic3r
