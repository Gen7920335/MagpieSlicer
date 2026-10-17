#pragma once

#include "libslic3r/ModHost/ModIndex.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// MAGPIE: where the app looks for mods when the user has not set "mod_index_url".
// Placeholder for the official index repository (not published yet).
extern const char *const DEFAULT_MOD_INDEX_URL;

// Downloads `url` (http, https or file) on the calling thread. Throws std::runtime_error with the
// reason when the request fails or the body is larger than `size_limit` bytes.
std::string fetch_mod_url(const std::string &url, size_t size_limit);

// Downloads and parses the index at `url`. Throws std::runtime_error / ModHost::ModError.
std::vector<ModHost::ModIndexEntry> fetch_mod_index(const std::string &url);

// Downloads entry.url, checks it and installs it into <data_dir>/mods, then reloads the active
// mods. Returns the installed path. Throws on any failure, leaving the mods unchanged.
std::string download_and_install_mod(const ModHost::ModIndexEntry &entry);

} // namespace Slic3r
