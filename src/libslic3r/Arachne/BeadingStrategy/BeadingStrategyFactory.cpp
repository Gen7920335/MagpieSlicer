//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "BeadingStrategyFactory.hpp"

#include <boost/log/trivial.hpp>
#include <memory>
#include <utility>
#include <stdexcept>

#include "LimitedBeadingStrategy.hpp"
#include "WideningBeadingStrategy.hpp"
#include "DistributedBeadingStrategy.hpp"
#include "RedistributeBeadingStrategy.hpp"
#include "OuterWallInsetBeadingStrategy.hpp"
#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"

namespace Slic3r::Arachne {

BeadingStrategyPtr BeadingStrategyFactory::makeStrategy(const coord_t preferred_bead_width_outer,
                                                        const coord_t preferred_bead_width_inner,
                                                        const coord_t preferred_transition_length,
                                                        const float   transitioning_angle,
                                                        const bool    print_thin_walls,
                                                        const coord_t min_bead_width,
                                                        const coord_t min_feature_size,
                                                        const double  wall_split_middle_threshold,
                                                        const double  wall_add_middle_threshold,
                                                        const coord_t max_bead_count,
                                                        const coord_t outer_wall_offset,
                                                        const int     inward_distributed_center_wall_count,
                                                        const double  minimum_variable_line_ratio,
                                                         const size_t  fixed_outer_wall_count,
                                                         const coord_t fixed_outer_wall_boundary_overlap,
                                                         const std::vector<coord_t>& fixed_outer_wall_spacings,
                                                         const std::vector<coord_t>& fixed_outer_wall_overlaps)
{
    // Handle a special case when there is just one external perimeter.
    // Because big differences in bead width for inner and other perimeters cause issues with current beading strategies.
    const coord_t      optimal_width = max_bead_count <= 2 ? preferred_bead_width_outer : preferred_bead_width_inner;
    BeadingStrategyPtr ret = std::make_unique<DistributedBeadingStrategy>(optimal_width, preferred_transition_length, transitioning_angle,
                                                                          wall_split_middle_threshold, wall_add_middle_threshold,
                                                                          inward_distributed_center_wall_count);

    if (fixed_outer_wall_spacings.size() != fixed_outer_wall_overlaps.size())
        throw std::invalid_argument("Wall spacing and overlap profiles must have equal length");
    const size_t outer_shell_count = fixed_outer_wall_spacings.empty() ?
        (fixed_outer_wall_count == 0 ? 1 : fixed_outer_wall_count) : fixed_outer_wall_spacings.size();
    BOOST_LOG_TRIVIAL(trace) << "Applying " << outer_shell_count
                             << " Redistribute meta-strategy shell(s) with outer-wall width = " << preferred_bead_width_outer
                             << ", inner-wall width = " << preferred_bead_width_inner << ".";
    for (size_t shell_idx = 0; shell_idx < outer_shell_count; ++shell_idx) {
        // Each wrapper adds one outer shell, so consume the profile inside-out.
        const size_t depth = outer_shell_count - shell_idx - 1;
        const coord_t spacing = fixed_outer_wall_spacings.empty() ? preferred_bead_width_outer : fixed_outer_wall_spacings[depth];
        const coord_t boundary_overlap = fixed_outer_wall_spacings.empty() ?
            (shell_idx == 0 ? fixed_outer_wall_boundary_overlap : 0) : fixed_outer_wall_overlaps[depth];
        // Positive spacing and nonnegative overlap, both in scaled mm.
        if (!fixed_outer_wall_spacings.empty() && (spacing <= 0 || boundary_overlap < 0 || boundary_overlap >= spacing))
            throw std::invalid_argument("Invalid wall spacing profile");
        ret = std::make_unique<RedistributeBeadingStrategy>(spacing, minimum_variable_line_ratio,
                                                            boundary_overlap, std::move(ret));
    }

    if (print_thin_walls) {
        BOOST_LOG_TRIVIAL(trace) << "Applying the Widening Beading meta-strategy with minimum input width " << min_feature_size << " and minimum output width " << min_bead_width << ".";
        ret = std::make_unique<WideningBeadingStrategy>(std::move(ret), min_feature_size, min_bead_width);
    }
    // Orca: we allow negative outer_wall_offset here
    if (outer_wall_offset != 0) {
        BOOST_LOG_TRIVIAL(trace) << "Applying the OuterWallOffset meta-strategy with offset = " << outer_wall_offset << ".";
        ret = std::make_unique<OuterWallInsetBeadingStrategy>(outer_wall_offset, std::move(ret));
    }

    // Apply the LimitedBeadingStrategy last, since that adds a 0-width marker wall which other beading strategies shouldn't touch.
    BOOST_LOG_TRIVIAL(trace) << "Applying the Limited Beading meta-strategy with maximum bead count = " << max_bead_count << ".";
    ret = std::make_unique<LimitedBeadingStrategy>(max_bead_count, std::move(ret));
    return ret;
}
} // namespace Slic3r::Arachne
