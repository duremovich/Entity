#pragma once

// Drop-in replacements for ImGui's numeric widgets that accept basic
// arithmetic expressions in text-edit mode. Typing "6.74 / 2" and hitting
// Enter commits 3.37. Plain numbers still work; the wrapper is a strict
// superset of ImGui's existing parser behavior.
//
// Supported operators: + - * /, parentheses, unary +/-, scientific
// notation. No variables, functions, units, or constants — by design.
// On parse failure the widget silently reverts to its previous value.
//
// All wrappers preserve the underlying ImGui widget's ImGuiID so the
// surrounding IsItemActivated() / IsItemDeactivatedAfterEdit() plumbing
// (heavily used by PropertyWindow for keyframe pre/post-edit snapshots)
// continues to fire correctly. MarkItemEdited(id) is called on commit.
//
// Pinned to ImGui 1.89.7. See docs/adr/ if upgrading the dep.

#include <imgui.h>

#include <optional>
#include <string_view>

namespace entity::ui {

// Evaluate a single arithmetic expression. Returns nullopt on:
//   - empty / whitespace-only input
//   - unparseable syntax ("3 +", "(1+2", "abc")
//   - trailing garbage after a valid expression (except whitespace and a
//     trailing format-decoration suffix — see below)
//   - division by zero
//   - non-finite result (overflow / underflow / NaN)
//
// Trailing format decoration like " deg" (from formats such as
// "%.1f deg") is tolerated when it begins with whitespace followed by a
// non-digit, non-operator character — this lets the user edit a field
// whose buffer was seeded by ImGui's printf-style format string.
std::optional<double> evaluateExpression(std::string_view text);

// ---- Numeric widget wrappers --------------------------------------------
//
// Signatures match the corresponding ImGui:: functions for ImGui 1.89.7.
// All return true when the bound value changed this frame, matching
// ImGui semantics.

bool DragFloat(const char* label, float* v, float speed = 1.0f,
               float v_min = 0.0f, float v_max = 0.0f,
               const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool DragFloat2(const char* label, float v[2], float speed = 1.0f,
                float v_min = 0.0f, float v_max = 0.0f,
                const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool DragFloat3(const char* label, float v[3], float speed = 1.0f,
                float v_min = 0.0f, float v_max = 0.0f,
                const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool DragFloat4(const char* label, float v[4], float speed = 1.0f,
                float v_min = 0.0f, float v_max = 0.0f,
                const char* format = "%.3f", ImGuiSliderFlags flags = 0);

bool SliderFloat(const char* label, float* v, float v_min, float v_max,
                 const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool SliderFloat2(const char* label, float v[2], float v_min, float v_max,
                  const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool SliderFloat3(const char* label, float v[3], float v_min, float v_max,
                  const char* format = "%.3f", ImGuiSliderFlags flags = 0);

bool InputFloat(const char* label, float* v, float step = 0.0f,
                float step_fast = 0.0f, const char* format = "%.3f",
                ImGuiInputTextFlags flags = 0);
bool InputDouble(const char* label, double* v, double step = 0.0,
                 double step_fast = 0.0, const char* format = "%.6f",
                 ImGuiInputTextFlags flags = 0);

bool DragInt(const char* label, int* v, float speed = 1.0f,
             int v_min = 0, int v_max = 0, const char* format = "%d",
             ImGuiSliderFlags flags = 0);
bool SliderInt(const char* label, int* v, int v_min, int v_max,
               const char* format = "%d", ImGuiSliderFlags flags = 0);
bool InputInt(const char* label, int* v, int step = 1, int step_fast = 100,
              ImGuiInputTextFlags flags = 0);
bool InputInt2(const char* label, int v[2], ImGuiInputTextFlags flags = 0);

// InputScalar — covers all integer widths and double via the data-type
// argument. Used by cue modals (frame field is ImGuiDataType_S64) and
// any future numeric fields that need a non-standard integer width.
bool InputScalar(const char* label, ImGuiDataType data_type, void* p_data,
                 const void* p_step = nullptr,
                 const void* p_step_fast = nullptr,
                 const char* format = nullptr,
                 ImGuiInputTextFlags flags = 0);

}  // namespace entity::ui
