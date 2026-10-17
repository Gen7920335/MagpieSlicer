#include "ModIndex.hpp"
#include "ModSet.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace Slic3r {
namespace ModHost {

using json = nlohmann::json;

const char *trust_name(ModTrust trust)
{
    switch (trust) {
    case ModTrust::Official: return "official";
    case ModTrust::Verified: return "verified";
    default: return "community";
    }
}

static std::string entry_string(const json &j, const char *key, bool required, size_t index)
{
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        if (required)
            throw ModError("mod index entry " + std::to_string(index) + " has no \"" + key + "\"");
        return {};
    }
    if (!it->is_string())
        throw ModError("mod index entry " + std::to_string(index) + ": \"" + key + "\" must be a string");
    return it->get<std::string>();
}

static bool is_mod_identifier(const std::string &id)
{
    if (id.empty() || std::isdigit(static_cast<unsigned char>(id.front())))
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

std::vector<ModIndexEntry> parse_mod_index(const std::string &json_text)
{
    json j;
    try {
        j = json::parse(json_text, nullptr, true, /* ignore_comments */ true);
    } catch (const json::parse_error &e) {
        throw ModError(std::string("mod index is not valid JSON: ") + e.what());
    }
    if (!j.is_object() || !j.contains("mods") || !j["mods"].is_array())
        throw ModError("mod index must be an object with a \"mods\" array");
    if (j.contains("version") && (!j["version"].is_number_integer() || j["version"].get<int>() != 1))
        throw ModError("unsupported mod index version");

    std::vector<ModIndexEntry> entries;
    std::set<std::string>      ids;
    size_t                     index = 0;
    for (const json &e : j["mods"]) {
        if (!e.is_object())
            throw ModError("mod index entry " + std::to_string(index) + " must be an object");
        ModIndexEntry entry;
        entry.id          = entry_string(e, "id", true, index);
        entry.name        = entry_string(e, "name", false, index);
        entry.version     = entry_string(e, "version", true, index);
        entry.description = entry_string(e, "description", false, index);
        entry.author      = entry_string(e, "author", false, index);
        entry.url         = entry_string(e, "url", true, index);
        entry.sha256      = boost::algorithm::to_lower_copy(entry_string(e, "sha256", true, index));
        const std::string trust = entry_string(e, "trust", false, index);
        if (trust.empty() || trust == "community")
            entry.trust = ModTrust::Community;
        else if (trust == "verified")
            entry.trust = ModTrust::Verified;
        else if (trust == "official")
            entry.trust = ModTrust::Official;
        else
            throw ModError("mod index entry " + entry.id + ": unknown trust \"" + trust + "\"");
        if (const auto tags = e.find("tags"); tags != e.end()) {
            if (!tags->is_array())
                throw ModError("mod index entry " + entry.id + ": \"tags\" must be an array");
            for (const json &t : *tags) {
                if (!t.is_string())
                    throw ModError("mod index entry " + entry.id + ": tags must be strings");
                entry.tags.push_back(t.get<std::string>());
            }
        }
        if (!is_mod_identifier(entry.id))
            throw ModError("mod index entry " + std::to_string(index) + ": id \"" + entry.id +
                           "\" must use only a-z, 0-9 and _ and not start with a digit");
        if (!ids.insert(entry.id).second)
            throw ModError("mod index lists \"" + entry.id + "\" twice");
        if (!(boost::starts_with(entry.url, "https://") || boost::starts_with(entry.url, "http://") ||
              boost::starts_with(entry.url, "file://")))
            throw ModError("mod index entry " + entry.id + ": url must be http(s) or file");
        if (entry.sha256.size() != 64 ||
            !std::all_of(entry.sha256.begin(), entry.sha256.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }))
            throw ModError("mod index entry " + entry.id + ": sha256 must be 64 hex digits");
        entries.push_back(std::move(entry));
        ++index;
    }
    return entries;
}

