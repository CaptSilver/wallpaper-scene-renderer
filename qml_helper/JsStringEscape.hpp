#pragma once

// Single source of truth for escaping a string so it can be embedded inside a
// JavaScript **single-quoted** string literal (e.g. `JSON.parse('...')`) that
// is then fed to a QJSEngine.
//
// Why this exists (F18): the historical idiom across SceneBackend.cpp was
//   json.replace('\\', "\\\\"); json.replace('\'', "\\'");
// i.e. it escaped only the backslash and the single quote.  That is NOT enough
// for a JS string literal:
//   * U+2028 (LINE SEPARATOR) and U+2029 (PARAGRAPH SEPARATOR) are *legal
//     literal characters inside a JSON string* but are *line terminators*
//     inside a JS source string literal, so a raw one turns
//     `JSON.parse('...<U+2028>...')` into a SyntaxError.
//   * A raw newline (U+000A) or carriage return (U+000D) embedded in the
//     interpolated text likewise terminates the literal early.
// A SyntaxError here is swallowed (the surrounding try/catch or an ignored
// error return), so the affected script's scriptProperties / init / user-props
// silently fail to apply — a subtly-wrong or dead wallpaper with nothing in the
// log.  This helper escapes the full set so the literal can never be broken by
// author data.
//
// Header-only / inline so it adds no exported or undefined symbol to the
// plugin .so (per feedback_verify_plugin_so_link) and is unit-testable in
// isolation with only QString (Qt6::Core) — no QJSEngine, no Vulkan.

#include <QtCore/QChar>
#include <QtCore/QString>

#include <cstdint>
#include <vector>

namespace wek::qml_helper
{

// Escape `in` for safe interpolation into a JS single-quoted string literal.
//
// Handled, in a single forward pass (no multi-replace ordering hazard):
//   '\\' -> "\\\\"   (backslash first, conceptually — the single pass means a
//                     just-emitted backslash is never re-scanned)
//   '\'' -> "\\'"
//   '\n' -> "\\n"
//   '\r' -> "\\r"
//   '\t' -> "\\t"
//   U+2028 -> "\\u2028"
//   U+2029 -> "\\u2029"
//   any other C0 control char (< 0x20) -> "\\uXXXX"
// All other characters (including non-control non-ASCII) pass through verbatim;
// for special-character-free input the output is byte-identical to the old
// `\\`+`'` escaping, so existing wallpapers are unaffected.
inline QString escapeForJsSingleQuoted(const QString& in) {
    QString out;
    out.reserve(in.size() + 8);
    for (const QChar ch : in) {
        const char16_t u = ch.unicode();
        switch (u) {
        case u'\\': out += QLatin1String("\\\\"); break;
        case u'\'': out += QLatin1String("\\'"); break;
        case u'\n': out += QLatin1String("\\n"); break;
        case u'\r': out += QLatin1String("\\r"); break;
        case u'\t': out += QLatin1String("\\t"); break;
        case u' ': out += QLatin1String("\\u2028"); break;
        case u' ': out += QLatin1String("\\u2029"); break;
        default:
            if (u < 0x20) {
                // Remaining C0 controls (e.g. \b, \f, NUL, \v) as \uXXXX.
                static const char kHex[] = "0123456789abcdef";
                out += QLatin1String("\\u00");
                out += QLatin1Char(kHex[(u >> 4) & 0xF]);
                out += QLatin1Char(kHex[u & 0xF]);
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

// Build the JS expression that looks up a scene layer by name, with the name
// fully escaped via escapeForJsSingleQuoted.  Single source of truth so the
// three `thisScene.getLayer('<name>')` call sites in SceneBackend.cpp cannot
// drift in their escaping (two were entirely unescaped before F18; the sound
// path had a bespoke `'`-only escape).
//
// Returns e.g. `thisScene.getLayer('My\'Layer')`.
inline QString jsLayerLookupExpr(const QString& layerName) {
    return QStringLiteral("thisScene.getLayer('%1')").arg(escapeForJsSingleQuoted(layerName));
}

// Build the JS expression for the initial value handed to a shader-value
// script's `init(value)`.  Wallpaper Engine seeds init with the material
// constant's declared value, shaped to its component count: a plain number for
// a scalar (alpha), or a VecN for a vector (scale / colour).  Scripts branch on
// the shape — e.g. `value.hasOwnProperty('x')` to tell vector from scalar — so
// passing `undefined` (the old behaviour) threw "Cannot call method
// 'hasOwnProperty' of undefined" and the script's init never ran, breaking the
// media-info fade/scale on Totoro (2891663007).  This mirrors the per-frame
// update arg shaping (the m_vecNFn calls) so init and update see the same type.
// VecN here resolves to the engine's global Vec2/Vec3/Vec4 ctors (kVecClassesJs),
// which are new-safe when called without `new`.
inline QString shaderValueInitExpr(const std::vector<float>& initialValue, int argShape) {
    auto num = [&](std::size_t i) -> QString {
        double v = i < initialValue.size() ? (double)initialValue[i] : 0.0;
        return QString::number(v, 'g', 9);
    };
    if (argShape <= 1) return num(0);
    if (argShape == 2) return QStringLiteral("Vec2(%1,%2)").arg(num(0), num(1));
    if (argShape == 3) return QStringLiteral("Vec3(%1,%2,%3)").arg(num(0), num(1), num(2));
    return QStringLiteral("Vec4(%1,%2,%3,%4)").arg(num(0), num(1), num(2), num(3));
}

} // namespace wek::qml_helper
