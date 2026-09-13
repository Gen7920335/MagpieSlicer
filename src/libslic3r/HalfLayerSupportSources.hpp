#pragma once

#include "Layer.hpp"
#include "Print.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

namespace Slic3r {

// A logical support event owns no extrusion: physical geometry remains owned by
// PrintObject. This descriptor is valid only until support invalidation/clearing.
class HalfLayerSupportEvent final : public SupportLayer {
public:
    HalfLayerSupportEvent(size_t id, PrintObject *object, const LogicalBand &band) :
        SupportLayer(id, band.interface_id, object, band.height_mm, band.print_z_mm, -1.) {}

    std::vector<const SupportLayer *> physical_layers;
    bool has_extrusions() const override
    {
        return std::any_of(physical_layers.begin(), physical_layers.end(),
            [](const SupportLayer *layer) { return layer->has_extrusions(); });
    }
};

struct HalfLayerSupportSources {
    std::vector<std::unique_ptr<HalfLayerSupportEvent>> events;

    HalfLayerSupportSources(PrintObject *object, const SupportLayerPtrs &physical)
    {
        std::vector<SupportLayer::LogicalBand> bands;
        for (const SupportLayer *layer : physical) {
            if (layer->half_layer_parent_bands.empty())
                throw std::logic_error("Half-height support geometry has no logical owner");
            bands.insert(bands.end(), layer->half_layer_parent_bands.begin(), layer->half_layer_parent_bands.end());
        }
        std::sort(bands.begin(), bands.end(), [](const auto &a, const auto &b) {
            if (a.print_z_mm != b.print_z_mm) return a.print_z_mm < b.print_z_mm;
            if (a.height_mm != b.height_mm) return a.height_mm < b.height_mm;
            return a.interface_id < b.interface_id;
        });
        // Logical Z coalescing tolerance, mm, matching support layer installation.
        constexpr double z_tolerance_mm = 0.000001;
        for (const auto &band : bands) {
            if (!std::isfinite(band.print_z_mm) || !std::isfinite(band.height_mm) || band.height_mm <= 0.)
                throw std::logic_error("Invalid logical support interval");
            if (events.empty() || band.print_z_mm > events.back()->print_z + z_tolerance_mm)
                events.push_back(std::make_unique<HalfLayerSupportEvent>(events.size(), object, band));
        }
        for (const SupportLayer *layer : physical) {
            auto it = std::lower_bound(events.begin(), events.end(), layer->print_z - z_tolerance_mm,
                [](const auto &event, double z_mm) { return event->print_z < z_mm; });
            if (it == events.end())
                throw std::logic_error("Physical support exceeds its logical event grid");
            (*it)->physical_layers.push_back(layer);
            (*it)->support_type = layer->support_type;
        }
    }
};

// Logical event consumers use this view. Geometry consumers continue to use
// support_layers(); neither view copies or transfers extrusion ownership.
inline std::vector<const SupportLayer *> support_event_layers(const PrintObject &object)
{
    std::vector<const SupportLayer *> layers;
    const auto *sources = object.half_layer_support_sources();
    layers.reserve(sources == nullptr ? object.support_layers().size() : sources->events.size());
    if (sources != nullptr) {
        for (const auto &event : sources->events)
            layers.push_back(event.get());
    } else {
        for (const SupportLayer *layer : object.support_layers())
            layers.push_back(layer);
    }
    return layers;
}

} // namespace Slic3r
