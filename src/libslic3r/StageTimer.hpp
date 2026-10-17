#pragma once

// MAGPIE: per-function timing for deciding what to move to the GPU. Compiled in only with the
// MAGPIE_SLICING_TIMING CMake option; without it MAGPIE_STAGE_TIMER expands to nothing.
//
// Each named scope adds its elapsed time and one call to a process-wide total. Scopes running on
// several threads at once add up their own times, so a total is CPU-side time summed over threads,
// not wall time; wall time comes from the Print step scopes, which never overlap themselves.

#include <string>
#include <vector>

namespace Slic3r::StageTimer {

struct Total
{
    std::string name;
    double      seconds = 0.; // summed over calls and threads
    long long   calls   = 0;
};

#ifdef MAGPIE_SLICING_TIMING

void add(const char *name, double seconds);
std::vector<Total> totals();
void reset();
// Writes "name<TAB>seconds<TAB>calls" lines, sorted by name, to `path` (or stderr when empty).
void dump(const std::string &path);

class Scope
{
public:
    explicit Scope(const char *name);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    const char *m_name;
    long long   m_start_ns;
};

#define MAGPIE_STAGE_TIMER_CAT2(a, b) a##b
#define MAGPIE_STAGE_TIMER_CAT(a, b) MAGPIE_STAGE_TIMER_CAT2(a, b)
#define MAGPIE_STAGE_TIMER(name) ::Slic3r::StageTimer::Scope MAGPIE_STAGE_TIMER_CAT(magpie_stage_timer_, __LINE__)(name)

#else

#define MAGPIE_STAGE_TIMER(name) ((void)0)

#endif

} // namespace Slic3r::StageTimer
