// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "EnttecFrameParser.hpp"

namespace entity::dmx {

namespace {
constexpr std::uint8_t kSof = 0x7E;
constexpr std::uint8_t kEof = 0xE7;
constexpr std::uint8_t kLabelReceivedDmx = 9;
} // namespace

void EnttecFrameParser::feed(const std::uint8_t* bytes, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t b = bytes[i];
        switch (m_state) {
        case State::WaitSof:
            if (b == kSof) m_state = State::Label;
            break;
        case State::Label:
            m_label = b;
            m_state = State::LenLo;
            break;
        case State::LenLo:
            m_remaining = b;
            m_state = State::LenHi;
            break;
        case State::LenHi:
            m_remaining |= static_cast<std::uint16_t>(b) << 8;
            m_buf.clear();
            if (m_remaining == 0) {
                m_state = State::Eof;
            } else {
                m_buf.reserve(m_remaining);
                m_state = State::Data;
            }
            break;
        case State::Data:
            m_buf.push_back(b);
            if (m_buf.size() == m_remaining) m_state = State::Eof;
            break;
        case State::Eof:
            if (b == kEof) {
                if (m_label == kLabelReceivedDmx && !m_buf.empty() && m_cb) {
                    // First byte = start code; skip it.
                    if (m_buf.size() > 1) {
                        m_cb(m_buf.data() + 1, m_buf.size() - 1);
                    }
                }
                m_state = State::WaitSof;
            } else {
                // Framing error -- resync from the next 0x7E.
                m_state = (b == kSof) ? State::Label : State::WaitSof;
            }
            break;
        }
    }
}

} // namespace entity::dmx
