// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace entity::dmx {

// State-machine parser for Enttec DMX-USB-Pro inbound frames.
//
// Wire framing:
//   0x7E [label] [len-lo] [len-hi] [data...len bytes...] 0xE7
//
// We only consume label 9 (Received DMX on change). Payload for label
// 9 is [start_code, ch1..chN] -- start_code is usually 0x00.
class EnttecFrameParser {
public:
    using OnDmxFrame = std::function<void(const std::uint8_t* channels,
                                           std::size_t count)>;

    explicit EnttecFrameParser(OnDmxFrame cb) : m_cb(std::move(cb)) {}

    // Feed raw bytes from the serial port. The callback fires once
    // per complete label-9 frame seen.
    void feed(const std::uint8_t* bytes, std::size_t len);

private:
    enum class State : std::uint8_t {
        WaitSof = 0,
        Label,
        LenLo,
        LenHi,
        Data,
        Eof
    };

    State                       m_state{State::WaitSof};
    std::uint8_t                m_label{0};
    std::uint16_t               m_remaining{0};
    std::vector<std::uint8_t>   m_buf;
    OnDmxFrame                  m_cb;
};

} // namespace entity::dmx
