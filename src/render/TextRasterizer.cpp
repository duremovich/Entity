#include "entity/render/TextRasterizer.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <dwrite.h>
#include <d2d1.h>
#include <wincodec.h>

#include <algorithm>
#include <iostream>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace entity {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), wlen);
    wide.resize(wlen - 1);  // strip trailing null
    return wide;
}

namespace {

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
    out.resize(len - 1);
    return out;
}

DWRITE_TEXT_ALIGNMENT toDWriteAlignment(uint8_t alignment) {
    switch (alignment) {
        case 0: return DWRITE_TEXT_ALIGNMENT_LEADING;
        case 2: return DWRITE_TEXT_ALIGNMENT_TRAILING;
        default: return DWRITE_TEXT_ALIGNMENT_CENTER;
    }
}

// Cache key: (family, weight, style, size-as-quantized-int). Size is
// quantized to 0.1pt to avoid float-equality issues for keys.
struct TextFormatKey {
    std::wstring family;
    int          weight;       // DWRITE_FONT_WEIGHT
    int          style;        // DWRITE_FONT_STYLE
    int          sizeTenths;   // fontSize * 10, rounded
    bool operator==(const TextFormatKey& other) const {
        return weight == other.weight && style == other.style
            && sizeTenths == other.sizeTenths && family == other.family;
    }
};
struct TextFormatKeyHash {
    std::size_t operator()(const TextFormatKey& k) const noexcept {
        std::size_t h = std::hash<std::wstring>()(k.family);
        h = h * 31u + std::hash<int>()(k.weight);
        h = h * 31u + std::hash<int>()(k.style);
        h = h * 31u + std::hash<int>()(k.sizeTenths);
        return h;
    }
};

// Cache map: TextRasterizer owns one of these by void*; we cast when used.
// IDWriteTextFormat is shareable across rasterize calls; alignment is set
// per-call (it's mutable state on the format, not part of the key).
struct TextFormatCache {
    std::unordered_map<TextFormatKey, ComPtr<IDWriteTextFormat>, TextFormatKeyHash> map;
};

} // namespace

// ---------------------------------------------------------------------------
// TextRasterizer
// ---------------------------------------------------------------------------

TextRasterizer::~TextRasterizer() {
    // Drop cached TextFormat refs before the DWrite factory so they don't
    // outlive the factory they came from.
    if (m_textFormatCache) {
        delete static_cast<TextFormatCache*>(m_textFormatCache);
        m_textFormatCache = nullptr;
    }
    if (m_wicFactory)    { static_cast<IWICImagingFactory*>(m_wicFactory)->Release();    m_wicFactory    = nullptr; }
    if (m_d2dFactory)    { static_cast<ID2D1Factory*>(m_d2dFactory)->Release();          m_d2dFactory    = nullptr; }
    if (m_dwriteFactory) { static_cast<IDWriteFactory*>(m_dwriteFactory)->Release();     m_dwriteFactory = nullptr; }
    // Balance CoInitializeEx only if we were the one who initialized it (S_OK, not S_FALSE).
    if (m_coInitHr == S_OK) CoUninitialize();
}

