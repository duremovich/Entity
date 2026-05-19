// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace entity::dmx {

// One discovered FTDI VCP COM port that looks like a DMX-USB-Pro.
struct EnttecDevice {
    std::string portName;     // e.g. "COM5"
    std::string description;  // human-readable, includes "USB-DMX Pro" etc.
};

// Windows-only. Returns an empty vector on non-Windows or if no FTDI
// VCP ports are present. Filters to VID_0403&PID_6001 (FT232 generic);
// the caller can choose by description if multiple FTDI devices are
// plugged in.
std::vector<EnttecDevice> enumerateEnttecDevices();

// Write the discovered device list to %APPDATA%/Entity/dmx_enttec_devices.json
// for the SettingsWindow port dropdown to read (so the UI layer doesn't
// have to link the plugin).
void writeDeviceSidecar(const std::vector<EnttecDevice>& devices);

} // namespace entity::dmx
