#include "HotendPresetStore.hpp"

#include "Utils.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace Slic3r {

namespace fs = boost::filesystem;

HotendPresetStore::HotendPresetStore(fs::path directory) : m_directory(std::move(directory))
{
}

bool HotendPresetStore::is_valid_name(const std::string &name)
{
    if (name.empty() || name == "." || name == ".." || name.front() == ' ' || name.back() == ' ' || name.back() == '.')
        return false;

    static constexpr const char *invalid_chars = "<>:\"/\\|?*";
    return std::none_of(name.begin(), name.end(), [](unsigned char ch) {
        return ch < 32 || std::strchr(invalid_chars, static_cast<char>(ch)) != nullptr;
    });
}

fs::path HotendPresetStore::path_for_name(const std::string &name) const
{
    if (!is_valid_name(name))
        throw std::invalid_argument("Invalid hotend preset name.");
    return m_directory / fs::path(name + ".ini");
}

std::vector<std::string> HotendPresetStore::names() const
{
    std::vector<std::string> result;
    boost::system::error_code ec;
    if (!fs::exists(m_directory, ec) || ec)
        return result;

    for (fs::directory_iterator it(m_directory, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path &path = it->path();
        if (!fs::is_regular_file(path, ec) || ec || fs::is_symlink(path, ec) || ec)
            continue;
        if (boost::algorithm::iequals(path.extension().string(), ".ini"))
            result.emplace_back(path.stem().string());
    }
    if (ec)
        throw std::runtime_error("Failed to enumerate hotend presets: " + ec.message());

    std::sort(result.begin(), result.end(), [](const std::string &left, const std::string &right) {
        const std::string left_lower = boost::algorithm::to_lower_copy(left);
        const std::string right_lower = boost::algorithm::to_lower_copy(right);
        return left_lower == right_lower ? left < right : left_lower < right_lower;
    });
    return result;
}

bool HotendPresetStore::contains(const std::string &name) const
{
    boost::system::error_code ec;
    const fs::path path = path_for_name(name);
    return fs::is_regular_file(path, ec) && !ec && !fs::is_symlink(path, ec) && !ec;
}

void HotendPresetStore::save(const std::string &name, const DynamicPrintConfig &config) const
{
    validate_hotend_preset_config(config);
    const fs::path target = path_for_name(name);
    boost::system::error_code ec;
    fs::create_directories(m_directory, ec);
    if (ec)
        throw std::runtime_error("Failed to create the hotend preset directory: " + ec.message());

    const fs::path temporary = m_directory / fs::unique_path(".hotend-%%%%-%%%%-%%%%.tmp");
    try {
        config.save(temporary.string());
        if (const std::error_code rename_error = rename_file(temporary.string(), target.string()))
            throw std::runtime_error("Failed to replace the hotend preset: " + rename_error.message());
    } catch (...) {
        fs::remove(temporary, ec);
        throw;
    }
}

void HotendPresetStore::load(const std::string &name, DynamicPrintConfig &config) const
{
    const fs::path source = path_for_name(name);
    boost::system::error_code ec;
    if (!fs::is_regular_file(source, ec) || ec || fs::is_symlink(source, ec) || ec)
        throw std::runtime_error("Hotend preset does not exist.");
    config.load_from_ini(source.string(), ForwardCompatibilitySubstitutionRule::Disable);
    validate_hotend_preset_config(config);
}

} // namespace Slic3r