bool TextRasterizer::ensureFactories() {
    if (m_initialized) return true;
    if (m_initFailed)  return false;

    // DWrite — does NOT require CoInitializeEx
    ComPtr<IDWriteFactory> dwrite;
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[TextRasterizer] DWriteCreateFactory failed: 0x" << std::hex << hr << "\n";
        m_initFailed = true;
        return false;
    }

    // D2D — does NOT require CoInitializeEx
    ComPtr<ID2D1Factory> d2d;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, d2d.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[TextRasterizer] D2D1CreateFactory failed: 0x" << std::hex << hr << "\n";
        m_initFailed = true;
        return false;
    }

    // WIC — needs COM STA apartment.  Initialize here so TextRasterizer is
    // self-contained and doesn't rely on the editor's startup sequence.
    // S_OK   = we initialized; destructor must CoUninitialize.
    // S_FALSE = already initialized on this thread; don't uninitialize.
    m_coInitHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(m_coInitHr)) {
        std::cerr << "[TextRasterizer] CoInitializeEx failed: 0x" << std::hex << m_coInitHr << "\n";
        m_initFailed = true;
        return false;
    }

    ComPtr<IWICImagingFactory> wic;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(wic.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[TextRasterizer] IWICImagingFactory CoCreateInstance failed: 0x"
                  << std::hex << hr << "\n";
        m_initFailed = true;
        return false;
    }

    // CopyTo AddRefs; ComPtr locals Release when they go out of scope, leaving
    // net ref count = 1 on each factory for the lifetime of this object.
    dwrite.CopyTo(reinterpret_cast<IDWriteFactory**>(&m_dwriteFactory));
    d2d.CopyTo(reinterpret_cast<ID2D1Factory**>(&m_d2dFactory));
    wic.CopyTo(reinterpret_cast<IWICImagingFactory**>(&m_wicFactory));

    m_initialized = true;
    return true;
}

TextRasterResult TextRasterizer::rasterize(const TextRasterSpec& spec) {
    TextRasterResult result;
    result.width  = spec.width;
    result.height = spec.height;

    if (spec.width == 0 || spec.height == 0) return result;
    if (!ensureFactories()) return result;

    auto* dwrite = static_cast<IDWriteFactory*>(m_dwriteFactory);
    auto* d2d    = static_cast<ID2D1Factory*>(m_d2dFactory);
    auto* wic    = static_cast<IWICImagingFactory*>(m_wicFactory);

    // --- WIC bitmap (BGRA premultiplied) ---
    ComPtr<IWICBitmap> wicBitmap;
    HRESULT hr = wic->CreateBitmap(spec.width, spec.height,
                                   GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapCacheOnDemand,
                                   wicBitmap.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[TextRasterizer] CreateBitmap failed: 0x" << std::hex << hr << "\n";
        return result;
    }

    // --- D2D render target ---
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);  // explicit DPI so 1 DIP = 1 pixel

    ComPtr<ID2D1RenderTarget> rt;
    hr = d2d->CreateWicBitmapRenderTarget(wicBitmap.Get(), rtProps, rt.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[TextRasterizer] CreateWicBitmapRenderTarget failed: 0x"
                  << std::hex << hr << "\n";
        return result;
    }

    // --- DWrite text format (cached by font, weight, style, size) ---
    if (!m_textFormatCache) m_textFormatCache = new TextFormatCache();
    auto* cache = static_cast<TextFormatCache*>(m_textFormatCache);

    const int weight = spec.bold   ? DWRITE_FONT_WEIGHT_BOLD  : DWRITE_FONT_WEIGHT_NORMAL;
    const int style  = spec.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    TextFormatKey key{
        spec.fontFamily,
        weight,
        style,
        static_cast<int>(spec.fontSize * 10.0f + 0.5f)
    };

    ComPtr<IDWriteTextFormat> textFormat;
    auto it = cache->map.find(key);
    if (it != cache->map.end()) {
        textFormat = it->second;
    } else {
        hr = dwrite->CreateTextFormat(
            spec.fontFamily.c_str(),
            nullptr,
            static_cast<DWRITE_FONT_WEIGHT>(weight),
            static_cast<DWRITE_FONT_STYLE>(style),
            DWRITE_FONT_STRETCH_NORMAL,
            spec.fontSize,
            L"en-us",
            textFormat.GetAddressOf());
        if (FAILED(hr)) {
            // Fall back to Arial if the requested font is unavailable
            hr = dwrite->CreateTextFormat(
                L"Arial", nullptr,
                static_cast<DWRITE_FONT_WEIGHT>(weight),
                static_cast<DWRITE_FONT_STYLE>(style),
                DWRITE_FONT_STRETCH_NORMAL,
                spec.fontSize, L"en-us",
                textFormat.GetAddressOf());
            if (FAILED(hr)) {
                std::cerr << "[TextRasterizer] CreateTextFormat failed (even Arial): 0x"
                          << std::hex << hr << "\n";
                return result;
            }
        }
        cache->map.emplace(key, textFormat);
    }

    (void)textFormat->SetTextAlignment(toDWriteAlignment(spec.alignment));
    // Multi-line wrap is the DWrite default (DWRITE_WORD_WRAPPING_WRAP) — nothing extra needed.

    // --- DWrite text layout ---
    std::wstring wtext = utf8ToWide(spec.text);
    ComPtr<IDWriteTextLayout> textLayout;
    hr = dwrite->CreateTextLayout(
        wtext.empty() ? L" " : wtext.data(),
        static_cast<UINT32>(wtext.empty() ? 1 : wtext.size()),
        textFormat.Get(),
        static_cast<float>(spec.width),
        static_cast<float>(spec.height),
        textLayout.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[TextRasterizer] CreateTextLayout failed: 0x" << std::hex << hr << "\n";
        return result;
    }

    // --- Draw ---
    // D2D1_COLOR_F takes straight alpha; D2D handles premul conversion to the target.
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
    hr = rt->CreateSolidColorBrush(
        D2D1::ColorF(spec.color.r, spec.color.g, spec.color.b, spec.color.a),
        brush.GetAddressOf());
    if (SUCCEEDED(hr)) {
        rt->DrawTextLayout(D2D1::Point2F(0.f, 0.f), textLayout.Get(), brush.Get());
    }
    hr = rt->EndDraw();

    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        // WIC render targets don't truly need device recreation, but guard anyway.
        std::cerr << "[TextRasterizer] EndDraw returned D2DERR_RECREATE_TARGET (unexpected for WIC RT)\n";
        return result;
    }
    if (FAILED(hr)) {
        std::cerr << "[TextRasterizer] EndDraw failed: 0x" << std::hex << hr << "\n";
        return result;
    }

    // --- Readback ---
    const UINT32 stride  = spec.width * 4;
    const UINT32 bufSize = stride * spec.height;
    result.bgra.resize(bufSize);
    hr = wicBitmap->CopyPixels(nullptr, stride, bufSize, result.bgra.data());
    if (FAILED(hr)) {
        std::cerr << "[TextRasterizer] CopyPixels failed: 0x" << std::hex << hr << "\n";
        result.bgra.clear();
    }
    return result;
}

