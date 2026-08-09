#ifndef __MADCDIS_POD_ALLOC_H
#define __MADCDIS_POD_ALLOC_H 1

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// default_init_allocator — an allocator adaptor under which
// vector::resize()'s value-initialization becomes DEFAULT-initialization:
// a no-op for trivially-constructible elements. A decode DESTINATION is
// sized once and fully overwritten by the decoder immediately after, so
// the zero-fill is pure waste (callgrind: __memset was still 12.9% of a
// packed <string> compile AFTER the staging buffers were eliminated —
// every remaining resize-then-decode paid one full memset of the payload).
// Use ONLY for buffers whose every element is written before it is read;
// a partially-decoded buffer on a failure path must be discarded, never
// read.

namespace madc {
namespace dis {

template<class T, class A = std::allocator<T> >
struct default_init_allocator : public A
{
	typedef std::allocator_traits<A> a_t;
	template<class U> struct rebind
	{
		typedef default_init_allocator<
			U, typename a_t::template rebind_alloc<U> > other;
	};
	default_init_allocator() {}
	template<class U, class B>
	default_init_allocator(const default_init_allocator<U, B> &o)
		: A(o) {}
	template<class U>
	void construct(U *p) { ::new (static_cast<void *>(p)) U; }
	template<class U, class... Args>
	void construct(U *p, Args &&...args)
	{
		::new (static_cast<void *>(p))
			U(std::forward<Args>(args)...);
	}
};

// Stateless: every instance is interchangeable.
template<class T, class A, class U, class B>
inline bool operator==(const default_init_allocator<T, A> &,
		       const default_init_allocator<U, B> &) { return true; }
template<class T, class A, class U, class B>
inline bool operator!=(const default_init_allocator<T, A> &,
		       const default_init_allocator<U, B> &) { return false; }

// A decode destination: size once, decode over it, then read.
template<class T>
using decode_vector = std::vector<T, default_init_allocator<T> >;
typedef decode_vector<uint8_t> decode_bytes;

} // namespace dis
} // namespace madc

#endif // __MADCDIS_POD_ALLOC_H
