#include "entity/ui/MathInput.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace entity::ui {

// ===================================================================
// Expression parser
// ===================================================================
//
// Grammar (recursive descent, left-associative):
//   expr   = term  (('+'|'-') term)*
//   term   = factor (('*'|'/') factor)*
//   factor = ('-'|'+') factor | '(' expr ')' | NUMBER
//   NUMBER = anything std::from_chars<double> accepts
//
// Trailing whitespace + non-numeric/non-operator decoration (e.g.
// " deg", " px") is tolerated so the parser plays nicely with seed
// buffers produced by printf-style formats like "%.1f deg".

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text), m_pos(0) {}

    std::optional<double> parse() {
        skipWhitespace();
        if (m_pos >= m_text.size()) return std::nullopt;

        auto result = parseExpr();
        if (!result) return std::nullopt;

        skipWhitespace();
        if (m_pos < m_text.size()) {
            // Anything that could plausibly extend the expression is an
            // error. Anything else (alpha, comma, etc.) is treated as
            // format decoration and ignored.
            char c = m_text[m_pos];
            const bool extendsExpr =
                (c == '+' || c == '-' || c == '*' || c == '/'
                 || c == '(' || c == ')' || c == '.'
                 || (c >= '0' && c <= '9'));
            if (extendsExpr) return std::nullopt;
        }

        if (!std::isfinite(*result)) return std::nullopt;
        return result;
    }

private:
    std::string_view m_text;
    size_t m_pos;

    void skipWhitespace() {
        while (m_pos < m_text.size()
               && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
            ++m_pos;
    }

    std::optional<double> parseExpr() {
        auto lhs = parseTerm();
        if (!lhs) return std::nullopt;
        while (true) {
            skipWhitespace();
            if (m_pos >= m_text.size()) break;
            char op = m_text[m_pos];
            if (op != '+' && op != '-') break;
            ++m_pos;
            auto rhs = parseTerm();
            if (!rhs) return std::nullopt;
            lhs = (op == '+') ? (*lhs + *rhs) : (*lhs - *rhs);
        }
        return lhs;
    }

    std::optional<double> parseTerm() {
        auto lhs = parseFactor();
        if (!lhs) return std::nullopt;
        while (true) {
            skipWhitespace();
            if (m_pos >= m_text.size()) break;
            char op = m_text[m_pos];
            if (op != '*' && op != '/') break;
            ++m_pos;
            auto rhs = parseFactor();
            if (!rhs) return std::nullopt;
            if (op == '/') {
                if (*rhs == 0.0) return std::nullopt;
                lhs = *lhs / *rhs;
            } else {
                lhs = *lhs * *rhs;
            }
        }
        return lhs;
    }

    std::optional<double> parseFactor() {
        skipWhitespace();
        if (m_pos >= m_text.size()) return std::nullopt;
        char c = m_text[m_pos];
        if (c == '-') {
            ++m_pos;
            auto inner = parseFactor();
            if (!inner) return std::nullopt;
            return -*inner;
        }
        if (c == '+') {
            ++m_pos;
            return parseFactor();
        }
        if (c == '(') {
            ++m_pos;
            auto inner = parseExpr();
            if (!inner) return std::nullopt;
            skipWhitespace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ')')
                return std::nullopt;
            ++m_pos;
            return inner;
        }
        return parseNumber();
    }

    std::optional<double> parseNumber() {
        const char* begin = m_text.data() + m_pos;
        const char* end = m_text.data() + m_text.size();
        double value = 0.0;
        auto r = std::from_chars(begin, end, value);
        if (r.ec != std::errc{}) return std::nullopt;
        m_pos = static_cast<size_t>(r.ptr - m_text.data());
        return value;
    }
};

}  // namespace

std::optional<double> evaluateExpression(std::string_view text) {
    Parser p(text);
    return p.parse();
}

// ===================================================================
// Widget plumbing
// ===================================================================

