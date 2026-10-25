// Stand-in for the Concurrency Runtime's concurrent_vector on compilers without
// PPL, for CBPC. Like the original, growing it keeps its elements in place.
#pragma once

#include <deque>
#include <memory>

namespace concurrency
{
	template <class T, class Allocator = std::allocator<T>>
	class concurrent_vector : public std::deque<T, Allocator>
	{
	public:
		using std::deque<T, Allocator>::deque;
	};
}
