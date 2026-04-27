#include "entity/render/RuntimeShaderCompiler.hpp"

#include <iostream>

namespace entity {

namespace {

// FNV-1a 64-bit. Stable across runs (only used for in-process cache, but the
// determinism makes it easier to reproduce a compile when a test fails).
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

void fnvUpdate(uint64_t& h, const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

void fnvUpdate(uint64_t& h, std::string_view sv) {
    fnvUpdate(h, sv.data(), sv.size());
}

void fnvUpdate(uint64_t& h, const std::wstring& ws) {
    fnvUpdate(h, ws.data(), ws.size() * sizeof(wchar_t));
}

uint64_t makeKey(std::string_view              source,
                 std::string_view              entry,
                 std::string_view              profile,
                 const std::vector<std::wstring>& includes,
                 const std::vector<std::wstring>& extras) {
    uint64_t h = kFnvOffset;
    fnvUpdate(h, source);
    fnvUpdate(h, "|", 1);
    fnvUpdate(h, entry);
    fnvUpdate(h, "|", 1);
    fnvUpdate(h, profile);
    fnvUpdate(h, "|", 1);
    for (const auto& inc : includes) {
        fnvUpdate(h, inc);
        fnvUpdate(h, ";", 1);
    }
    fnvUpdate(h, "|", 1);
    for (const auto& a : extras) {
        fnvUpdate(h, a);
        fnvUpdate(h, ";", 1);
    }
    return h;
}

std::wstring widen(std::string_view sv) {
    std::wstring out;
    out.reserve(sv.size());
    // OK for the ASCII subset we feed DXC (entry name + profile name).
    for (char c : sv) out.push_back(static_cast<wchar_t>(c));
    return out;
}

} // namespace

bool RuntimeShaderCompiler::ensureInstances() {
    if (m_initialized) return true;
    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils));
    if (FAILED(hr)) {
        std::cerr << "[RuntimeShaderCompiler] DxcCreateInstance(DxcUtils) failed hr=0x"
                  << std::hex << hr << std::dec << '\n';
        return false;
    }
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler));
    if (FAILED(hr)) {
        std::cerr << "[RuntimeShaderCompiler] DxcCreateInstance(DxcCompiler) failed hr=0x"
                  << std::hex << hr << std::dec << '\n';
        return false;
    }
    m_utils->CreateDefaultIncludeHandler(&m_includeHandler);
    m_initialized = true;
    return true;
}

RuntimeShaderCompiler::CompileResult RuntimeShaderCompiler::compile(
    std::string_view                 source,
    std::string_view                 entryPoint,
    std::string_view                 targetProfile,
    const std::vector<std::wstring>& includeDirs,
    const std::vector<std::wstring>& extraArgs) {
    CompileResult out;
    if (!ensureInstances()) {
        out.errors = "DXC instance creation failed (dxcompiler.dll missing?)";
        return out;
    }

    DxcBuffer buf{};
    buf.Ptr      = source.data();
    buf.Size     = source.size();
    buf.Encoding = DXC_CP_UTF8;

    // Build args. ComPtr-managed lifetimes: we keep wstring storage on the
    // stack; LPCWSTR* points into it.
    std::wstring wEntry   = widen(entryPoint);
    std::wstring wProfile = widen(targetProfile);

    std::vector<std::wstring> argStorage;
    argStorage.emplace_back(L"-T");
    argStorage.push_back(wProfile);
    argStorage.emplace_back(L"-E");
    argStorage.push_back(wEntry);
    argStorage.emplace_back(L"-HV");
    argStorage.emplace_back(L"2021");
    argStorage.emplace_back(L"-Zpc"); // matrix layout = column-major (matches offline build)
    for (const auto& dir : includeDirs) {
        argStorage.emplace_back(L"-I");
        argStorage.push_back(dir);
    }
    for (const auto& a : extraArgs) {
        argStorage.push_back(a);
    }

    std::vector<LPCWSTR> args;
    args.reserve(argStorage.size());
    for (const auto& s : argStorage) args.push_back(s.c_str());

    Microsoft::WRL::ComPtr<IDxcResult> result;
    HRESULT hr = m_compiler->Compile(&buf,
                                     args.data(),
                                     static_cast<UINT32>(args.size()),
                                     m_includeHandler.Get(),
                                     IID_PPV_ARGS(&result));
    if (FAILED(hr)) {
        out.errors = "IDxcCompiler3::Compile returned a failed HRESULT";
        return out;
    }

    // Capture diagnostics regardless of success (warnings show up here too).
    Microsoft::WRL::ComPtr<IDxcBlobUtf8> errBlob;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errBlob), nullptr)) &&
        errBlob && errBlob->GetStringLength() > 0) {
        out.errors.assign(errBlob->GetStringPointer(), errBlob->GetStringLength());
    }

    HRESULT compileStatus = S_OK;
    result->GetStatus(&compileStatus);
    if (FAILED(compileStatus)) {
        return out; // out.success stays false; errors holds the diag log
    }

    Microsoft::WRL::ComPtr<IDxcBlob> shaderBlob;
    hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);
    if (FAILED(hr) || !shaderBlob || shaderBlob->GetBufferSize() == 0) {
        if (out.errors.empty()) out.errors = "DXC produced no DXIL object";
        return out;
    }

    out.blob    = std::move(shaderBlob);
    out.success = true;
    return out;
}

RuntimeShaderCompiler::CompileResult RuntimeShaderCompiler::compileAndCache(
    std::string_view                 source,
    std::string_view                 entryPoint,
    std::string_view                 targetProfile,
    const std::vector<std::wstring>& includeDirs,
    const std::vector<std::wstring>& extraArgs) {
    const uint64_t key = makeKey(source, entryPoint, targetProfile, includeDirs, extraArgs);

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            // Return a copy: ComPtr ref-counts the blob, so callers can safely
            // hold it past a clearCache() invalidation on another thread.
            return it->second;
        }
    }

    CompileResult result = compile(source, entryPoint, targetProfile, includeDirs, extraArgs);

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        // Don't overwrite a concurrent successful compile that beat us in.
        auto [it, inserted] = m_cache.try_emplace(key, result);
        if (!inserted) return it->second;
    }
    return result;
}

void RuntimeShaderCompiler::clearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cache.clear();
}

size_t RuntimeShaderCompiler::cacheSize() const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_cache.size();
}

} // namespace entity
