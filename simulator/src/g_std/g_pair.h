#ifndef G_PAIR_H_
#define G_PAIR_H_

#include <utility>
#include "g_std/stl_galloc.h"

template <typename T1, typename T2>
class g_pair : public std::pair<T1, T2>, public GlobAlloc
{
};

// Custom make_pair equivalent
template <typename T1, typename T2>
g_pair<T1, T2> g_make_pair(T1&& x, T2&& y)
{
    return g_pair<T1, T2>(std::forward<T1>(x), std::forward<T2>(y));
}

#endif