// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "Plugin.hpp"

#include <cstdint>
#include <string_view>

// Hot-path interface: produce DecodedFrames from a byte stream. Like
// OutputDriver, this is STATIC-LINK ONLY -- a codec provider that crosses
// a dlopen boundary either pays the bus's serialization tax per frame
// (catastrophic at 4K60) or exposes a fragile C++ vtable. First-party
// Entity Pro codec providers (R3D, ARRI raw, Notch blocks) compile in
// alongside core.
//
// The interface here is a deliberate STUB. The first real implementation
// will dictate the shape -- in particular how DecodedFrame ownership and
// FrameCache integration work for plugin-produced frames. Locking that in
// before the first concrete consumer would be a guess.
//
// Plugins that want to add file-format support today should add a new
// FFmpeg-backed Decoder subclass under `src/media/` (still GPL core).
// CodecProvider is the eventual home for codec paths that can't reasonably
// live in the open-core repo (NDA-bound vendor SDKs, etc.).

namespace entity::plugin {

class CodecProvider {
public:
    virtual ~CodecProvider() = default;

    virtual std::string_view codecName() const noexcept = 0;
    virtual int              apiVersion() const noexcept { return PLUGIN_API_VERSION; }

    // Concrete hot-path methods (probe, open, decode-frame, seek, ...)
    // land when the first hot-path codec provider does.
};

} // namespace entity::plugin
