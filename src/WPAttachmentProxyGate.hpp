#pragma once
//
// Decision for redirecting a plain (effect-less) parented child's transform
// parent to an anchor proxy.  Pure boolean rule, extracted so it can be
// unit-tested without a scene parse (mirrors AttachmentLinkOrder.hpp).
//
// A plain child renders through its real parent SceneNode chain, which is
// correct UNLESS:
//   parentReset  — the parent's world node was reset to identity for the
//                  parent's own non-compose effect base pass, so the live
//                  chain no longer carries the parent's world.
//   boneOffset   — the child rigs into a named attachment on the parent's
//                  puppet whose bind-pose bone offset (boneWorld * attachment)
//                  is non-identity, so the bare parent*local chain misses it.
// Either way the child needs a proxy carrying parentWorld * boneOffset.
// Requires the parent's world to be known (parentWorldKnown); without it there
// is nothing to anchor to and we leave the child on its bare chain.
//

namespace wallpaper
{

inline bool plainChildNeedsAnchorProxy(bool parentWorldKnown, bool parentReset, bool boneOffset) {
    return parentWorldKnown && (parentReset || boneOffset);
}

} // namespace wallpaper