std::vector<ModIndexEntry> search_mod_index(const std::vector<ModIndexEntry> &entries, const std::string &query)
{
    std::vector<std::string> words;
    {
        std::istringstream in(boost::algorithm::to_lower_copy(query));
        for (std::string w; in >> w;)
            words.push_back(w);
    }
    std::vector<ModIndexEntry> found;
    for (const ModIndexEntry &e : entries) {
        std::string text = e.id + '\n' + e.name + '\n' + e.description + '\n' + e.author;
        for (const std::string &t : e.tags)
            text += '\n' + t;
        boost::algorithm::to_lower(text);
        if (std::all_of(words.begin(), words.end(), [&text](const std::string &w) { return text.find(w) != std::string::npos; }))
            found.push_back(e);
    }
    return found;
}

std::string sha256_hex(const std::string &data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), hash);
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(2 * SHA256_DIGEST_LENGTH);
    for (unsigned char b : hash) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

std::string install_mod(const ModIndexEntry &entry, const std::string &definition,
                        const boost::filesystem::path &mods_dir,
                        const std::vector<boost::filesystem::path> &checked_dirs)
{
    namespace fs = boost::filesystem;
    if (sha256_hex(definition) != entry.sha256)
        throw ModError("the downloaded file of \"" + entry.id + "\" does not match the index (SHA-256 differs)");
    Mod mod = ModSet::parse_mod(definition);
    if (mod.id != entry.id)
        throw ModError("the downloaded file defines \"" + mod.id + "\", not \"" + entry.id + "\"");

    // Load the existing mods without this id's installed copy and check the new one against them.
    std::vector<std::string> errors;
    std::vector<Mod>         mods;
    for (const fs::path &dir : checked_dirs) {
        if (!fs::is_directory(dir))
            continue;
        std::vector<fs::path> files;
        for (const fs::directory_entry &f : fs::directory_iterator(dir))
            if (fs::is_regular_file(f.path()) && f.path().extension() == ".json")
                files.push_back(f.path());
        std::sort(files.begin(), files.end());
        const bool installed_dir = fs::is_directory(mods_dir) && fs::equivalent(dir, mods_dir);
        for (const fs::path &file : files) {
            boost::nowide::ifstream in(file.string(), std::ios::binary);
            std::stringstream       text;
            text << in.rdbuf();
            try {
                Mod existing = ModSet::parse_mod(text.str());
                if (existing.id == mod.id) {
                    if (installed_dir)
                        continue; // the copy being replaced
                    throw ModError("\"" + mod.id + "\" is a bundled mod and cannot be replaced");
                }
                mods.push_back(std::move(existing));
            } catch (const ModError &) {
                if (installed_dir)
                    continue; // an installed file that already fails to load does not block others
                throw;
            }
        }
    }
    mods.push_back(mod);
    ModSet::build(std::move(mods)); // throws ModError on a conflict

    fs::create_directories(mods_dir);
    const fs::path target = mods_dir / (entry.id + ".json");
    const fs::path temp   = mods_dir / (entry.id + ".json.download");
    {
        boost::nowide::ofstream out(temp.string(), std::ios::binary | std::ios::trunc);
        out.write(definition.data(), std::streamsize(definition.size()));
        out.close();
        if (!out)
            throw ModError("could not write " + temp.string());
    }
    fs::rename(temp, target);
    return target.string();
}

std::vector<std::string> missing_mod_ids(const DynamicPrintConfig &config, const std::vector<std::string> &loaded_ids)
{
    std::set<std::string> missing;
    auto scan = [&](const std::string &text) {
        if (text.empty())
            return;
        json j;
        try {
            j = json::parse(text);
        } catch (const json::parse_error &) {
            return;
        }
        if (!j.is_object())
            return;
        for (const auto &item : j.items()) {
            const std::string &key = item.key();
            const size_t       dot = key.find('.');
            if (dot == std::string::npos || dot == 0)
                continue;
            const std::string id = key.substr(0, dot);
            if (std::find(loaded_ids.begin(), loaded_ids.end(), id) == loaded_ids.end())
                missing.insert(id);
        }
    };
    if (const auto *opt = config.option<ConfigOptionString>("mod_settings"))
        scan(opt->value);
    if (const auto *opt = config.option<ConfigOptionString>("printer_mod_settings"))
        scan(opt->value);
    if (const auto *opt = config.option<ConfigOptionStrings>("filament_mod_settings"))
        for (const std::string &text : opt->values)
            scan(text);
    return {missing.begin(), missing.end()};
}

} // namespace ModHost
} // namespace Slic3r
