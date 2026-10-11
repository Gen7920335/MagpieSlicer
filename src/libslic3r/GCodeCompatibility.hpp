#ifndef slic3r_GCodeCompatibility_hpp_
#define slic3r_GCodeCompatibility_hpp_

#include "libslic3r_version.h"

namespace Slic3r {

// Producer name written to and recognized in G-code headers. Printers and host
// software identify Orca output by it, so it stays independent of SLIC3R_APP_NAME.
inline constexpr const char *GCODE_COMPATIBLE_PRODUCER = SLIC3R_GCODE_GENERATOR_NAME;

} // namespace Slic3r

#endif
