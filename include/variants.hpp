// variants.hpp — enumerate all 3 x 4 = 12 UnionFind variants at compile time.
//
// Usage:
//   uf::for_each_variant([&](auto tag) {
//       using UF = typename decltype(tag)::type;
//       UF u(n);
//       ...
//   });
//
// for_each_variant<uf::CountStats>(f) gives the instrumented versions.

#pragma once

#include <type_traits>

#include "union_find.hpp"

namespace uf {

template <class T>
struct type_tag { using type = T; };

template <class Link, class Stats, class F>
void for_each_find(F& f) {
    f(type_tag<UnionFind<Link, FindNone, Stats>>{});
    f(type_tag<UnionFind<Link, FindFull, Stats>>{});
    f(type_tag<UnionFind<Link, FindHalving, Stats>>{});
    f(type_tag<UnionFind<Link, FindSplitting, Stats>>{});
}

template <class Stats = NoStats, class F>
void for_each_variant(F&& f) {
    for_each_find<LinkNaive, Stats>(f);
    for_each_find<LinkByRank, Stats>(f);
    for_each_find<LinkBySize, Stats>(f);
}

// The only variant with no defence against deep trees. It is O(n) per find
// on chain-like inputs, so the harness caps its problem size.
template <class UF>
inline constexpr bool is_unprotected =
    std::is_same_v<typename UF::link_type, LinkNaive> &&
    std::is_same_v<typename UF::find_type, FindNone>;

}  // namespace uf
