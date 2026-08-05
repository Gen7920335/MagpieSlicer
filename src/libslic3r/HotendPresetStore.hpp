#ifndef slic3r_HotendPresetStore_hpp_
#define slic3r_HotendPresetStore_hpp_

#include "HotendConfigService.hpp"

#include <boost/filesystem/path.hpp>

#include <array>
#include <string>
#include <vector>

namespace Slic3r {

class HotendPresetStore
{
public:
    explicit HotendPresetStore(boost::filesystem::path directory);

    static bool is_valid_name(const std::string &name);

    std::vector<std::string> names() const;
    bool                     contains(const std::string &name) const;
    void                     save(const std::string &name, const DynamicPrintConfig &config) const;
    void                     load(const std::string &name, DynamicPrintConfig &config) const;

private:
    boost::filesystem::path path_for_name(const std::string &name) const;

    boost::filesystem::path m_directory;
};

} // namespace Slic3r

#endif
