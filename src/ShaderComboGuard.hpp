#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace wallpaper
{

// Tracks the preprocessor conditionals open around the line currently being
// scanned.
//
// A shader may declare a texture uniform inside the very conditional its combo
// controls:
//
//     #if MASK == 1
//     uniform sampler2D g_Texture1; // {"combo":"MASK","default":"util/black"}
//     #endif
//
// That declaration only exists once MASK is on, so its default cannot be the
// evidence that turns MASK on — the reasoning is circular, and for the spin
// effect it is actively wrong: a black mask makes the blend return the
// untransformed sample, so the layer stops spinning.
class ShaderComboGuard {
public:
    // Feed every line of the shader, in order, before inspecting it.
    void OnLine(std::string_view line) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos) return;
        const std::string_view st = line.substr(first);
        if (! st.starts_with("#")) return;

        if (st.starts_with("#endif")) {
            if (! m_open.empty()) m_open.pop_back();
        } else if (st.starts_with("#elif")) {
            if (! m_open.empty())
                m_open.back() = std::string(st);
            else
                m_open.emplace_back(st);
        } else if (st.starts_with("#if")) {
            m_open.emplace_back(st);
        }
    }

    // True when an open conditional tests `combo` by name.
    bool Guards(std::string_view combo) const {
        if (combo.empty()) return false;
        for (const auto& cond : m_open) {
            if (MentionsIdentifier(cond, combo)) return true;
        }
        return false;
    }

private:
    // Whole-identifier match, so `#if MASKED` does not guard `MASK`.
    static bool MentionsIdentifier(std::string_view text, std::string_view name) {
        for (std::size_t at = text.find(name); at != std::string_view::npos;
             at             = text.find(name, at + 1)) {
            const bool        left_ok  = at == 0 || ! IsIdentChar(text[at - 1]);
            const std::size_t after    = at + name.size();
            const bool        right_ok = after >= text.size() || ! IsIdentChar(text[after]);
            if (left_ok && right_ok) return true;
        }
        return false;
    }

    static bool IsIdentChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    }

    std::vector<std::string> m_open;
};

} // namespace wallpaper
