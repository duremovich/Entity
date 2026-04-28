#include "entity/bus/InMemoryMessageTransport.hpp"
#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

using namespace entity;
using namespace entity::bus;

namespace {

std::vector<std::uint8_t> makeBytes(std::initializer_list<int> ints) {
    std::vector<std::uint8_t> v;
    v.reserve(ints.size());
    for (int i : ints) v.push_back(static_cast<std::uint8_t>(i));
    return v;
}

} // namespace

TEST(InMemoryTransport, BasicSendDrainSingleDirection) {
    InMemoryMessageTransport t;
    t.send(Direction::D2R, makeBytes({1, 2, 3}));
    t.send(Direction::D2R, makeBytes({4, 5, 6}));
    EXPECT_EQ(t.pending(Direction::D2R), 2u);
    EXPECT_EQ(t.pending(Direction::R2D), 0u);

    std::vector<std::vector<std::uint8_t>> received;
    t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&& b) {
        received.push_back(std::move(b));
    });
    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0], makeBytes({1, 2, 3}));
    EXPECT_EQ(received[1], makeBytes({4, 5, 6}));
    EXPECT_EQ(t.pending(Direction::D2R), 0u);
}

TEST(InMemoryTransport, DirectionsAreIsolated) {
    InMemoryMessageTransport t;
    t.send(Direction::D2R, makeBytes({1}));
    t.send(Direction::R2D, makeBytes({2}));

    std::vector<std::uint8_t> got;
    t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&& b) { got = std::move(b); });
    EXPECT_EQ(got, makeBytes({1}));
    EXPECT_EQ(t.pending(Direction::R2D), 1u) << "draining D2R must not touch R2D";

    got.clear();
    t.drain(Direction::R2D, [&](std::vector<std::uint8_t>&& b) { got = std::move(b); });
    EXPECT_EQ(got, makeBytes({2}));
}

TEST(InMemoryTransport, FifoOrderPreserved) {
    InMemoryMessageTransport t;
    for (int i = 0; i < 100; ++i) {
        t.send(Direction::D2R, makeBytes({i & 0xff}));
    }
    int seen = 0;
    t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&& b) {
        ASSERT_EQ(b.size(), 1u);
        EXPECT_EQ(b[0], static_cast<std::uint8_t>(seen & 0xff));
        ++seen;
    });
    EXPECT_EQ(seen, 100);
}

TEST(InMemoryTransport, DrainCalledTwiceSecondTimeIsEmpty) {
    InMemoryMessageTransport t;
    t.send(Direction::D2R, makeBytes({1}));
    int calls1 = 0, calls2 = 0;
    t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&&) { ++calls1; });
    t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&&) { ++calls2; });
    EXPECT_EQ(calls1, 1);
    EXPECT_EQ(calls2, 0);
}

TEST(InMemoryTransport, ZeroByteMessagesPreserved) {
    InMemoryMessageTransport t;
    t.send(Direction::D2R, std::vector<std::uint8_t>{});
    int calls = 0;
    t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&& b) {
        EXPECT_TRUE(b.empty());
        ++calls;
    });
    EXPECT_EQ(calls, 1);
}

TEST(InMemoryTransport, SendDuringDrainIsSafe) {
    // A sink that bounces a reply back on the same direction must not
    // self-deadlock and must not see its own bounce within the same drain
    // call (drain stole the queue snapshot before invoking sinks).
    InMemoryMessageTransport t;
    t.send(Direction::D2R, makeBytes({1}));
    t.send(Direction::D2R, makeBytes({2}));

    int delivered = 0;
    int reentries = 0;
    t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&& b) {
        ++delivered;
        if (b == makeBytes({1})) {
            ++reentries;
            t.send(Direction::D2R, makeBytes({99})); // bounces a reply
        }
    });
    EXPECT_EQ(delivered, 2);
    EXPECT_EQ(reentries, 1);
    EXPECT_EQ(t.pending(Direction::D2R), 1u) << "bounced reply queued for next drain";

    int second = 0;
    t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&& b) {
        ++second;
        EXPECT_EQ(b, makeBytes({99}));
    });
    EXPECT_EQ(second, 1);
}

TEST(InMemoryTransport, ConcurrentProducerConsumer10k) {
    // Hammer test from the plan: multiple producers, single consumer
    // pumping drain in a loop until all messages received.
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 2500;
    constexpr int kTotal = kProducers * kPerProducer;

    InMemoryMessageTransport t;
    std::atomic<int> received{0};
    std::set<int> seen; // consumer-only, no contention
    std::atomic<bool> stop{false};

    std::thread consumer([&] {
        while (!stop.load(std::memory_order_acquire) || received.load() < kTotal) {
            t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&& b) {
                ASSERT_EQ(b.size(), 4u);
                int id = (b[0]) | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
                seen.insert(id);
                received.fetch_add(1, std::memory_order_relaxed);
            });
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                int id = p * kPerProducer + i;
                std::vector<std::uint8_t> bytes = {
                    static_cast<std::uint8_t>(id & 0xff),
                    static_cast<std::uint8_t>((id >> 8) & 0xff),
                    static_cast<std::uint8_t>((id >> 16) & 0xff),
                    static_cast<std::uint8_t>((id >> 24) & 0xff),
                };
                t.send(Direction::D2R, std::move(bytes));
            }
        });
    }
    for (auto& th : producers) th.join();
    stop.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(received.load(), kTotal);
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kTotal))
        << "every producer message id must appear exactly once";
}

TEST(InMemoryTransport, RoundTripViaSerialization) {
    // End-to-end smoke: serialize a Message into bytes, hand to the
    // transport, drain on the other side, deserialize back.
    InMemoryMessageTransport t;

    RenderFrame rf;
    rf.frameNumber = 7;
    rf.deltaTime = 0.0166;
    rf.playState = TransportState::Playing;

    t.send(Direction::D2R, serialize(Message{rf}));

    Message decoded;
    int calls = 0;
    t.drain(Direction::D2R, [&](std::vector<std::uint8_t>&& bytes) {
        auto msg = deserialize(bytes);
        ASSERT_TRUE(msg.has_value());
        decoded = std::move(*msg);
        ++calls;
    });
    ASSERT_EQ(calls, 1);

    ASSERT_TRUE(std::holds_alternative<RenderFrame>(decoded));
    const auto& got = std::get<RenderFrame>(decoded);
    EXPECT_EQ(got.frameNumber, 7);
    EXPECT_DOUBLE_EQ(got.deltaTime, 0.0166);
    EXPECT_EQ(got.playState, TransportState::Playing);
}