namespace {

constexpr size_t kBufSize = 64;

struct EditState {
    char buf[kBufSize] = {};
    bool justEntered = false;  // grab focus on first frame of expression mode
    bool wasActive = false;    // Input* path: skip buffer refresh while editing
};

// Per-id state. ImGui is main-thread-only, so the static map is fine.
// Entries are dropped on commit/cancel for drag/slider widgets; Input*
// widgets keep their entry alive across frames so the buffer survives.
std::unordered_map<ImGuiID, EditState>& editStates() {
    static std::unordered_map<ImGuiID, EditState> s;
    return s;
}

void formatValue(char* buf, size_t buf_size, ImGuiDataType type,
                 const void* p_data, const char* format) {
    if (!format) format = ImGui::DataTypeGetInfo(type)->PrintFmt;
    ImGui::DataTypeFormatString(buf, static_cast<int>(buf_size), type,
                                p_data, format);
}

// Clamp `value` into [min, max] when min != max (ImGui's "clamping is
// inactive when min == max" convention). Returns the clamped value.
double clampValue(double value, ImGuiDataType type,
                  const void* p_min, const void* p_max) {
    if (!p_min || !p_max) return value;
    double mn = 0.0, mx = 0.0;
    switch (type) {
        case ImGuiDataType_Float:
            mn = *static_cast<const float*>(p_min);
            mx = *static_cast<const float*>(p_max);
            break;
        case ImGuiDataType_Double:
            mn = *static_cast<const double*>(p_min);
            mx = *static_cast<const double*>(p_max);
            break;
        case ImGuiDataType_S32:
            mn = *static_cast<const int*>(p_min);
            mx = *static_cast<const int*>(p_max);
            break;
        case ImGuiDataType_U32:
            mn = *static_cast<const unsigned*>(p_min);
            mx = *static_cast<const unsigned*>(p_max);
            break;
        case ImGuiDataType_S64:
            mn = static_cast<double>(*static_cast<const long long*>(p_min));
            mx = static_cast<double>(*static_cast<const long long*>(p_max));
            break;
        case ImGuiDataType_U64:
            mn = static_cast<double>(*static_cast<const unsigned long long*>(p_min));
            mx = static_cast<double>(*static_cast<const unsigned long long*>(p_max));
            break;
        default:
            return value;
    }
    if (mn == mx) return value;  // clamping disabled
    if (value < mn) value = mn;
    if (value > mx) value = mx;
    return value;
}

bool applyValue(double value, ImGuiDataType type, void* p_data,
                const void* p_min, const void* p_max) {
    const double v = clampValue(value, type, p_min, p_max);
    switch (type) {
        case ImGuiDataType_Float:
            *static_cast<float*>(p_data) = static_cast<float>(v);
            return true;
        case ImGuiDataType_Double:
            *static_cast<double*>(p_data) = v;
            return true;
        case ImGuiDataType_S32:
            *static_cast<int*>(p_data) = static_cast<int>(v);
            return true;
        case ImGuiDataType_U32:
            *static_cast<unsigned*>(p_data) =
                static_cast<unsigned>(std::max(0.0, v));
            return true;
        case ImGuiDataType_S64:
            *static_cast<long long*>(p_data) = static_cast<long long>(v);
            return true;
        case ImGuiDataType_U64:
            *static_cast<unsigned long long*>(p_data) =
                static_cast<unsigned long long>(std::max(0.0, v));
            return true;
        default:
            return false;
    }
}

// Decide whether the drag/slider widget we just rendered should switch
// to expression-edit mode this frame. Triggered by Ctrl+Click,
// double-click, or keyboard nav-activate.
bool shouldEnterExpressionMode() {
    const bool hovered = ImGui::IsItemHovered();
    const bool activated = ImGui::IsItemActivated();
    const ImGuiIO& io = ImGui::GetIO();
    const bool leftClicked =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool leftDoubleClicked =
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    const bool ctrlClick = hovered && leftClicked && io.KeyCtrl;
    const bool dblClick = hovered && leftDoubleClicked;
    // Keyboard nav activate: item just became active without a mouse
    // click. Catches Tab → Enter / Space.
    const bool navActivate = activated && !leftClicked && !leftDoubleClicked;

    return ctrlClick || dblClick || navActivate;
}

// Render the expression-mode InputText for an active edit. Returns true
// if a value was successfully committed. Sets `deactivated` if the edit
// finished (either via commit or cancel) so the caller can drop the
// state entry.
bool renderExpressionInput(ImGuiID id, const char* label,
                           ImGuiDataType type, void* p_data,
                           const void* p_min, const void* p_max,
                           bool& deactivated) {
    auto& states = editStates();
    auto it = states.find(id);
    if (it == states.end()) {
        deactivated = true;
        return false;
    }
    EditState& st = it->second;

    if (st.justEntered) {
        ImGui::SetKeyboardFocusHere();
        st.justEntered = false;
    }

    const ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_AutoSelectAll
        | ImGuiInputTextFlags_EnterReturnsTrue;

    const bool enterPressed =
        ImGui::InputText(label, st.buf, sizeof(st.buf), flags);
    const bool deactAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
    const bool deact = ImGui::IsItemDeactivated();

    bool committed = false;
    if (enterPressed || deactAfterEdit) {
        auto value = evaluateExpression(st.buf);
        if (value && applyValue(*value, type, p_data, p_min, p_max)) {
            committed = true;
            ImGui::MarkItemEdited(id);
        }
        deactivated = true;
    } else if (deact) {
        // Escape / cancel — drop state, leave *p_data untouched.
        deactivated = true;
    }
    return committed;
}

// Drag / Slider path: render the normal widget with NoInput, watch for
// activation triggers, transition to expression mode on next frame.
template <typename RenderFn>
bool dragOrSlider(const char* label, ImGuiDataType type, void* p_data,
                  const void* p_min, const void* p_max,
                  const char* format, RenderFn renderFn) {
    ImGuiID id = ImGui::GetCurrentWindow()->GetID(label);
    auto& states = editStates();

    if (states.count(id)) {
        bool deactivated = false;
        bool committed = renderExpressionInput(id, label, type, p_data,
                                               p_min, p_max, deactivated);
        if (deactivated) states.erase(id);
        return committed;
    }

    const bool changed = renderFn();

    if (shouldEnterExpressionMode()) {
        EditState& st = states[id];
        st.justEntered = true;
        st.wasActive = false;
        formatValue(st.buf, sizeof(st.buf), type, p_data, format);
        // The drag widget just became active on this frame (Ctrl+Click /
        // Double-Click / NavActivate). Next frame we render an InputText
        // with the same id — but ImGui's InputTextEx only initializes
        // g.InputTextState when g.ActiveId != id. Without clearing here,
        // the InputText sees "already active" and runs without state, so
        // keystrokes are ignored. Mirrors ImGui's own TempInputText.
        ImGui::ClearActiveID();
    }
    return changed;
}

// Input* path: always render as InputText with our parser. No drag.
bool inputScalar(const char* label, ImGuiDataType type, void* p_data,
                 const char* format, ImGuiInputTextFlags flags) {
    ImGuiID id = ImGui::GetCurrentWindow()->GetID(label);
    auto& states = editStates();
    EditState& st = states[id];

    if (!st.wasActive) {
        formatValue(st.buf, sizeof(st.buf), type, p_data, format);
    }

    const ImGuiInputTextFlags itFlags = flags
        | ImGuiInputTextFlags_EnterReturnsTrue
        | ImGuiInputTextFlags_AutoSelectAll;
    const bool enter = ImGui::InputText(label, st.buf, sizeof(st.buf),
                                        itFlags);
    const bool isActiveNow = ImGui::IsItemActive();
    const bool deactAfterEdit = ImGui::IsItemDeactivatedAfterEdit();

    bool committed = false;
    if (enter || deactAfterEdit) {
        auto value = evaluateExpression(st.buf);
        if (value && applyValue(*value, type, p_data, nullptr, nullptr)) {
            committed = true;
            ImGui::MarkItemEdited(id);
        }
        formatValue(st.buf, sizeof(st.buf), type, p_data, format);
        st.wasActive = false;
    } else {
        st.wasActive = isActiveNow;
    }
    return committed;
}

// Mirror of ImGui's DragScalarN — group + push-id + iterate. Components
// are rendered via the public DragFloat (which routes through
// dragOrSlider) so each axis gets expression-edit independently.
template <typename Fn>
bool multiComponent(const char* label, int components, Fn perComponent) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    bool changed = false;
    ImGui::BeginGroup();
    ImGui::PushID(label);
    ImGui::PushMultiItemsWidths(components, ImGui::CalcItemWidth());
    for (int i = 0; i < components; ++i) {
        ImGui::PushID(i);
        if (i > 0) ImGui::SameLine(0, g.Style.ItemInnerSpacing.x);
        changed |= perComponent(i);
        ImGui::PopID();
        ImGui::PopItemWidth();
    }
    ImGui::PopID();

