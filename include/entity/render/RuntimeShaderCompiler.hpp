#pragma once

#include <wrl/client.h>
#include <d3d12.h>
#include <dxcapi.h>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace entity {

/**
 * RuntimeShaderCompiler — in-memory HLSL → DXIL via dxcompiler.dll.
 *
 * Phase C.12 #2. The offline DXC build (apps/CMakeLists.txt) handles all the
 * regular pixel/vertex shaders that don't depend on runtime state; this is
 * the path for OCIO-generated shader fragments where the HLSL source is
 * derived from the loaded OCIO config. Subtasks 4-5 splice the OCIO-emitted
 * function source into our existing pixel shaders and recompile them via
 * compileAndCache().
 *
 * Threading: the compile cache is guarded by a mutex. Compiling the same
 * shader from multiple threads collapses to one compile + N cache hits.
 *
 * DLL dependency: dxcompiler.dll + dxil.dll must sit alongside the host
 * exe (the build's POST_BUILD copy in apps/CMakeLists.txt handles this).
 *
 * Lifecycle: construct once per process. createInstances() is called lazily
 * on the first compile so test-time instantiation doesn't pay DXC startup
 * cost unless a test actually compiles something.
 */
class RuntimeShaderCompiler {
public:
    RuntimeShaderCompiler() = default;
    ~RuntimeShaderCompiler() = default;

    RuntimeShaderCompiler(const RuntimeShaderCompiler&)            = delete;
    RuntimeShaderCompiler& operator=(const RuntimeShaderCompiler&) = delete;

    struct CompileResult {
        Microsoft::WRL::ComPtr<IDxcBlob> blob;
        std::string                      errors;
        bool                             success{false};

        // Convenience: hand directly to D3D12_GRAPHICS_PIPELINE_STATE_DESC.PS / VS.
        D3D12_SHADER_BYTECODE asBytecode() const {
            if (!blob) return {nullptr, 0};
            return {blob->GetBufferPointer(), blob->GetBufferSize()};
        }
    };

    /**
     * Compile HLSL source. `targetProfile` follows DXC convention
     * ("ps_6_0", "vs_6_0", etc.). `includeDirs` is forwarded as `-I` flags;
     * leave empty if the source is self-contained.
     *
     * Bypasses the cache. Use compileAndCache() for repeat compiles.
     */
    CompileResult compile(std::string_view              source,
                          std::string_view              entryPoint,
                          std::string_view              targetProfile,
                          const std::vector<std::wstring>& includeDirs = {},
                          const std::vector<std::wstring>& extraArgs   = {});

    /**
     * Compile-with-cache. Cache key is derived from (source, entryPoint,
     * targetProfile, includeDirs, extraArgs). On hit returns the previously-
     * compiled blob without re-running DXC. On miss compiles + caches.
     *
     * Returned `errors` is empty on cache-hit (the original error log from
     * a failing compile is also cached, so a previously-failed compile
     * stays failed on subsequent calls).
     */
    CompileResult compileAndCache(std::string_view              source,
                                  std::string_view              entryPoint,
                                  std::string_view              targetProfile,
                                  const std::vector<std::wstring>& includeDirs = {},
                                  const std::vector<std::wstring>& extraArgs   = {});

    /** Drop the entire cache. Call after Settings.ocioConfigPath changes. */
    void clearCache();

    size_t cacheSize() const;

private:
    bool ensureInstances();

    Microsoft::WRL::ComPtr<IDxcUtils>          m_utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3>      m_compiler;
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> m_includeHandler;
    bool                                       m_initialized{false};

    mutable std::mutex                                  m_cacheMutex;
    std::unordered_map<uint64_t, CompileResult>         m_cache;
};

} // namespace entity
