#pragma once

#include <map>
#include <set>
#include <unordered_map>

namespace wallpaper
{

template<class Key, class Value>
using Map = std::map<Key, Value, std::less<>>;

template<class Key>
using Set = std::set<Key, std::less<>>;

template<class Key, class Value, class KeyLike, class Allocator>
inline bool exists(const std::map<Key, Value, std::less<>, Allocator>& m,
                   const KeyLike&                                      key) noexcept {
    auto iter = m.find(key);
    return iter != m.end();
}

template<class Key, class KeyLike, class Allocator>
inline bool exists(const std::set<Key, std::less<>, Allocator>& m, const KeyLike& key) noexcept {
    auto iter = m.find(key);
    return iter != m.end();
}

// std::unordered_map overload — Scene::cameras / renderTargets are
// unordered_map, which the std::map overload above does not match.  Lets the
// camera-lookup guards read identically to the existing exists() call sites.
template<class Key, class Value, class Hash, class Eq, class Allocator, class KeyLike>
inline bool exists(const std::unordered_map<Key, Value, Hash, Eq, Allocator>& m,
                   const KeyLike&                                             key) noexcept {
    auto iter = m.find(key);
    return iter != m.end();
}

} // namespace wallpaper