    const char* label_end = ImGui::FindRenderedTextEnd(label);
    if (label != label_end) {
        ImGui::SameLine(0, g.Style.ItemInnerSpacing.x);
        ImGui::TextEx(label, label_end);
    }
    ImGui::EndGroup();
    return changed;
}

}  // namespace

// ===================================================================
// Public widget functions
// ===================================================================

bool DragFloat(const char* label, float* v, float speed,
               float v_min, float v_max, const char* format,
               ImGuiSliderFlags flags) {
    return dragOrSlider(label, ImGuiDataType_Float, v, &v_min, &v_max,
                        format, [&]() {
        return ImGui::DragScalar(label, ImGuiDataType_Float, v, speed,
                                 &v_min, &v_max, format,
                                 flags | ImGuiSliderFlags_NoInput);
    });
}

bool DragFloat2(const char* label, float v[2], float speed,
                float v_min, float v_max, const char* format,
                ImGuiSliderFlags flags) {
    return multiComponent(label, 2, [&](int i) {
        return DragFloat("", &v[i], speed, v_min, v_max, format, flags);
    });
}

bool DragFloat3(const char* label, float v[3], float speed,
                float v_min, float v_max, const char* format,
                ImGuiSliderFlags flags) {
    return multiComponent(label, 3, [&](int i) {
        return DragFloat("", &v[i], speed, v_min, v_max, format, flags);
    });
}

