#pragma once

#include <cstdint>

namespace entity {

// Tagged-union over the parameter types an effect can declare. Layout
// is fixed at 24 bytes — small enough that a vector<ParamValue> stays
// cache-friendly even for effects with a dozen params, and pure-POD so
// it's trivially copyable into the snapshot-bake paramBlob path.
//
// Float    → f4[0]
// Vec2     → f4[0..1]
// Vec3     → f4[0..2]
// Color    → f4[0..3] (rgba, linear-light)
// Int/Enum → i
// Bool     → b
//
// Extending: add a Type enum value + a payload field if it doesn't fit
// in the existing storage. Texture / asset-reference parameters will
// likely arrive via a sibling component (TextureSocket binding), not by
// stuffing a handle into ParamValue.
struct ParamValue {
    enum class Type : std::uint8_t {
        Float = 0,
        Int   = 1,
        Vec2  = 2,
        Vec3  = 3,
        Color = 4,
        Bool  = 5,
        Enum  = 6,
    };

    Type type{Type::Float};
    float f4[4]{0.0f, 0.0f, 0.0f, 0.0f};
    std::int32_t i{0};
    bool b{false};

    static ParamValue makeFloat(float v) {
        ParamValue p; p.type = Type::Float; p.f4[0] = v; return p;
    }
    static ParamValue makeInt(std::int32_t v) {
        ParamValue p; p.type = Type::Int; p.i = v; return p;
    }
    static ParamValue makeVec2(float x, float y) {
        ParamValue p; p.type = Type::Vec2; p.f4[0] = x; p.f4[1] = y; return p;
    }
    static ParamValue makeVec3(float x, float y, float z) {
        ParamValue p; p.type = Type::Vec3;
        p.f4[0] = x; p.f4[1] = y; p.f4[2] = z; return p;
    }
    static ParamValue makeColor(float r, float g, float b, float a) {
        ParamValue p; p.type = Type::Color;
        p.f4[0] = r; p.f4[1] = g; p.f4[2] = b; p.f4[3] = a; return p;
    }
    static ParamValue makeBool(bool v) {
        ParamValue p; p.type = Type::Bool; p.b = v; return p;
    }
    static ParamValue makeEnum(std::int32_t v) {
        ParamValue p; p.type = Type::Enum; p.i = v; return p;
    }
};

} // namespace entity
