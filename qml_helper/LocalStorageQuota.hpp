#pragma once

// Quota helper for the SceneScript `localStorage` bridge in SceneBackend.cpp.
// Browsers throw QuotaExceededError once a single setItem exceeds ~1 MiB or
// the per-origin aggregate exceeds ~10 MiB; the WE plugin's bridge had no
// cap at all, so a runaway / hostile script could fill RAM and then write
// 100 GB of JSON to $XDG_CACHE_HOME/wallpaper-scene-renderer/.  Since the
// user's ~/.cache no-delete rule means manual `rm` is the only recovery,
// the unbounded write is weaponizable; this header centralizes the cap
// math + the cached serialized-byte counter that lets `lsSet` make a fast
// projected-size check.
//
// Header-only / inline so it adds no exported or undefined symbol to the
// plugin .so (per feedback_verify_plugin_so_link) and is unit-testable in
// isolation with only Qt6::Core types — no QJSEngine, no Vulkan, no
// SceneBackend link.  scenescript_tests links Qt6::Core + Qt6::Qml, which
// is sufficient for the per-value / per-scope contract pinned here; the
// production-wiring trip-throw + flush-on-first-write recovery are
// validated end-to-end via sceneviewer-script.

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/qglobal.h>
#include <optional>

namespace wek::qml_helper
{

// Browser-parity caps.  Per-value matches Chrome/Firefox's "single setItem
// must be < ~1 MiB" limit; per-scope matches the documented per-origin
// 10 MiB quota.  Pre-load cap (2x scope) tolerates pre-existing slightly-
// oversized files written by pre-cap versions of this codebase — the empty
// load + flush-on-first-write recovers without manual rm.
inline constexpr qsizetype kMaxLsValueBytes   = 1 * 1024 * 1024;
inline constexpr qsizetype kMaxLsScopeBytes   = 10 * 1024 * 1024;
inline constexpr qsizetype kMaxLsPreloadBytes = 2 * kMaxLsScopeBytes;

// Serializing a stand-alone QJsonValue requires wrapping; the QJsonArray
// round-trip is the canonical idiom.  Same units as the on-disk flush
// (QJsonDocument(scope).toJson(Compact)) so projected aggregate compares
// apples-to-apples with kMaxLsScopeBytes.
inline qsizetype SerializedSize(const QJsonValue& v) {
    return QJsonDocument(QJsonArray { v }).toJson(QJsonDocument::Compact).size();
}

// Quota tracker for one localStorage scope (LOCATION_GLOBAL or
// LOCATION_SCREEN).  Owns a cached serialized-byte count + invalidation
// hooks tied to insert / remove / clear events.  The serialized-bytes
// metric is QJsonDocument(QJsonObject).toJson(Compact).size() — same units
// as the on-disk flush via flushLocalStorage's writeAtomic, and the same
// units SerializedSize returns for a single proposed value.
//
// Caller-mutation contract: the helper does NOT touch the scope itself.
// The caller checks via Check(...), then on Ok performs scope.insert(...)
// (or scope.remove / scope = {}) and Invalidate()s the cache so the next
// Check recomputes.  This keeps the helper Qt-types-only (no friend
// declarations against SceneObject) and trivially testable.
class LocalStorageQuota {
public:
    enum class Result
    {
        Ok,
        ValueTooLarge,
        ScopeWouldOverflow
    };

    // Pre-check.  Serializes the proposed value, compares against per-
    // value cap, then compares projected new-scope-bytes against the
    // per-scope cap.  Caller is responsible for the actual QJsonObject
    // mutation on Ok; on reject, scope is unchanged (the helper never
    // mutates).  The key_overhead approximation (key.size + 8) covers
    // the JSON framing for "<key>":<value>, — over-approximates by a
    // few bytes (UTF-8 expansion of the key, comma/colon/quotes), which
    // is fine since the cap is generous.
    Result Check(const QJsonObject& scope, const QString& key, const QJsonValue& value) const {
        const qsizetype value_size = SerializedSize(value);
        if (value_size > kMaxLsValueBytes) return Result::ValueTooLarge;
        const qsizetype current      = CachedBytes(scope);
        const qsizetype key_overhead = key.size() + 8;
        const qsizetype projected    = current + value_size + key_overhead;
        if (projected > kMaxLsScopeBytes) return Result::ScopeWouldOverflow;
        return Result::Ok;
    }

    // Reset the cache.  Called by the caller after any insert / remove /
    // clear so the next Check serializes the (now-stale) scope and
    // re-caches.  Lazy recompute is cheaper than per-call recomputation
    // given write rates < 100 Hz in practice, and accommodates replace-
    // of-existing-key cases where the old value's size isn't easily
    // known.
    void Invalidate() { m_cached_bytes.reset(); }

    // Read-through cache.  Serializes scope on miss.  const because the
    // cache is mutable — invalidation is intentional from the caller side.
    qsizetype CachedBytes(const QJsonObject& scope) const {
        if (! m_cached_bytes.has_value()) {
            m_cached_bytes = QJsonDocument(scope).toJson(QJsonDocument::Compact).size();
        }
        return *m_cached_bytes;
    }

    // Test introspection.
    static constexpr qsizetype CapValue() { return kMaxLsValueBytes; }
    static constexpr qsizetype CapScope() { return kMaxLsScopeBytes; }

private:
    mutable std::optional<qsizetype> m_cached_bytes;
};

} // namespace wek::qml_helper
