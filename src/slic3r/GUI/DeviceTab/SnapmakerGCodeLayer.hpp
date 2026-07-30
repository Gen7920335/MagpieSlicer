#ifndef slic3r_GUI_SnapmakerGCodeLayer_hpp_
#define slic3r_GUI_SnapmakerGCodeLayer_hpp_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

struct SnapmakerGCodePoint
{
    double x {0.0};
    double y {0.0};
};

struct SnapmakerGCodeSegment
{
    SnapmakerGCodePoint from;
    SnapmakerGCodePoint to;
    unsigned tool {0};
    std::string object;
};

struct SnapmakerGCodeLayer
{
    int number {0};
    std::vector<SnapmakerGCodeSegment> segments;
};

class SnapmakerGCodeLayerCache
{
public:
    static std::shared_ptr<SnapmakerGCodeLayerCache> parse(std::string gcode);
    ~SnapmakerGCodeLayerCache();

    SnapmakerGCodeLayer layer(int one_based_layer) const;
    int layer_count() const;
    int indexed_layer_count() const;
    bool empty() const;

private:
    struct Impl;

    explicit SnapmakerGCodeLayerCache(std::string gcode);
    void build_index();

    std::unique_ptr<Impl> m_impl;
};

} // namespace GUI
} // namespace Slic3r

#endif
