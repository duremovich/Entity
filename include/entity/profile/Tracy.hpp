#pragma once

// Single include for Tracy macros across the codebase.
//
// Disable via either:
//   - CMake -DENTITY_ENABLE_TRACY=OFF     (dependency dropped entirely)
//   - #define ENTITY_PROFILE_DISABLED      (per-translation-unit opt-out)
//
// When disabled all macros become no-ops with zero overhead. See
// docs/adr/0015-profiling-with-tracy.md for the rationale.

#if defined(ENTITY_ENABLE_TRACY) && !defined(ENTITY_PROFILE_DISABLED)
    #include <tracy/Tracy.hpp>
    #include <tracy/TracyD3D12.hpp>
#else
    // Local stubs so files including this header compile when Tracy
    // isn't on the include path at all.
    #define ZoneScoped
    #define ZoneScopedN(x)
    #define ZoneScopedC(x)
    #define ZoneScopedNC(x, y)
    #define ZoneText(x, y)
    #define ZoneName(x, y)
    #define ZoneColor(x)
    #define ZoneValue(x)
    #define FrameMark
    #define FrameMarkNamed(x)
    #define FrameMarkStart(x)
    #define FrameMarkEnd(x)
    #define TracyPlot(x, y)
    #define TracyPlotConfig(x, y, z, w, a)
    #define TracyMessage(x, y)
    #define TracyMessageL(x)
    #define TracyAlloc(x, y)
    #define TracyFree(x)
    #define TracyLockable(type, varname) type varname
    #define TracyLockableN(type, varname, desc) type varname
    #define LockableBase(type) type
    #define TracyAppInfo(x, y)
    #define TracySetProgramName(x)

    // Type alias so disabled-path code can declare `TracyD3D12Ctx ctx`
    // members without including <tracy/TracyD3D12.hpp>.
    using TracyD3D12Ctx = void*;

    #define TracyD3D12Context(device, queue) (nullptr)
    #define TracyD3D12NewFrame(ctx)
    #define TracyD3D12Zone(ctx, list, name)
    #define TracyD3D12ZoneC(ctx, list, name, color)
    #define TracyD3D12Collect(ctx)
    #define TracyD3D12Destroy(ctx)
    #define TracyD3D12ContextName(ctx, name, size)

    namespace tracy {
        inline void SetThreadName(const char*) {}
    }
#endif
