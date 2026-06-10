#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace entity::remote {

// Live-control parameter axes (v1 presentation params). UV-space
// semantics match AnimatableProperty: Rotation is the Z axis.
enum class RemoteParam : std::uint8_t {
    Opacity = 0, PosX, PosY, ScaleX, ScaleY, Rotation,
    Count
};
inline constexpr std::size_t kRemoteParamCount =
    static_cast<std::size_t>(RemoteParam::Count);
inline constexpr int kMaxRemotePatches = 64;

/**
 * RemoteControlStore — live remote-control value plane (ADR-0028).
 *
 * Three access tiers:
 *   - Editor thread (config): allocateSlot/freeSlot/renameSlot/reset/
 *     setEngaged. Mutates the id map under m_idMutex.
 *   - Any thread (live writes, message-rate): set*ById — id-map lookup
 *     AND atomic stores both happen under m_idMutex, so a write can
 *     never land on a freed or reallocated slot. OSC receiver / future
 *     DMX call these via IPluginContext::setRemoteParam. m_idMutex is
 *     never held by the show thread, so sample() remains lock-free.
 *   - Show thread (render-rate reads): sample(slot) — indexed atomic
 *     loads ONLY. No locks, no strings, no allocation. Never persisted,
 *     never on the undo stack.
 *
 * Values written while disengaged are stored (not applied), so engaging
 * takeover picks up current fader state without a re-send.
 */
class RemoteControlStore {
public:
    struct Sample {
        bool engaged{false};
        std::uint32_t presentMask{0};
        std::array<float, kRemoteParamCount> values{};
        bool  has(RemoteParam p) const {
            return (presentMask >> static_cast<unsigned>(p)) & 1u;
        }
        float get(RemoteParam p) const {
            return values[static_cast<std::size_t>(p)];
        }
    };

    // --- editor thread: config plane ---
    int  allocateSlot(const std::string& patchId);   // -1: full / dup / bad id
    void freeSlot(int slot);
    bool renameSlot(int slot, const std::string& newId);
    void reset();                                    // project close / pre-load
    void setEngaged(int slot, bool engaged);

    // --- any thread: live plane (by id) ---
    bool setParamById(std::string_view patchId, RemoteParam p, float value);
    bool setEngagedById(std::string_view patchId, bool engaged);
    bool setTextById(std::string_view patchId, std::string_view text);

    // --- show thread: render-rate reads (slot-indexed) ---
    Sample sample(int slot) const noexcept;

    // --- editor thread: UI readout + text consumption ---
    bool engaged(int slot) const noexcept;
    // Returns the text iff its generation differs from lastSeenGen;
    // updates lastSeenGen. Mutex-guarded — editor tick rate only.
    std::optional<std::string> consumeText(int slot,
                                           std::uint32_t& lastSeenGen) const;

private:
    struct Slot {
        std::array<std::atomic<float>, kRemoteParamCount> values{};
        std::atomic<std::uint32_t> presentMask{0};
        std::atomic<std::uint32_t> engagedFlag{0};
        std::atomic<std::uint32_t> textGen{0};
        std::atomic<bool>          inUse{false};
        mutable std::mutex         textMutex;
        std::string                text;
    };

    int findSlotLocked(std::string_view patchId) const;  // caller holds m_idMutex

    mutable std::mutex                   m_idMutex;
    std::unordered_map<std::string, int> m_idToSlot;
    std::array<Slot, kMaxRemotePatches>  m_slots;
};

} // namespace entity::remote
