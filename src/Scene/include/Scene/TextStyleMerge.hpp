#pragma once
#include <functional>
#include <string>

namespace wallpaper
{

// Per-id queued text-style update (thisLayer.horizontalalign/verticalalign/font).
// Empty-string fields mean "no change" so each SceneScript property setter can
// post independently without clobbering siblings.
struct PendingTextStyleUpdate {
    std::string halign;
    std::string valign;
    std::string fontName;
};

// Setter-side merge: fold one posted (halign,valign,fontName) onto the queued
// entry, per-field, treating empty as "leave unchanged".  Successive same-id
// posts within a tick accumulate.
inline void mergeTextStyle(PendingTextStyleUpdate& cur, const std::string& halign,
                           const std::string& valign, const std::string& fontName) {
    if (! halign.empty()) cur.halign = halign;
    if (! valign.empty()) cur.valign = valign;
    if (! fontName.empty()) cur.fontName = fontName;
}

// Apply-side target: a text layer's live style + resolved font bytes.
struct TextStyleTarget {
    std::string halign;
    std::string valign;
    std::string fontName;
    std::string fontData;
};

// Apply one queued style onto a target with a value-changed gate: a field only
// changes (and only flags a change) when the posted value is non-empty AND
// differs from the current.  A font change takes effect only when resolveFont
// yields non-empty bytes (a missing font leaves name+data unchanged; resolveFont
// is where the caller logs the miss).  Returns true iff anything changed (the
// caller uses that to set textStyleDirty).
inline bool applyTextStyle(const PendingTextStyleUpdate& style, TextStyleTarget& tl,
                           const std::function<std::string(const std::string&)>& resolveFont) {
    bool anyChange = false;
    if (! style.halign.empty() && style.halign != tl.halign) {
        tl.halign = style.halign;
        anyChange = true;
    }
    if (! style.valign.empty() && style.valign != tl.valign) {
        tl.valign = style.valign;
        anyChange = true;
    }
    if (! style.fontName.empty() && style.fontName != tl.fontName) {
        std::string bytes = resolveFont(style.fontName);
        if (! bytes.empty()) {
            tl.fontName = style.fontName;
            tl.fontData = std::move(bytes);
            anyChange   = true;
        }
    }
    return anyChange;
}

} // namespace wallpaper
