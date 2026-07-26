#pragma once

#include "entity/components/EffectParam.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace entity::effects {

// JSON mapping for ParamValue, shared by ProjectSerializer (v29 typed
// params) and SetEffectParamCommand (script/UI edits). Type tags are
// strings to keep .entity files and scripts diff-friendly.
//
// Value payload shape: scalar for Float/Int/Enum/Bool, array for the
// vector types ([x,y] / [x,y,z] / [r,g,b,a]).

std::string      paramTypeToJson(ParamValue::Type t);
ParamValue::Type jsonToParamType(const std::string& s);

nlohmann::json paramValueToJson(const ParamValue& v);

// Missing / wrong-shaped payload fields keep the ParamValue default of
// the requested type (never throws).
ParamValue jsonToParamValue(ParamValue::Type type, const nlohmann::json& val);

} // namespace entity::effects
