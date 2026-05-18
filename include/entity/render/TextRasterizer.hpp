#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace entity {

struct TextRasterSpec {
    std::string  text;
    std::wstring fontFamily  = L"Segoe UI";
    float        fontSize    = 96.0f;  // device-independent pixels (1 DIP = 1 px at 96 DPI)
    bool         bold        = false;
    bool         italic      = false;
    // 0 = Leading, 1 = Center, 2 = Trailing  (maps to DWRITE_TEXT_ALIGNMENT)
    uint8_t      alignment   = 1;
    uint32_t     width       = 1920;
    uint32_t     height      = 1080;
    glm::vec4    color       = {1.f, 1.f, 1.f, 1.f};  // straight alpha [0,1]
};

// Output is BGRA premultiplied bytes (GUID_WICPixelFormat32bppPBGRA /
// DXGI_FORMAT_B8G8R8A8_UNORM). Phase 2 note: if uploadVideoTexture expects
// RGBA8_UNORM, swizzle B<->R before uploading.
struct TextRasterResult {
    std::vector<uint8_t> bgra;  // stride = width * 4
    uint32_t             width  = 0;
    uint32_t             height = 0;
};

class TextRasterizer {
public:
    TextRasterizer()  = default;
    ~TextRasterizer();  // releases COM factories + balances CoInitializeEx

    // Non-copyable (owns COM factory singletons)
    TextRasterizer(const TextRasterizer&)            = delete;
    TextRasterizer& operator=(const TextRasterizer&) = delete;

    // Rasterize text into a BGRA pixel buffer.
    // Thread-affinity: editor thread only (IWICImagingFactory is STA).
    // Lazy-initializes DWrite / D2D / WIC on first call (~20-80 ms).
    TextRasterResult rasterize(const TextRasterSpec& spec);

    // Return sorted, deduplicated list of system font family names (UTF-8).
    // Lazy-initializes DWrite on first call. Safe to cache in the UI.
    std::vector<std::string> enumerateSystemFonts();

private:
    bool ensureFactories();

    // COM factory pointers stored as void* to avoid pulling Windows headers
    // into every TU that includes this header.
    void* m_dwriteFactory = nullptr;
    void* m_d2dFactory    = nullptr;
    void* m_wicFactory    = nullptr;

    // Opaque pointer to a TextFormatCache (defined in TextRasterizer.cpp).
    // Caches IDWriteTextFormat objects keyed by (font, weight, style, size)
    // so we don't re-look-up the font face on every rasterize call —
    // CreateTextFormat is the most expensive call in the hot path.
    void* m_textFormatCache = nullptr;

    // S_OK  = we called CoInitializeEx and must balance with CoUninitialize.
    // S_FALSE = COM was already initialized on this thread; don't uninitialize.
    // E_FAIL (default) = CoInitializeEx not called yet.
    long  m_coInitHr   = 0x80004005L;  // E_FAIL
    bool  m_initialized = false;
    bool  m_initFailed  = false;
};

// UTF-8 string → wide string via MultiByteToWideChar (Win32 CP_UTF8).
// Handles non-ASCII font family names correctly. Safe to call from the
// editor thread. Declared here so callers (e.g. TextSystem) don't need
// a Windows header.
std::wstring utf8ToWide(const std::string& utf8);

} // namespace entity
