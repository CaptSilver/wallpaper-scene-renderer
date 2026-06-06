#pragma once

#include <optional>

#include <QJSValue>
#include <QString>

namespace wek::qml_helper {

// Wallpaper Engine text-property `update(value)` semantics: the script returns
// the new text, OR returns undefined / null to mean "leave the text as it is"
// this tick.  Plenty of wallpapers lean on that — a log window that only appends
// a line once a second returns nothing on the in-between ticks, and a world-clock
// "month" layer only recomputes when the month actually rolls over.
//
// QJSValue::toString() turns an undefined return into the literal string
// "undefined" (and null into "null"), so writing that back clobbers the layer
// with garbage text.  Guard it here, the same way the origin/visible/colour
// property paths already do.
//
// Returns the new text when the script produced a real value, or std::nullopt
// when the caller should keep the current text untouched.
inline std::optional<QString> textUpdateResult(const QJSValue& result) {
    if (result.isUndefined() || result.isNull()) return std::nullopt;
    return result.toString();
}

} // namespace wek::qml_helper
