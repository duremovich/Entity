#pragma once

#include "Message.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace entity::bus {

// Serialize a Message to bytes. Wire format is UTF-8 JSON with a
// `{"type": "...", "data": {...}}` envelope. Indirected behind this one
// function so a Phase E swap to MessagePack is a single-file change.
// Serialization is stable: serializing the same Message twice yields
// equal bytes, and serialize→deserialize→serialize round-trips byte-exact.
std::vector<std::uint8_t> serialize(const Message& msg);

// Inverse of serialize. Returns nullopt on parse failure or unknown type.
std::optional<Message> deserialize(std::span<const std::uint8_t> bytes);
std::optional<Message> deserialize(const std::vector<std::uint8_t>& bytes);

} // namespace entity::bus
