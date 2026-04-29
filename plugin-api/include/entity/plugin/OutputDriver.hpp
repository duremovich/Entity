// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "Plugin.hpp"

#include <cstdint>
#include <string_view>

// Hot-path interface: pixel buffers leave the engine through one of these.
//
// Implementations are STATIC-LINK ONLY. Hot-path data crossing a dlopen
// boundary would either pay the bus's serialization tax (defeating the
// purpose) or expose an ABI-fragile C++ vtable that breaks the moment a
// plugin author rebuilds with a different STL. First-party Entity Pro
// drivers (NDI, SDI cards, vendor display SDKs) compile in alongside core.
//
// The interface here is a deliberate STUB. The first real implementation
// (NDI, in Phase D) will dictate the shape; locking it in now would just
// produce a guess we'd have to break later. When that lands:
//   - frame ingress (TextureRef + metadata)
//   - presentation timing / async completion
//   - device enumeration / hot-plug
// all get wired through here. Until then, plugins that want to drive
// outputs from day one should subscribe to the bus and operate at the
// `SetOutputEnabled` level instead.
//
// See plugin-api/CLAUDE.md for the boundary rules; OutputDriver is the
// one place where the plugin-api MAY include core types (`TextureRef`),
// strictly read-only.

namespace entity::plugin {

class OutputDriver {
public:
    virtual ~OutputDriver() = default;

    virtual std::string_view driverName() const noexcept = 0;
    virtual int              apiVersion() const noexcept { return PLUGIN_API_VERSION; }

    // Concrete hot-path methods (frame submit, present complete, …) land
    // when the first hot-path output driver does.
};

} // namespace entity::plugin