std::vector<std::string> TextRasterizer::enumerateSystemFonts() {
    if (!ensureFactories()) return {};

    auto* dwrite = static_cast<IDWriteFactory*>(m_dwriteFactory);

    ComPtr<IDWriteFontCollection> collection;
    HRESULT hr = dwrite->GetSystemFontCollection(collection.GetAddressOf(), FALSE);
    if (FAILED(hr)) {
        std::cerr << "[TextRasterizer] GetSystemFontCollection failed: 0x"
                  << std::hex << hr << "\n";
        return {};
    }

    UINT32 familyCount = collection->GetFontFamilyCount();
    std::vector<std::string> names;
    names.reserve(familyCount);

    wchar_t localeName[LOCALE_NAME_MAX_LENGTH];
    GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH);

    for (UINT32 i = 0; i < familyCount; ++i) {
        ComPtr<IDWriteFontFamily> family;
        if (FAILED(collection->GetFontFamily(i, family.GetAddressOf()))) continue;

        ComPtr<IDWriteLocalizedStrings> nameStrings;
        if (FAILED(family->GetFamilyNames(nameStrings.GetAddressOf()))) continue;

        UINT32 index = 0;
        BOOL   exists = FALSE;
        nameStrings->FindLocaleName(localeName, &index, &exists);
        if (!exists) nameStrings->FindLocaleName(L"en-us", &index, &exists);
        if (!exists) index = 0;

        UINT32 len = 0;
        nameStrings->GetStringLength(index, &len);
        std::wstring wname(len + 1, L'\0');
        nameStrings->GetString(index, wname.data(), len + 1);
        wname.resize(len);

        names.push_back(wideToUtf8(wname));
    }

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

} // namespace entity