bool DragFloat4(const char* label, float v[4], float speed,
                float v_min, float v_max, const char* format,
                ImGuiSliderFlags flags) {
    return multiComponent(label, 4, [&](int i) {
        return DragFloat("", &v[i], speed, v_min, v_max, format, flags);
    });
}

bool SliderFloat(const char* label, float* v, float v_min, float v_max,
                 const char* format, ImGuiSliderFlags flags) {
    return dragOrSlider(label, ImGuiDataType_Float, v, &v_min, &v_max,
                        format, [&]() {
        return ImGui::SliderScalar(label, ImGuiDataType_Float, v,
                                   &v_min, &v_max, format,
                                   flags | ImGuiSliderFlags_NoInput);
    });
}

bool SliderFloat2(const char* label, float v[2], float v_min, float v_max,
                  const char* format, ImGuiSliderFlags flags) {
    return multiComponent(label, 2, [&](int i) {
        return SliderFloat("", &v[i], v_min, v_max, format, flags);
    });
}

bool SliderFloat3(const char* label, float v[3], float v_min, float v_max,
                  const char* format, ImGuiSliderFlags flags) {
    return multiComponent(label, 3, [&](int i) {
        return SliderFloat("", &v[i], v_min, v_max, format, flags);
    });
}

bool InputFloat(const char* label, float* v, float /*step*/,
                float /*step_fast*/, const char* format,
                ImGuiInputTextFlags flags) {
    // step/step_fast (the +/- buttons) are dropped in v1 — none of the
    // current call sites use them. Add back if a site needs them.
    return inputScalar(label, ImGuiDataType_Float, v, format, flags);
}

bool InputDouble(const char* label, double* v, double /*step*/,
                 double /*step_fast*/, const char* format,
                 ImGuiInputTextFlags flags) {
    return inputScalar(label, ImGuiDataType_Double, v, format, flags);
}

bool DragInt(const char* label, int* v, float speed,
             int v_min, int v_max, const char* format,
             ImGuiSliderFlags flags) {
    return dragOrSlider(label, ImGuiDataType_S32, v, &v_min, &v_max,
                        format, [&]() {
        return ImGui::DragScalar(label, ImGuiDataType_S32, v, speed,
                                 &v_min, &v_max, format,
                                 flags | ImGuiSliderFlags_NoInput);
    });
}

bool SliderInt(const char* label, int* v, int v_min, int v_max,
               const char* format, ImGuiSliderFlags flags) {
    return dragOrSlider(label, ImGuiDataType_S32, v, &v_min, &v_max,
                        format, [&]() {
        return ImGui::SliderScalar(label, ImGuiDataType_S32, v,
                                   &v_min, &v_max, format,
                                   flags | ImGuiSliderFlags_NoInput);
    });
}

bool InputInt(const char* label, int* v, int /*step*/, int /*step_fast*/,
              ImGuiInputTextFlags flags) {
    return inputScalar(label, ImGuiDataType_S32, v, "%d", flags);
}

bool InputInt2(const char* label, int v[2], ImGuiInputTextFlags flags) {
    return multiComponent(label, 2, [&](int i) {
        return InputInt("", &v[i], 0, 0, flags);
    });
}

bool InputScalar(const char* label, ImGuiDataType data_type, void* p_data,
                 const void* /*p_step*/, const void* /*p_step_fast*/,
                 const char* format, ImGuiInputTextFlags flags) {
    return inputScalar(label, data_type, p_data, format, flags);
}

}  // namespace entity::ui
