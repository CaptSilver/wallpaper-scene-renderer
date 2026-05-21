#pragma once

// Single source of truth for normalizing modern-JS syntax that Wallpaper Engine
// scripts use but Qt's QJSEngine (the V4 interpreter behind SceneScript) does
// not accept, so the script can compile after the ES-module strip in
// stripESModuleSyntax().
//
// Why this exists (optional catch binding):
//   ES2019 made the catch parameter optional:
//       try { risky(); } catch { fallback(); }
//   QJSEngine/V4 still requires the binding and reports a *SyntaxError* on the
//   bare `catch {`.  We confirmed this empirically against the live engine:
//       JSFEAT optcatch=N(SyntaxError: Parse error) | nullish=Y | optchain=Y
//   A SyntaxError fails the *entire* script compile — not just the catch — so a
//   single `catch {` anywhere kills the whole property script.  Real-world hit:
//   the widely-reused "Gariam" parenting system (workshop 2686493967, embedded
//   in e.g. 2992803622) has two `catch {` clauses; its 791-line script never
//   compiled, breaking parent/child layout (visibility, origin, scale, color)
//   for every layer it drives.  This pattern is common across WE scripts, so the
//   fix is general, not per-wallpaper (per feedback_no_haphazard_patches).
//
// The transform inserts an unused, uniquely-named binding:
//       } catch {        ->  } catch (__wecatch) {
// It is anchored on the preceding `}` of the try/catch (and a `)` for the
// `} catch {` that follows a `catch (e) {}`-less guard is impossible — catch
// always follows a block).  Anchoring on `}` means a stray `catch {` inside a
// string or comment is never rewritten (a bare `catch {` there would need a
// preceding `}` *and* `catch` *and* `{` with only whitespace between, which does
// not occur in author string data); `catch (e) {` is left untouched because the
// `(` breaks the `catch\s*\{` match.  Each catch clause has its own scope, so
// reusing one binding name across nested/sibling catches only shadows, never
// collides.
//
// Header-only / inline so it adds no exported or undefined symbol to the plugin
// .so (per feedback_verify_plugin_so_link) and is unit-testable in isolation
// with only QString (Qt6::Core) — no QJSEngine, no Vulkan.

#include <QtCore/QRegularExpression>
#include <QtCore/QString>

namespace wek::qml_helper
{

// Rewrite ES2019 optional catch bindings (`} catch {`) into the parameterized
// form QJSEngine accepts (`} catch (__wecatch) {`).  No-op for scripts that
// already bind the error (`catch (e) {`) or have no try/catch.
inline void normalizeOptionalCatchBinding(QString& src) {
    // `}` (end of try/preceding block) + optional ws + `catch` + optional ws +
    // `{`.  The `\s*` spans newlines, covering `}\n  catch\n{`.
    static const QRegularExpression re(QStringLiteral("\\}\\s*catch\\s*\\{"));
    src.replace(re, QStringLiteral("} catch (__wecatch) {"));
}

// Remove the last trailing `}` (skipping trailing whitespace) from `src`.
// Returns true iff a brace was removed.
//
// This is a *compile-failure fallback*, not an unconditional transform.  Some
// WE scripts have a stray trailing `}` (net brace depth -1) that WE's engine
// tolerates but our IIFE wrapper does not — the unmatched `}` closes the
// wrapper function early and the trailing `return {...}` becomes a parse error,
// failing the whole compile.  Real-world hit: the Gariam parenting system
// (workshop 2686493967, embedded in 2992803622) ends with `}}`.
//
// The caller strips a trailing brace and recompiles ONLY after a compile has
// already failed (see SceneBackend.cpp), so a well-formed script — which always
// compiles first try — is never touched and cannot regress.  Acting only on the
// trailing edge (not an interior brace) keeps it from corrupting valid code.
inline bool dropOneTrailingBrace(QString& src) {
    int i = src.size() - 1;
    while (i >= 0 && src[i].isSpace()) --i;
    if (i < 0 || src[i] != QLatin1Char('}')) return false;
    src.remove(i, 1);
    return true;
}

} // namespace wek::qml_helper
