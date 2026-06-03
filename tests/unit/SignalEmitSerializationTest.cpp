// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"

#include <gtest/gtest.h>

using namespace entity::bus;

namespace {

// Serialize a T, deserialize, assert the decoded variant holds T, return it.
template <class T>
T roundTripExpect(const T& original) {
    Message msg = original;
    auto bytes = serialize(msg);
    auto decoded = deserialize(bytes);
    EXPECT_TRUE(decoded.has_value());
    EXPECT_TRUE(std::holds_alternative<T>(*decoded));
    return std::get<T>(*decoded);
}

// Serialize twice (with an intermediate decode) and confirm byte-identical output.
template <class T>
void expectByteStable(const T& original) {
    Message msg = original;
    auto bytes1 = serialize(msg);
    auto decoded = deserialize(bytes1);
    ASSERT_TRUE(decoded.has_value());
    auto bytes2 = serialize(*decoded);
    EXPECT_EQ(bytes1, bytes2)
        << "serialize->deserialize->serialize must be byte-identical for "
        << messageTypeName(msg);
}

} // namespace

// ----------------------------------------------------------------------------
// SignalEmitSerialization — round-trip + wire contract tests.
// ----------------------------------------------------------------------------

TEST(SignalEmitSerialization, RoundTripTypedArgs) {
    SignalEmit m;
    m.transport = SignalEmit::Transport::OSC;
    m.address   = "/entity/signal/test";

    SignalArg iArg;
    iArg.type = SignalArg::Type::Int;
    iArg.i    = -42;

    SignalArg fArg;
    fArg.type = SignalArg::Type::Float;
    fArg.f    = 3.14f;

    SignalArg sArg;
    sArg.type = SignalArg::Type::String;
    sArg.s    = "hello";

    m.args.push_back(iArg);
    m.args.push_back(fArg);
    m.args.push_back(sArg);

    auto out = roundTripExpect(m);

    EXPECT_EQ(out.transport, SignalEmit::Transport::OSC);
    EXPECT_EQ(out.address, m.address);

    ASSERT_EQ(out.args.size(), 3u);

    EXPECT_EQ(out.args[0].type, SignalArg::Type::Int);
    EXPECT_EQ(out.args[0].i, -42);

    EXPECT_EQ(out.args[1].type, SignalArg::Type::Float);
    EXPECT_FLOAT_EQ(out.args[1].f, 3.14f);

    EXPECT_EQ(out.args[2].type, SignalArg::Type::String);
    EXPECT_EQ(out.args[2].s, "hello");

    expectByteStable(m);
}

TEST(SignalEmitSerialization, EmptyArgList) {
    SignalEmit m;
    m.transport = SignalEmit::Transport::OSC;
    m.address   = "/osc/bang";

    auto out = roundTripExpect(m);
    EXPECT_EQ(out.address, m.address);
    EXPECT_TRUE(out.args.empty());
    expectByteStable(m);
}

TEST(SignalEmitSerialization, AllArgTypes) {
    // Exercise every SignalArg::Type through the enum<->string codec.
    const SignalArg::Type allTypes[] = {
        SignalArg::Type::Int,
        SignalArg::Type::Float,
        SignalArg::Type::String,
    };
    for (auto t : allTypes) {
        SignalEmit m;
        m.address = "/entity/signal/argtype";
        SignalArg a;
        a.type = t;
        m.args.push_back(a);
        auto out = roundTripExpect(m);
        ASSERT_EQ(out.args.size(), 1u);
        EXPECT_EQ(out.args[0].type, t)
            << "SignalArg::Type round-trip failed for value " << static_cast<int>(t);
    }
}

TEST(SignalEmitSerialization, MessageTypeNameIsSignalEmit) {
    Message msg = SignalEmit{};
    EXPECT_EQ(std::string(messageTypeName(msg)), "SignalEmit");
}

TEST(SignalEmitSerialization, UnknownTransportReturnsNullopt) {
    // Wire payload with an unrecognized transport string must not deserialize
    // silently — the bus rule is "unknown enum value throws -> nullopt".
    const std::string bad =
        R"({"type":"SignalEmit","data":{"transport":"MIDI","address":"/x","args":[]}})";
    std::vector<std::uint8_t> bytes(bad.begin(), bad.end());
    EXPECT_FALSE(deserialize(bytes).has_value());
}

TEST(SignalEmitSerialization, UnknownArgTypeReturnsNullopt) {
    const std::string bad =
        R"({"type":"SignalEmit","data":{"transport":"OSC","address":"/x","args":[{"type":"Bool","i":0,"f":0.0,"s":""}]}})";
    std::vector<std::uint8_t> bytes(bad.begin(), bad.end());
    EXPECT_FALSE(deserialize(bytes).has_value());
}
