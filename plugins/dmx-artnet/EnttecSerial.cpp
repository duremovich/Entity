// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "EnttecSerial.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <setupapi.h>
  #include <devguid.h>
  #include <regstr.h>
#endif

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace entity::dmx {

namespace {

#ifdef _WIN32
// Strip down a SETUPAPI hardware-id string (e.g.
// "FTDIBUS\COMPORT&VID_0403&PID_6001") to a VID-PID-only check.
bool isFtdiHardwareId(const std::string& s) {
    return s.find("VID_0403") != std::string::npos &&
           s.find("PID_6001") != std::string::npos;
}

std::string getStringProperty(HDEVINFO devInfo, SP_DEVINFO_DATA& devData,
                              DWORD prop) {
    DWORD bytes = 0;
    SetupDiGetDeviceRegistryPropertyA(devInfo, &devData, prop, nullptr,
                                       nullptr, 0, &bytes);
    if (bytes == 0) return {};
    std::vector<char> buf(bytes);
    if (!SetupDiGetDeviceRegistryPropertyA(devInfo, &devData, prop, nullptr,
                                            reinterpret_cast<BYTE*>(buf.data()),
                                            bytes, nullptr)) {
        return {};
    }
    return std::string(buf.data());
}

// Read the PortName ("COM5") from the device's Device Parameters registry key.
std::string readPortName(HDEVINFO devInfo, SP_DEVINFO_DATA& devData) {
    HKEY key = SetupDiOpenDevRegKey(devInfo, &devData, DICS_FLAG_GLOBAL, 0,
                                     DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) return {};
    char buf[64] = {0};
    DWORD bufLen = sizeof(buf);
    DWORD type = 0;
    LONG r = RegQueryValueExA(key, "PortName", nullptr, &type,
                               reinterpret_cast<BYTE*>(buf), &bufLen);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return std::string(buf);
}
#endif

std::filesystem::path sidecarPath() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::filesystem::path(appdata) / "Entity" / "dmx_enttec_devices.json";
    }
#endif
    return std::filesystem::path(".") / "dmx_enttec_devices.json";
}

} // namespace

std::vector<EnttecDevice> enumerateEnttecDevices() {
#ifdef _WIN32
    std::vector<EnttecDevice> out;
    HDEVINFO devInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr,
                                             nullptr, DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) return out;

    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); ++i) {
        const std::string hwid = getStringProperty(devInfo, devData,
                                                    SPDRP_HARDWAREID);
        if (!isFtdiHardwareId(hwid)) continue;
        EnttecDevice d;
        d.portName    = readPortName(devInfo, devData);
        d.description = getStringProperty(devInfo, devData, SPDRP_FRIENDLYNAME);
        if (!d.portName.empty()) {
            out.push_back(std::move(d));
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return out;
#else
    return {};
#endif
}

void writeDeviceSidecar(const std::vector<EnttecDevice>& devices) {
    nlohmann::json j;
    j["devices"] = nlohmann::json::array();
    for (const auto& d : devices) {
        j["devices"].push_back({
            {"port",        d.portName},
            {"description", d.description}
        });
    }
    const auto p = sidecarPath();
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p);
    if (out.is_open()) out << j.dump(2);
}

} // namespace entity::dmx
