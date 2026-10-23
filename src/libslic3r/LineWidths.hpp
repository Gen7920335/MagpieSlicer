#pragma once

// MAGPIE: line widths per physical nozzle. On a printer whose nozzles differ in size each of the 11 line width
// settings has a per-nozzle field (<key>_per_nozzle, mm per nozzle, 0 = the setting itself). This header holds the
// routing table that says, for every printed feature, which settings give its width and which filament setting
// picks the nozzle that prints it (Cura's limit_to_extruder, as data), and the one accessor every width read goes
// through. Precedence, for one setting on one nozzle: an object, part, modifier or layer range that sets the width
// > the per-nozzle field > the setting (an absolute width scaled to the nozzle, see
// scale_absolute_line_widths_to_nozzles). The first rule is applied when the configs are prepared
// (prepare_line_widths clears a per-nozzle field an override beats), the second here.

#include "Flow.hpp"
#include "PrintConfig.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Slic3r {

class Print;
class PrintObject;

// What an extrusion prints, for its line width and the nozzle that prints it.
enum class WidthFeature : uint8_t {
    OuterWall,
    InnerWall,
    SparseInfill,
    InternalSolidInfill,
    TopSurface,
    BottomSurface,
    SkinInfill,
    SkeletonInfill,
    Support,
    SupportInterface,
    Skirt,
    Brim,
    Count
};

// Where the nozzle printing a feature comes from.
enum class WidthNozzleSource : uint8_t {
    RegionFilament, // the region's filament setting `filament_key`
    ObjectFilament, // the object's filament setting `filament_key`
    PrintingTool,   // the tool the G-code prints it with (skirt, brim): the caller passes that tool's nozzle
};

struct WidthRoute
{
    WidthFeature                feature;
    WidthNozzleSource           nozzle_source;
    const char                 *filament_key;        // nullptr for PrintingTool
    ConfigOptionInt PrintRegionConfig::*region_filament; // filament_key of a RegionFilament route, else nullptr
    ConfigOptionInt PrintObjectConfig::*object_filament; // filament_key of an ObjectFilament route, else nullptr
    FlowRole                    auto_role;           // automatic width when every setting of the chain is 0
    bool                        first_layer_initial; // on the first layer initial_layer_line_width comes first
    std::array<const char *, 3> chain;               // width settings tried in order, nullptr ends it; 0 moves on
};

// Indexed by WidthFeature.
const std::array<WidthRoute, size_t(WidthFeature::Count)> &width_routes();
inline const WidthRoute &width_route(WidthFeature feature) { return width_routes()[size_t(feature)]; }

// The configs a width is read from, after prepare_line_widths(). object and region are nullptr where the feature
// has none (a skirt without objects; support has no region).
struct WidthConfigs
{
    const PrintConfig       *print  = nullptr;
    const PrintObjectConfig *object = nullptr;
    const PrintRegionConfig *region = nullptr;
};

// The filament printing a feature whose filament setting is filament_id (1-based). 0 is Orca's "default" of support,
// the tool that happens to be current, whose nozzle is unknown while the support is laid out. On a printer whose
// nozzles differ in size it is filament 1, the support's widths and layer heights being resolved on its nozzle; with
// one nozzle size 0 stays 0 (any tool).
int printing_filament(const PrintConfig &print, int filament_id);

// The filament printing support interface set to interface_filament, the support base being set to support_filament
// (1-based). A "default" interface (0) prints with the tool that just printed the base, so on a printer whose nozzles
// differ in size it is the base's filament (filament 1 when that is "default" too), and its widths are resolved there.
int support_interface_printing_filament(const PrintConfig &print, int interface_filament, int support_filament);

// The 0-based physical nozzle that prints filament filament_id (1-based), printing_filament() resolving 0.
// Out-of-range ids and 0 with one nozzle size give nozzle 0, the way nozzle_diameter_for_filament() reads
// nozzle_diameter.get_at().
size_t printing_nozzle_index(const PrintConfig &print, int filament_id, bool is_bbl_printer);

// The 0-based physical nozzle printing `feature`, by the routing table. Not for PrintingTool features.
size_t feature_printing_nozzle(WidthFeature feature, const WidthConfigs &configs, bool is_bbl_printer);

// What setting width_key means on 0-based physical nozzle `nozzle`: its per-nozzle value as an absolute width when
// that is above 0, else the setting itself (a percentage of the nozzle, an absolute width, or 0 for automatic).
// `config` holds width_key and its per-nozzle field. A missing field or a nozzle past its end reads as 0.
ConfigOptionFloatOrPercent line_width_setting_on_nozzle(const ConfigOptionResolver &config, const char *width_key, size_t nozzle);

// The width setting `feature` prints with on `nozzle`: the first setting of its chain that is not 0 on that nozzle
// (the first layer tries initial_layer_line_width first when the route says so); 0 (automatic) when none is set.
ConfigOptionFloatOrPercent feature_line_width(WidthFeature feature, const WidthConfigs &configs, size_t nozzle, bool first_layer);

// The flow of `feature` printed by `nozzle` (0-based) at height_mm, the automatic width included.
Flow feature_flow(WidthFeature feature, const WidthConfigs &configs, size_t nozzle, float height_mm, bool first_layer);

// The extrusion width of `feature` on `nozzle`, mm: feature_flow()'s width without the layer height.
double feature_line_width_mm(WidthFeature feature, const WidthConfigs &configs, size_t nozzle, bool first_layer);

// bridge_line_width on `nozzle`, mm, resolved on the float nozzle diameter a Flow keeps (what the bridge flows
// always used); 0 when it is not set (bridges then take the bridged feature's flow).
double bridge_line_width_mm(const PrintConfig &print, const PrintRegionConfig &region, size_t nozzle);

// What the per-nozzle cell (width_key, nozzle) of `config` falls back to when it is empty, mm: the width slicing then
// uses for the feature the setting is named for (an outer wall for line_width and, on the first layer, for
// initial_layer_line_width; internal solid infill for a bridge without its own width). `config` is a process config
// with the printer's nozzle_diameter and printer_variant. For the per-nozzle table of the settings tab.
double per_nozzle_line_width_fallback_mm(const DynamicPrintConfig &config, const char *width_key, size_t nozzle);

// Skirt and brim are printed by whichever tool visits them (Print.cpp). The skirt flow on `nozzle` (0-based): the
// Skirt route read on the print's first object. The brim flow of `object` on `nozzle`: the Brim route read on the
// object and its first region. Both at the skirt's first layer height.
Flow skirt_flow_on_nozzle(const Print &print, size_t nozzle);
Flow brim_flow_on_nozzle(const Print &print, const PrintObject &object, size_t nozzle);
// The tool (0-based filament) that prints the brim of `object`: the G-code prints it on the first visit of the
// object on its first layer, so the first tool of the first layer's tool order (the print's, or the object's own in
// a by-object print) that prints anything of that layer of the object. Needs the tool ordering of Print::process().
unsigned int brim_tool(const Print &print, const PrintObject &object);

} // namespace Slic3r
