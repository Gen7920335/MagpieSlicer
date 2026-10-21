// MAGPIE: the one place a call into a native mod runs under structured exception handling.
//
// MSVC refuses __try in a function that has C++ objects to unwind (C2712), so this file holds one
// function with none: it takes a plain function pointer and a context pointer. A hardware exception in
// the mod (an access violation, a divide by zero, a stack overflow) lands in the __except block, and
// NativeMod::guarded_call turns it into a slicing error and disables the mod for the rest of the run.
// Elsewhere than Windows the call runs unguarded.
#include <cstdint>

#ifdef _WIN32
#include <excpt.h>
#include <windows.h>
#endif

extern "C" {

typedef int32_t (*magpie_guarded_fn)(void *context);

// Calls fn(context). Returns its result, or sets *code_out and returns INT32_MIN when the call
// raised a structured exception.
int32_t magpie_native_guarded_call(magpie_guarded_fn fn, void *context, uint32_t *code_out)
{
#ifdef _WIN32
    __try {
        return fn(context);
    } __except (*code_out = static_cast<uint32_t>(GetExceptionCode()),
                // A C++ exception (MSVC raises it as this SEH code) is the core's own, thrown by the call's host side
                // (a cancel, a slicing error): it keeps unwinding to its handler instead of counting as a crash that
                // disables the mod (review 2026-10-22).
                *code_out == 0xE06D7363u ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) {
        return INT32_MIN;
    }
#else
    (void) code_out;
    return fn(context);
#endif
}

} // extern "C"
