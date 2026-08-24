/* SPDX-License-Identifier: MIT */
#ifndef HAX_ATOMICS_H
#define HAX_ATOMICS_H

/* C++ bridge over the C11 <stdatomic.h> spellings the codebase uses. The project
 * compiles as C++, where _Atomic and the C11 atomic free functions do not exist,
 * so this header supplies the same names on top of <atomic>. C translation units
 * (none remain) would still get the real <stdatomic.h>. */

#ifdef __cplusplus

#include <atomic>

using std::memory_order_acquire;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_seq_cst;

using atomic_bool = std::atomic<bool>;
using atomic_int = std::atomic<int>;
using atomic_size_t = std::atomic<size_t>;
using atomic_ulong = std::atomic<unsigned long>;

template <typename T> inline void atomic_init(T *p, typename T::value_type v)
{
    p->store(v, std::memory_order_relaxed);
}

template <typename T> inline auto atomic_load(const T *p)
{
    return p->load();
}

template <typename T> inline auto atomic_load_explicit(const T *p, std::memory_order order)
{
    return p->load(order);
}

template <typename T> inline void atomic_store(T *p, typename T::value_type v)
{
    p->store(v);
}

template <typename T> inline auto atomic_exchange(T *p, typename T::value_type v)
{
    return p->exchange(v);
}

template <typename T> inline auto atomic_fetch_add(T *p, typename T::value_type v)
{
    return p->fetch_add(v);
}

template <typename T>
inline auto atomic_fetch_add_explicit(T *p, typename T::value_type v, std::memory_order order)
{
    return p->fetch_add(v, order);
}

#else

#include <stdatomic.h>

#endif /* __cplusplus */

#endif /* HAX_ATOMICS_H */
