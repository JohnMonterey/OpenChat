#pragma once

#if defined(__x86_64__) || defined(__i386__)
#    define OPENCHAT_HAS_MXCSR 1
#    include <pmmintrin.h>
#    include <xmmintrin.h>
#else
#    define OPENCHAT_HAS_MXCSR 0
#endif

namespace OpenChat {

// Flush-to-zero and denormals-are-zero for the duration of a plugin's process
// call, restored on the way out.
//
// A denormal is a float so close to zero that the hardware handles it in
// microcode, at ten to a hundred times the cost of a normal one. That is a
// curiosity in most code and a real problem here, because of what sits directly
// upstream: MicrophoneProcessor's gate emits *literal digital silence* when it
// closes. A reverb, a filter or a compressor fed exact zeros decays smoothly
// down into the denormal range and then stays there for as long as the user is
// quiet -- so the slowdown lands precisely when nobody is talking, which is
// also when a glitch is most audible on the far end when they start again.
//
// Scoped rather than set once at startup because MXCSR is per-thread state and
// FTZ/DAZ are not IEEE 754. Leaving them on would silently change the
// arithmetic of every other thing that shares this thread -- the level meter,
// the jitter buffer's timing maths, anything in Qt -- and a subtly different
// rounding mode reached by "the user enabled voice effects" is not a defect
// anybody would find.
class ScopedNoDenormals final
{
public:
    ScopedNoDenormals() noexcept
    {
#if OPENCHAT_HAS_MXCSR
        m_saved = _mm_getcsr();
        // 0x8000 is FTZ. 0x0040 is DAZ, and it is guarded: DAZ arrived with
        // SSE3 and setting an unsupported MXCSR bit faults rather than being
        // ignored, so a build running on a pre-SSE3 machine would crash on the
        // first frame instead of merely being slow.
        unsigned int bits = m_saved | 0x8000u;
        if (__builtin_cpu_supports("sse3"))
            bits |= 0x0040u;
        _mm_setcsr(bits);
#endif
    }

    ~ScopedNoDenormals() noexcept
    {
#if OPENCHAT_HAS_MXCSR
        _mm_setcsr(m_saved);
#endif
    }

    ScopedNoDenormals(const ScopedNoDenormals &) = delete;
    ScopedNoDenormals &operator=(const ScopedNoDenormals &) = delete;

private:
#if OPENCHAT_HAS_MXCSR
    unsigned int m_saved = 0;
#endif
};

} // namespace OpenChat
