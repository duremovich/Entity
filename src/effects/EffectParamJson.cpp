#include "entity/effects/EffectParamJson.hpp"

namespace entity::effects {

using nlohmann::json;

std::string paramTypeToJson(ParamValue::Type t) {
    switch (t) {
        case ParamValue::Type::Float: return "float";
        case ParamValue::Type::Int:   return "int";
        case ParamValue::Type::Vec2:  return "vec2";
        case ParamValue::Type::Vec3:  return "vec3";
        case ParamValue::Type::Color: return "color";
        case ParamValue::Type::Bool:  return "bool";
        case ParamValue::Type::Enum:  return "enum";
    }
    return "float";
}

ParamValue::Type jsonToParamType(const std::string& s) {
    if (s == "int")   return ParamValue::Type::Int;
    if (s == "vec2")  return ParamValue::Type::Vec2;
    if (s == "vec3")  return ParamValue::Type::Vec3;
    if (s == "color") return ParamValue::Type::Color;
    if (s == "bool")  return ParamValue::Type::Bool;
    if (s == "enum")  return ParamValue::Type::Enum;
    return ParamValue::Type::Float;
}

json paramValueToJson(const ParamValue& v) {
    switch (v.type) {
        case ParamValue::Type::Float: return v.f4[0];
        case ParamValue::Type::Int:
        case ParamValue::Type::Enum:  return v.i;
        case ParamValue::Type::Bool:  return v.b;
        case ParamValue::Type::Vec2:  return json::array({v.f4[0], v.f4[1]});
        case ParamValue::Type::Vec3:  return json::array({v.f4[0], v.f4[1], v.f4[2]});
        case ParamValue::Type::Color: return json::array({v.f4[0], v.f4[1], v.f4[2], v.f4[3]});
    }
    return v.f4[0];
}

ParamValue jsonToParamValue(ParamValue::Type type, const json& val) {
    ParamValue p;
    p.type = type;
    auto readVec = [&](std::size_t n) {
        if (!val.is_array()) return;
        for (std::size_t i = 0; i < n && i < val.size(); ++i) {
            if (val[i].is_number()) p.f4[i] = val[i].get<float>();
        }
    };
    switch (type) {
        case ParamValue::Type::Float:
            if (val.is_number()) p.f4[0] = val.get<float>();
            break;
        case ParamValue::Type::Int:
        case ParamValue::Type::Enum:
            if (val.is_number()) p.i = val.get<std::int32_t>();
            break;
        case ParamValue::Type::Bool:
            if (val.is_boolean()) p.b = val.get<bool>();
            break;
        case ParamValue::Type::Vec2:  readVec(2); break;
        case ParamValue::Type::Vec3:  readVec(3); break;
        case ParamValue::Type::Color: readVec(4); break;
    }
    return p;
}

} // namespace entity::effects
