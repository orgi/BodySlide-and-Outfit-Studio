// Stand-in for the Concurrency Runtime's concurrent_unordered_map on compilers
// without PPL, for CBPC. Like the original, inserting keeps the elements in place.
#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace concurrency
{
	template <class Key, class Value, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>, class Allocator = std::allocator<std::pair<const Key, Value>>>
	class concurrent_unordered_map : public std::unordered_map<Key, Value, Hash, KeyEqual, Allocator>
	{
		using Base = std::unordered_map<Key, Value, Hash, KeyEqual, Allocator>;

	public:
		using Base::Base;

		typename Base::size_type unsafe_erase(const Key& a_key) { return Base::erase(a_key); }
		typename Base::iterator unsafe_erase(typename Base::const_iterator a_where) { return Base::erase(a_where); }
	};
}
