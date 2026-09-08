#pragma once

#include <QJSValue>

#include <cmath>
#include <optional>
#include <vector>

namespace wek::qml_helper
{

// One JS number the renderer can use.  QJSValue::isNumber() is true for NaN
// and Inf, so "is it a number" is not enough: a non-finite value reaching a
// uniform poisons it twice over — the GPU gets the NaN, and every later
// |new - cached| compare against it is false, so the slot never updates again.
inline std::optional<float> finiteJsNumber(const QJSValue& v) {
    if (! v.isNumber()) return std::nullopt;
    const double d = v.toNumber();
    if (! std::isfinite(d)) return std::nullopt;
    return static_cast<float>(d);
}

// Packs a script's return value into the float vector a material uniform
// takes.  Accepts a bare number, a plain array, or a Vec2/3/4-shaped object —
// the object form stops at the first missing component, so {x,y} yields two
// floats.
//
// A non-numeric, non-finite or over-long value rejects the WHOLE write rather
// than being dropped element-wise: dropping keeps a bad tick alive and
// silently changes the uniform's arity, which is just a different wrong value.
inline std::optional<std::vector<float>> packJsFloats(const QJSValue& value, int maxN) {
    std::vector<float> floats;
    if (value.isNumber()) {
        auto f = finiteJsNumber(value);
        if (! f) return std::nullopt;
        floats.push_back(*f);
    } else if (value.isArray()) {
        const int n = value.property("length").toInt();
        if (n <= 0 || n > maxN) return std::nullopt;
        floats.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            auto f = finiteJsNumber(value.property(static_cast<quint32>(i)));
            if (! f) return std::nullopt;
            floats.push_back(*f);
        }
    } else if (value.isObject()) {
        static const char* kComponents[] = { "x", "y", "z", "w" };
        for (int k = 0; k < 4 && k < maxN; k++) {
            QJSValue comp = value.property(kComponents[k]);
            if (comp.isUndefined()) break; // Vec2 has no z/w
            auto f = finiteJsNumber(comp);
            if (! f) return std::nullopt;
            floats.push_back(*f);
        }
    }
    if (floats.empty()) return std::nullopt;
    return floats;
}

} // namespace wek::qml_helper
