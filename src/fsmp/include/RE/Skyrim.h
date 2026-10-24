// Stand-in for CommonLibSSE's RE/Skyrim.h, used to build FSMP outside of the
// game. It declares the engine types FSMP uses with the members it accesses.
// The scene graph types behave like the engine's (reference counting, world
// transform updates, name lookup through the case-insensitive string pool);
// the standalone layer (FsmpScene) fills them from NIF files.
#pragma once

#include <REL/Relocation.h>
#include <SKSE/Logger.h>

// CommonLibSSE's headers make these available to every file that includes them.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cmath>
#include <concepts>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iomanip>
#include <ios>
#include <istream>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numbers>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <source_location>
#include <span>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace RE
{
	using FormID = std::uint32_t;

	class Actor;
	class BSFadeNode;
	class BSFaceGenNiNode;
	class BSGeometry;
	class BSTriShape;
	class BSDynamicTriShape;
	class NiAVObject;
	class NiExtraData;
	class NiGeometry;
	class NiNode;
	class NiSkinInstance;
	class TESObjectREFR;
	class BGSHeadPart;
	class TESObjectARMA;
	class ActorEquipManager;
	class BGSEquipSlot;
	class BipedAnim;
	class BSBethesdaPlatform;
	class ExtraDataList;
	struct StaticFunctionTag;

	// -----------------------------------------------------------------------
	// Strings
	// -----------------------------------------------------------------------

	namespace detail
	{
		// Returns the pool entry for a string. Like the engine's string pool the
		// lookup ignores case and keeps the first spelling it was given. Empty
		// strings have no entry (nullptr).
		const char* PoolString(std::string_view a_string);

		template <class CharT>
		class BSFixedString
		{
			static_assert(std::is_same_v<CharT, char>);

		public:
			using value_type = CharT;
			using size_type = std::uint32_t;

			BSFixedString() noexcept = default;
			BSFixedString(const BSFixedString&) noexcept = default;
			BSFixedString(BSFixedString&&) noexcept = default;
			BSFixedString(const value_type* a_string) :
				_data(a_string ? PoolString(a_string) : nullptr) {}
			BSFixedString(std::string_view a_string) :
				_data(PoolString(a_string)) {}
			BSFixedString(const std::string& a_string) :
				_data(PoolString(a_string)) {}

			BSFixedString& operator=(const BSFixedString&) noexcept = default;
			BSFixedString& operator=(BSFixedString&&) noexcept = default;
			BSFixedString& operator=(const value_type* a_string)
			{
				_data = a_string ? PoolString(a_string) : nullptr;
				return *this;
			}
			BSFixedString& operator=(std::string_view a_string)
			{
				_data = PoolString(a_string);
				return *this;
			}
			BSFixedString& operator=(const std::string& a_string)
			{
				_data = PoolString(a_string);
				return *this;
			}

			[[nodiscard]] const value_type* data() const noexcept { return _data ? _data : ""; }
			[[nodiscard]] const value_type* c_str() const noexcept { return data(); }
			[[nodiscard]] size_type size() const noexcept { return _data ? static_cast<size_type>(std::strlen(_data)) : 0; }
			[[nodiscard]] size_type length() const noexcept { return size(); }
			[[nodiscard]] bool empty() const noexcept { return size() == 0; }
			[[nodiscard]] bool contains(std::string_view a_rhs) const { return std::string_view(*this).find(a_rhs) != std::string_view::npos; }

			[[nodiscard]] operator std::string_view() const noexcept { return { data(), size() }; }

			[[nodiscard]] friend bool operator==(const BSFixedString& a_lhs, const BSFixedString& a_rhs) noexcept { return a_lhs._data == a_rhs._data; }
			[[nodiscard]] friend bool operator==(const BSFixedString& a_lhs, std::string_view a_rhs) noexcept { return EqualsIgnoreCase(a_lhs, a_rhs); }
			[[nodiscard]] friend bool operator==(const BSFixedString& a_lhs, const value_type* a_rhs) noexcept { return EqualsIgnoreCase(a_lhs, a_rhs ? std::string_view(a_rhs) : std::string_view()); }
			[[nodiscard]] friend bool operator==(const BSFixedString& a_lhs, const std::string& a_rhs) noexcept { return EqualsIgnoreCase(a_lhs, a_rhs); }

		private:
			static bool EqualsIgnoreCase(std::string_view a_lhs, std::string_view a_rhs) noexcept
			{
				return a_lhs.size() == a_rhs.size() &&
				       std::equal(a_lhs.begin(), a_lhs.end(), a_rhs.begin(), [](char a, char b) {
						   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
					   });
			}

			const value_type* _data{ nullptr };
		};
	}

	using BSFixedString = detail::BSFixedString<char>;

	// -----------------------------------------------------------------------
	// Containers and smart pointers
	// -----------------------------------------------------------------------

	template <class T>
	class BSTArray
	{
	public:
		using value_type = T;
		using size_type = std::uint32_t;
		using iterator = typename std::vector<T>::iterator;
		using const_iterator = typename std::vector<T>::const_iterator;

		[[nodiscard]] size_type size() const noexcept { return static_cast<size_type>(_data.size()); }
		[[nodiscard]] bool empty() const noexcept { return _data.empty(); }
		[[nodiscard]] T& operator[](size_type a_pos) { return _data[a_pos]; }
		[[nodiscard]] const T& operator[](size_type a_pos) const { return _data[a_pos]; }
		[[nodiscard]] T* data() noexcept { return _data.data(); }
		[[nodiscard]] const T* data() const noexcept { return _data.data(); }
		[[nodiscard]] T& front() { return _data.front(); }
		[[nodiscard]] T& back() { return _data.back(); }
		iterator begin() noexcept { return _data.begin(); }
		iterator end() noexcept { return _data.end(); }
		const_iterator begin() const noexcept { return _data.begin(); }
		const_iterator end() const noexcept { return _data.end(); }
		void push_back(const T& a_value) { _data.push_back(a_value); }
		void push_back(T&& a_value) { _data.push_back(std::move(a_value)); }
		template <class... Args>
		T& emplace_back(Args&&... a_args) { return _data.emplace_back(std::forward<Args>(a_args)...); }
		void resize(size_type a_size) { _data.resize(a_size); }
		void clear() noexcept { _data.clear(); }
		iterator erase(const_iterator a_pos) { return _data.erase(a_pos); }

	private:
		std::vector<T> _data;
	};

	template <class T>
	class BSSimpleList
	{
	public:
		using iterator = typename std::vector<T>::iterator;
		using const_iterator = typename std::vector<T>::const_iterator;

		iterator begin() noexcept { return _data.begin(); }
		iterator end() noexcept { return _data.end(); }
		const_iterator begin() const noexcept { return _data.begin(); }
		const_iterator end() const noexcept { return _data.end(); }
		void push_front(const T& a_value) { _data.insert(_data.begin(), a_value); }
		void push_back(const T& a_value) { _data.push_back(a_value); }
		[[nodiscard]] bool empty() const noexcept { return _data.empty(); }

	private:
		std::vector<T> _data;
	};

	// Pointer to an object counted with IncRefCount/DecRefCount (NiRefObject and TESObjectREFR).
	template <class T>
	class NiPointer
	{
	public:
		using element_type = T;

		constexpr NiPointer() noexcept = default;
		constexpr NiPointer(std::nullptr_t) noexcept {}

		template <class Y, std::enable_if_t<std::is_convertible_v<Y*, T*>, int> = 0>
		explicit NiPointer(Y* a_rhs) :
			_ptr(a_rhs)
		{
			Attach();
		}

		NiPointer(const NiPointer& a_rhs) :
			_ptr(a_rhs._ptr)
		{
			Attach();
		}

		template <class Y, std::enable_if_t<std::is_convertible_v<Y*, T*>, int> = 0>
		NiPointer(const NiPointer<Y>& a_rhs) :
			_ptr(a_rhs.get())
		{
			Attach();
		}

		NiPointer(NiPointer&& a_rhs) noexcept :
			_ptr(std::exchange(a_rhs._ptr, nullptr)) {}

		~NiPointer() { Detach(); }

		NiPointer& operator=(const NiPointer& a_rhs)
		{
			if (this != std::addressof(a_rhs))
				reset(a_rhs._ptr);
			return *this;
		}

		template <class Y, std::enable_if_t<std::is_convertible_v<Y*, T*>, int> = 0>
		NiPointer& operator=(const NiPointer<Y>& a_rhs)
		{
			reset(a_rhs.get());
			return *this;
		}

		NiPointer& operator=(NiPointer&& a_rhs) noexcept
		{
			if (this != std::addressof(a_rhs)) {
				Detach();
				_ptr = std::exchange(a_rhs._ptr, nullptr);
			}
			return *this;
		}

		NiPointer& operator=(std::nullptr_t)
		{
			reset();
			return *this;
		}

		void reset() { reset(static_cast<T*>(nullptr)); }

		template <class Y, std::enable_if_t<std::is_convertible_v<Y*, T*>, int> = 0>
		void reset(Y* a_ptr)
		{
			T* const old = _ptr;
			_ptr = a_ptr;
			Attach();
			if (old)
				old->DecRefCount();
		}

		[[nodiscard]] constexpr T* get() const noexcept { return _ptr; }
		[[nodiscard]] T& operator*() const noexcept { return *_ptr; }
		[[nodiscard]] T* operator->() const noexcept { return _ptr; }
		[[nodiscard]] explicit constexpr operator bool() const noexcept { return _ptr != nullptr; }

		template <class Y>
		[[nodiscard]] friend bool operator==(const NiPointer& a_lhs, const NiPointer<Y>& a_rhs) noexcept { return a_lhs.get() == a_rhs.get(); }
		[[nodiscard]] friend bool operator==(const NiPointer& a_lhs, std::nullptr_t) noexcept { return !a_lhs; }

	private:
		void Attach()
		{
			if (_ptr)
				_ptr->IncRefCount();
		}

		void Detach()
		{
			if (_ptr)
				std::exchange(_ptr, nullptr)->DecRefCount();
		}

		T* _ptr{ nullptr };
	};

	class BSIntrusiveRefCounted
	{
	public:
		std::uint32_t IncRef() const { return ++_refCount; }
		std::uint32_t DecRef() const { return --_refCount; }

		mutable std::atomic<std::uint32_t> _refCount{ 0 };

	protected:
		BSIntrusiveRefCounted() = default;
		BSIntrusiveRefCounted(const BSIntrusiveRefCounted&) noexcept {}
		BSIntrusiveRefCounted& operator=(const BSIntrusiveRefCounted&) noexcept { return *this; }
	};

	// Pointer to an object counted with IncRef/DecRef; it deletes the object when the count drops to 0.
	template <class T>
	class BSTSmartPointer
	{
	public:
		using element_type = T;

		constexpr BSTSmartPointer() noexcept = default;
		constexpr BSTSmartPointer(std::nullptr_t) noexcept {}

		template <class Y, std::enable_if_t<std::is_convertible_v<Y*, T*>, int> = 0>
		explicit BSTSmartPointer(Y* a_rhs) :
			_ptr(a_rhs)
		{
			Attach();
		}

		BSTSmartPointer(const BSTSmartPointer& a_rhs) :
			_ptr(a_rhs._ptr)
		{
			Attach();
		}

		template <class Y, std::enable_if_t<std::is_convertible_v<Y*, T*>, int> = 0>
		BSTSmartPointer(const BSTSmartPointer<Y>& a_rhs) :
			_ptr(a_rhs.get())
		{
			Attach();
		}

		BSTSmartPointer(BSTSmartPointer&& a_rhs) noexcept :
			_ptr(std::exchange(a_rhs._ptr, nullptr)) {}

		template <class Y, std::enable_if_t<std::is_convertible_v<Y*, T*>, int> = 0>
		BSTSmartPointer(BSTSmartPointer<Y>&& a_rhs) :
			_ptr(a_rhs.get())
		{
			Attach();
			a_rhs.reset();
		}

		~BSTSmartPointer() { Detach(); }

		BSTSmartPointer& operator=(const BSTSmartPointer& a_rhs)
		{
			if (this != std::addressof(a_rhs))
				reset(a_rhs._ptr);
			return *this;
		}

		template <class Y, std::enable_if_t<std::is_convertible_v<Y*, T*>, int> = 0>
		BSTSmartPointer& operator=(const BSTSmartPointer<Y>& a_rhs)
		{
			reset(a_rhs.get());
			return *this;
		}

		BSTSmartPointer& operator=(BSTSmartPointer&& a_rhs) noexcept
		{
			if (this != std::addressof(a_rhs)) {
				Detach();
				_ptr = std::exchange(a_rhs._ptr, nullptr);
			}
			return *this;
		}

		BSTSmartPointer& operator=(std::nullptr_t)
		{
			reset();
			return *this;
		}

		void reset() { reset(static_cast<T*>(nullptr)); }

		template <class Y, std::enable_if_t<std::is_convertible_v<Y*, T*>, int> = 0>
		void reset(Y* a_ptr)
		{
			T* const old = _ptr;
			_ptr = a_ptr;
			Attach();
			if (old && old->DecRef() == 0)
				delete old;
		}

		[[nodiscard]] constexpr T* get() const noexcept { return _ptr; }
		[[nodiscard]] T& operator*() const noexcept { return *_ptr; }
		[[nodiscard]] T* operator->() const noexcept { return _ptr; }
		[[nodiscard]] explicit constexpr operator bool() const noexcept { return _ptr != nullptr; }

		template <class Y>
		[[nodiscard]] friend bool operator==(const BSTSmartPointer& a_lhs, const BSTSmartPointer<Y>& a_rhs) noexcept { return a_lhs.get() == a_rhs.get(); }
		[[nodiscard]] friend bool operator==(const BSTSmartPointer& a_lhs, std::nullptr_t) noexcept { return !a_lhs; }

	private:
		void Attach()
		{
			if (_ptr)
				_ptr->IncRef();
		}

		void Detach()
		{
			if (_ptr) {
				T* const old = std::exchange(_ptr, nullptr);
				if (old->DecRef() == 0)
					delete old;
			}
		}

		T* _ptr{ nullptr };
	};

	template <class T, class... Args>
	[[nodiscard]] BSTSmartPointer<T> make_smart(Args&&... a_args)
	{
		return BSTSmartPointer<T>(new T(std::forward<Args>(a_args)...));
	}

	// Havok's reference-counted pointer, as far as FSMP reads it.
	template <class T>
	class hkRefPtr
	{
	public:
		constexpr hkRefPtr() noexcept = default;
		explicit constexpr hkRefPtr(T* a_ptr) noexcept :
			_ptr(a_ptr) {}

		[[nodiscard]] constexpr T* get() const noexcept { return _ptr; }
		[[nodiscard]] T* operator->() const noexcept { return _ptr; }
		[[nodiscard]] explicit constexpr operator bool() const noexcept { return _ptr != nullptr; }

	private:
		T* _ptr{ nullptr };
	};

	// -----------------------------------------------------------------------
	// Math
	// -----------------------------------------------------------------------

	class NiPoint3
	{
	public:
		constexpr NiPoint3() noexcept = default;
		constexpr NiPoint3(float a_x, float a_y, float a_z) noexcept :
			x(a_x), y(a_y), z(a_z) {}

		[[nodiscard]] float& operator[](std::size_t a_idx) { return (&x)[a_idx]; }
		[[nodiscard]] const float& operator[](std::size_t a_idx) const { return (&x)[a_idx]; }

		[[nodiscard]] constexpr NiPoint3 operator+(const NiPoint3& a_rhs) const noexcept { return { x + a_rhs.x, y + a_rhs.y, z + a_rhs.z }; }
		[[nodiscard]] constexpr NiPoint3 operator-(const NiPoint3& a_rhs) const noexcept { return { x - a_rhs.x, y - a_rhs.y, z - a_rhs.z }; }
		[[nodiscard]] constexpr NiPoint3 operator*(float a_scalar) const noexcept { return { x * a_scalar, y * a_scalar, z * a_scalar }; }
		[[nodiscard]] constexpr NiPoint3 operator/(float a_scalar) const noexcept { return { x / a_scalar, y / a_scalar, z / a_scalar }; }
		[[nodiscard]] constexpr NiPoint3 operator-() const noexcept { return { -x, -y, -z }; }
		constexpr NiPoint3& operator+=(const NiPoint3& a_rhs) noexcept { return *this = *this + a_rhs; }
		constexpr NiPoint3& operator-=(const NiPoint3& a_rhs) noexcept { return *this = *this - a_rhs; }
		constexpr NiPoint3& operator*=(float a_scalar) noexcept { return *this = *this * a_scalar; }
		constexpr NiPoint3& operator/=(float a_scalar) noexcept { return *this = *this / a_scalar; }

		[[nodiscard]] constexpr float Dot(const NiPoint3& a_rhs) const noexcept { return x * a_rhs.x + y * a_rhs.y + z * a_rhs.z; }
		[[nodiscard]] constexpr NiPoint3 Cross(const NiPoint3& a_rhs) const noexcept
		{
			return { y * a_rhs.z - z * a_rhs.y, z * a_rhs.x - x * a_rhs.z, x * a_rhs.y - y * a_rhs.x };
		}
		[[nodiscard]] float SqrLength() const noexcept { return Dot(*this); }
		[[nodiscard]] float Length() const noexcept { return std::sqrt(SqrLength()); }
		float Unitize() noexcept
		{
			const float length = Length();
			if (length > 1e-06f)
				*this /= length;
			else
				*this = {};
			return length;
		}

		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
	};

	class NiMatrix3
	{
	public:
		NiMatrix3() noexcept { MakeIdentity(); }

		void MakeIdentity() noexcept
		{
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					entry[r][c] = r == c ? 1.0f : 0.0f;
		}

		[[nodiscard]] NiMatrix3 Transpose() const noexcept
		{
			NiMatrix3 ret;
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					ret.entry[r][c] = entry[c][r];
			return ret;
		}

		[[nodiscard]] NiMatrix3 operator*(const NiMatrix3& a_rhs) const noexcept
		{
			NiMatrix3 ret;
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					ret.entry[r][c] = entry[r][0] * a_rhs.entry[0][c] + entry[r][1] * a_rhs.entry[1][c] + entry[r][2] * a_rhs.entry[2][c];
			return ret;
		}

		[[nodiscard]] NiMatrix3 operator*(float a_scalar) const noexcept
		{
			NiMatrix3 ret;
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					ret.entry[r][c] = entry[r][c] * a_scalar;
			return ret;
		}

		[[nodiscard]] NiPoint3 operator*(const NiPoint3& a_rhs) const noexcept
		{
			return {
				entry[0][0] * a_rhs.x + entry[0][1] * a_rhs.y + entry[0][2] * a_rhs.z,
				entry[1][0] * a_rhs.x + entry[1][1] * a_rhs.y + entry[1][2] * a_rhs.z,
				entry[2][0] * a_rhs.x + entry[2][1] * a_rhs.y + entry[2][2] * a_rhs.z
			};
		}

		float entry[3][3];
	};

	class NiTransform
	{
	public:
		[[nodiscard]] NiTransform operator*(const NiTransform& a_rhs) const noexcept
		{
			NiTransform ret;
			ret.rotate = rotate * a_rhs.rotate;
			ret.translate = rotate * (a_rhs.translate * scale) + translate;
			ret.scale = scale * a_rhs.scale;
			return ret;
		}

		[[nodiscard]] NiPoint3 operator*(const NiPoint3& a_point) const noexcept { return rotate * (a_point * scale) + translate; }

		[[nodiscard]] NiTransform Invert() const noexcept
		{
			NiTransform ret;
			ret.rotate = rotate.Transpose();
			ret.scale = scale != 0.0f ? 1.0f / scale : 0.0f;
			ret.translate = (ret.rotate * -translate) * ret.scale;
			return ret;
		}

		NiMatrix3 rotate;
		NiPoint3 translate;
		float scale{ 1.0f };
	};

	struct NiBound
	{
		NiPoint3 center;
		float radius{ 0.0f };
	};

	struct hkQuad
	{
		float m128_f32[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
	};

	struct hkVector4
	{
		hkQuad quad;
	};

	struct hkQuaternion
	{
		hkVector4 vec;
	};

	struct hkQsTransform
	{
		hkVector4 translation;
		hkQuaternion rotation;
		hkVector4 scale;
	};

	// -----------------------------------------------------------------------
	// Scene graph
	// -----------------------------------------------------------------------

	struct NiUpdateData
	{
		enum class Flag : std::uint32_t
		{
			kNone = 0,
			kDirty = 1 << 0
		};

		float time{ 0.0f };
		Flag flags{ Flag::kNone };
	};

	class NiObject;

	struct NiCloningProcess
	{
		char appendChar{ '$' };
		std::uint32_t copyType{ 0 };
		NiPoint3 scale{ 1.0f, 1.0f, 1.0f };

		// Original object → its clone, filled by CreateClone and read by ProcessClone.
		std::unordered_map<NiObject*, NiObject*> cloneMap;
	};

	class NiRefObject
	{
	public:
		NiRefObject() = default;
		NiRefObject(const NiRefObject&) = delete;
		NiRefObject& operator=(const NiRefObject&) = delete;
		virtual ~NiRefObject() = default;

		virtual void DeleteThis() { delete this; }

		std::uint32_t IncRefCount() { return ++_refCount; }
		std::uint32_t DecRefCount()
		{
			const auto count = --_refCount;
			if (count == 0)
				DeleteThis();
			return count;
		}

		std::atomic<std::uint32_t> _refCount{ 0 };
	};

	class NiObject : public NiRefObject
	{
	public:
		virtual NiNode* AsNode() { return nullptr; }
		virtual BSFadeNode* AsFadeNode() { return nullptr; }
		virtual BSGeometry* AsGeometry() { return nullptr; }
		virtual BSTriShape* AsTriShape() { return nullptr; }
		virtual BSDynamicTriShape* AsDynamicTriShape() { return nullptr; }
		virtual NiGeometry* AsNiGeometry() { return nullptr; }

		virtual NiObject* CreateClone(NiCloningProcess& a_cloning);
		virtual void ProcessClone(NiCloningProcess& a_cloning);
	};

	class NiExtraData : public NiObject
	{
	public:
		BSFixedString name;
	};

	class NiStringExtraData : public NiExtraData
	{
	public:
		void SetValue(std::string a_value)
		{
			_storage = std::move(a_value);
			value = _storage.data();
		}

		char* value{ nullptr };

	private:
		std::string _storage;
	};

	class BSExtraData
	{
	public:
		virtual ~BSExtraData() = default;
	};

	class ExtraWorn : public BSExtraData
	{};

	class ExtraWornLeft : public BSExtraData
	{};

	template <class To, class From>
	[[nodiscard]] To netimmerse_cast(From* a_from)
	{
		return a_from ? dynamic_cast<To>(a_from) : nullptr;
	}

	class NiObjectNET : public NiObject
	{
	public:
		void AddExtraData(NiExtraData* a_extra);
		[[nodiscard]] NiExtraData* GetExtraData(const BSFixedString& a_name) const;

		template <class T>
		[[nodiscard]] T* GetExtraData(const BSFixedString& a_name) const
		{
			return netimmerse_cast<T*>(GetExtraData(a_name));
		}

		BSFixedString name;
		NiExtraData** extra{ nullptr };
		std::uint16_t extraDataSize{ 0 };

	protected:
		void CopyMembers(const NiObjectNET& a_source);

	private:
		std::vector<NiPointer<NiExtraData>> _extraStore;
		std::vector<NiExtraData*> _extraList;
	};

	class NiAVObject : public NiObjectNET
	{
	public:
		[[nodiscard]] TESObjectREFR* GetUserData() const { return userData; }

		virtual NiAVObject* GetObjectByName(const BSFixedString& a_name);
		virtual void UpdateWorldData(NiUpdateData* a_data);
		virtual void UpdateDownwardPass(NiUpdateData& a_data);

		void Update(NiUpdateData& a_data) { UpdateDownwardPass(a_data); }

		NiObject* CreateClone(NiCloningProcess& a_cloning) override;

		NiNode* parent{ nullptr };
		NiTransform local;
		NiTransform world;
		TESObjectREFR* userData{ nullptr };

	protected:
		void CopyMembers(const NiAVObject& a_source);
	};

	class NiNode : public NiAVObject
	{
	public:
		NiNode* AsNode() override { return this; }

		NiAVObject* GetObjectByName(const BSFixedString& a_name) override;
		void UpdateDownwardPass(NiUpdateData& a_data) override;
		NiObject* CreateClone(NiCloningProcess& a_cloning) override;

		[[nodiscard]] BSTArray<NiPointer<NiAVObject>>& GetChildren() { return children; }
		[[nodiscard]] const BSTArray<NiPointer<NiAVObject>>& GetChildren() const { return children; }

		void AttachChild(NiAVObject* a_child, bool a_firstAvail = false);
		void DetachChild(NiAVObject* a_child);
		void DetachChild(NiAVObject* a_child, NiPointer<NiAVObject>& a_childOut);
		void DetachChild2(NiAVObject* a_child) { DetachChild(a_child); }
		void DetachChildAt(std::uint32_t a_idx);
		void DetachChildAt2(std::uint32_t a_idx) { DetachChildAt(a_idx); }

		BSTArray<NiPointer<NiAVObject>> children;

	protected:
		void CopyChildren(const NiNode& a_source, NiCloningProcess& a_cloning);
	};

	class BSFadeNode : public NiNode
	{
	public:
		BSFadeNode* AsFadeNode() override { return this; }
		NiObject* CreateClone(NiCloningProcess& a_cloning) override;
	};

	class BSFaceGenNiNode : public NiNode
	{
	public:
		NiObject* CreateClone(NiCloningProcess& a_cloning) override;
	};

	class NiCamera : public NiAVObject
	{
	public:
		[[nodiscard]] bool NodeInFrustum(NiAVObject* a_node) const;
	};

	// -----------------------------------------------------------------------
	// Geometry and skinning
	// -----------------------------------------------------------------------

	namespace BSGraphics
	{
		struct Vertex
		{
			enum Flags : std::uint16_t
			{
				VF_NONE = 0,
				VF_VERTEX = 1 << 0,
				VF_UV = 1 << 1,
				VF_UV_2 = 1 << 2,
				VF_NORMAL = 1 << 3,
				VF_TANGENT = 1 << 4,
				VF_COLORS = 1 << 5,
				VF_SKINNED = 1 << 6,
				VF_LANDDATA = 1 << 7,
				VF_EYEDATA = 1 << 8,
				VF_FULLPREC = 1 << 10
			};
		};

		class VertexDesc
		{
		public:
			[[nodiscard]] Vertex::Flags GetFlags() const noexcept { return flags; }
			[[nodiscard]] std::uint32_t GetSize() const noexcept { return size; }

			Vertex::Flags flags{ Vertex::VF_NONE };
			std::uint32_t size{ 0 };
		};

		struct TriShape
		{
			std::uint8_t* rawVertexData{ nullptr };
			std::uint16_t* rawIndexData{ nullptr };
		};
	}

	class NiSkinData : public NiObject
	{
	public:
		struct BoneData
		{
			NiTransform skinToBone;
			NiBound bound;
		};

		NiTransform rootParentToSkin;
		BoneData* boneData{ nullptr };
		std::uint32_t bones{ 0 };

		std::vector<BoneData> boneStore;
	};

	class NiSkinPartition : public NiObject
	{
	public:
		struct Partition
		{
			BSGraphics::VertexDesc vertexDesc;
			std::uint16_t vertices{ 0 };
			std::uint16_t triangles{ 0 };
			std::uint16_t bones{ 0 };
			std::uint16_t bonesPerVertex{ 0 };
			std::uint16_t* triList{ nullptr };
			BSGraphics::TriShape* buffData{ nullptr };

			std::vector<std::uint16_t> triStore;
		};

		std::uint32_t numPartitions{ 0 };
		BSTArray<Partition> partitions;
		std::uint32_t vertexCount{ 0 };

		// The engine shares one vertex buffer among all partitions of a shape.
		std::vector<std::uint8_t> vertexStore;
		BSGraphics::TriShape bufferData;
	};

	class NiSkinInstance : public NiObject
	{
	public:
		void SetBoneCount(std::uint32_t a_count)
		{
			boneStore.assign(a_count, nullptr);
			boneWorldStore.assign(a_count, nullptr);
			bones = boneStore.data();
			boneWorldTransforms = boneWorldStore.data();
		}

		NiPointer<NiSkinData> skinData;
		NiPointer<NiSkinPartition> skinPartition;
		NiAVObject* rootParent{ nullptr };
		NiAVObject** bones{ nullptr };
		NiTransform** boneWorldTransforms{ nullptr };

		std::vector<NiAVObject*> boneStore;
		std::vector<NiTransform*> boneWorldStore;
	};

	class NiGeometry : public NiAVObject
	{
	public:
		struct NiGeometryRuntimeData
		{
			NiPointer<NiSkinInstance> spSkinInstance;
		};

		NiGeometry* AsNiGeometry() override { return this; }

		[[nodiscard]] NiGeometryRuntimeData& GetRuntimeData() noexcept { return runtimeData; }

		NiGeometryRuntimeData runtimeData;
	};

	class BSGeometry : public NiAVObject
	{
	public:
		struct GEOMETRY_RUNTIME_DATA
		{
			NiPointer<NiSkinInstance> skinInstance;
		};

		BSGeometry* AsGeometry() override { return this; }

		[[nodiscard]] GEOMETRY_RUNTIME_DATA& GetGeometryRuntimeData() noexcept { return geometryRuntimeData; }
		[[nodiscard]] const GEOMETRY_RUNTIME_DATA& GetGeometryRuntimeData() const noexcept { return geometryRuntimeData; }

		GEOMETRY_RUNTIME_DATA geometryRuntimeData;
	};

	class BSTriShape : public BSGeometry
	{
	public:
		BSTriShape* AsTriShape() override { return this; }
		NiObject* CreateClone(NiCloningProcess& a_cloning) override;

		std::uint16_t triangleCount{ 0 };
		std::uint16_t vertexCount{ 0 };

	protected:
		void CopyMembers(const BSTriShape& a_source);
	};

	class BSDynamicTriShape : public BSTriShape
	{
	public:
		struct DYNAMIC_TRISHAPE_RUNTIME_DATA
		{
			void* dynamicData{ nullptr };
		};

		BSDynamicTriShape* AsDynamicTriShape() override { return this; }
		NiObject* CreateClone(NiCloningProcess& a_cloning) override;

		[[nodiscard]] DYNAMIC_TRISHAPE_RUNTIME_DATA& GetDynamicTrishapeRuntimeData() noexcept { return dynamicRuntimeData; }

		DYNAMIC_TRISHAPE_RUNTIME_DATA dynamicRuntimeData;

		// Positions as the engine keeps them for a dynamic shape: x, y, z and one unused float.
		std::vector<float> dynamicStore;
	};

	// -----------------------------------------------------------------------
	// Face generation
	// -----------------------------------------------------------------------

	struct BSFaceGenModelMeshData
	{
		NiPointer<NiAVObject> faceNode;
	};

	class BSFaceGenModel : public BSIntrusiveRefCounted
	{
	public:
		BSFaceGenModelMeshData* modelMeshData{ nullptr };
	};

	class BSFaceGenModelExtraData : public NiExtraData
	{
	public:
		BSFaceGenModel* m_model{ nullptr };
		BSFixedString bones[8];
	};

	// -----------------------------------------------------------------------
	// Resources
	// -----------------------------------------------------------------------

	namespace BSResource
	{
		struct Stream
		{
			std::uint32_t totalSize{ 0 };
		};
	}

	// Reads a file relative to the game's Data folder, loose files first, then archives.
	class BSResourceNiBinaryStream
	{
	public:
		explicit BSResourceNiBinaryStream(const char* a_path);
		explicit BSResourceNiBinaryStream(const std::string& a_path) :
			BSResourceNiBinaryStream(a_path.c_str()) {}

		[[nodiscard]] bool good() const { return _good; }
		void read(void* a_buffer, std::uint32_t a_bytes);

		BSResource::Stream* stream{ nullptr };

		[[nodiscard]] const std::vector<char>& bytes() const { return _bytes; }
		[[nodiscard]] const std::string& path() const { return _path; }

	private:
		std::string _path;
		BSResource::Stream _stream;
		std::vector<char> _bytes;
		std::size_t _position{ 0 };
		bool _good{ false };
	};

	// Loads NIF files into the scene graph.
	class NiStream
	{
	public:
		bool Load1(BSResourceNiBinaryStream* a_stream);

		BSTArray<NiPointer<NiObject>> topObjects;
	};

	// -----------------------------------------------------------------------
	// Events
	// -----------------------------------------------------------------------

	enum class BSEventNotifyControl
	{
		kContinue = 0,
		kStop = 1
	};

	template <class Event>
	class BSTEventSource;

	template <class Event>
	class BSTEventSink
	{
	public:
		virtual ~BSTEventSink() = default;
		virtual BSEventNotifyControl ProcessEvent(const Event* a_event, BSTEventSource<Event>* a_eventSource) = 0;
	};

	template <class Event>
	class BSTEventSource
	{
	public:
		using Sink = BSTEventSink<Event>;

		void AddEventSink(Sink* a_sink)
		{
			if (a_sink && std::find(_sinks.begin(), _sinks.end(), a_sink) == _sinks.end())
				_sinks.push_back(a_sink);
		}

		void RemoveEventSink(Sink* a_sink)
		{
			_sinks.erase(std::remove(_sinks.begin(), _sinks.end(), a_sink), _sinks.end());
		}

		void SendEvent(const Event* a_event)
		{
			const auto sinks = _sinks;
			for (auto* sink : sinks) {
				if (sink->ProcessEvent(a_event, this) == BSEventNotifyControl::kStop)
					break;
			}
		}

	private:
		std::vector<Sink*> _sinks;
	};

	struct MenuOpenCloseEvent
	{
		BSFixedString menuName;
		bool opening{ false };
	};

	// -----------------------------------------------------------------------
	// Havok animation data
	// -----------------------------------------------------------------------

	class hkStringPtr
	{
	public:
		hkStringPtr() = default;
		explicit hkStringPtr(std::string a_string) :
			_string(std::move(a_string)) {}

		[[nodiscard]] const char* data() const noexcept { return _string.c_str(); }
		[[nodiscard]] const char* c_str() const noexcept { return _string.c_str(); }

	private:
		std::string _string;
	};

	template <class T>
	class hkArray
	{
	public:
		[[nodiscard]] std::int32_t size() const noexcept { return static_cast<std::int32_t>(_data.size()); }
		[[nodiscard]] T& operator[](std::int32_t a_idx) { return _data[a_idx]; }
		[[nodiscard]] const T& operator[](std::int32_t a_idx) const { return _data[a_idx]; }
		void push_back(T a_value) { _data.push_back(std::move(a_value)); }
		void clear() noexcept { _data.clear(); }

	private:
		std::vector<T> _data;
	};

	struct hkaBone
	{
		hkStringPtr name;
		bool lockTranslation{ false };
	};

	class hkaSkeleton
	{
	public:
		hkStringPtr name;
		hkArray<std::int16_t> parentIndices;
		hkArray<hkaBone> bones;
		hkArray<hkQsTransform> referencePose;
	};

	class hkbCharacterSetup
	{
	public:
		hkRefPtr<hkaSkeleton> animationSkeleton;
	};

	class hkbCharacter
	{
	public:
		hkRefPtr<hkbCharacterSetup> setup;
	};

	class BShkbAnimationGraph : public BSIntrusiveRefCounted
	{
	public:
		hkbCharacter characterInstance;
	};

	class BSAnimationGraphManager : public BSIntrusiveRefCounted
	{
	public:
		BSTArray<BSTSmartPointer<BShkbAnimationGraph>> graphs;
	};

	using BSAnimationGraphManagerPtr = BSTSmartPointer<BSAnimationGraphManager>;

	// -----------------------------------------------------------------------
	// Forms
	// -----------------------------------------------------------------------

	class TESForm
	{
	public:
		virtual ~TESForm() = default;

		[[nodiscard]] static TESForm* LookupByID(FormID a_formID);

		template <class T>
		[[nodiscard]] static T* LookupByID(FormID a_formID)
		{
			auto* form = LookupByID(a_formID);
			return form ? dynamic_cast<T*>(form) : nullptr;
		}

		template <class T>
		[[nodiscard]] T* As() noexcept
		{
			return dynamic_cast<T*>(this);
		}

		[[nodiscard]] FormID GetFormID() const noexcept { return formID; }

		FormID formID{ 0 };
	};

	// Registers or removes a form for TESForm::LookupByID.
	void RegisterForm(TESForm* a_form);
	void UnregisterForm(TESForm* a_form);

	template <class To, class From>
	[[nodiscard]] To skyrim_cast(From* a_from)
	{
		return a_from ? dynamic_cast<To>(a_from) : nullptr;
	}

	class TESFullName
	{
	public:
		virtual ~TESFullName() = default;

		[[nodiscard]] const char* GetFullName() const noexcept { return fullName.c_str(); }

		BSFixedString fullName;
	};

	class TESModel
	{
	public:
		[[nodiscard]] const char* GetModel() const noexcept { return model.c_str(); }

		BSFixedString model;
	};

	struct BIPED_MODEL
	{
		enum class BipedObjectSlot : std::uint32_t
		{
			kNone = 0,
			kHead = 1u << 0,
			kHair = 1u << 1,
			kBody = 1u << 2,
			kHands = 1u << 3,
			kForearms = 1u << 4,
			kAmulet = 1u << 5,
			kRing = 1u << 6,
			kFeet = 1u << 7,
			kCalves = 1u << 8,
			kShield = 1u << 9,
			kTail = 1u << 10,
			kLongHair = 1u << 11,
			kCirclet = 1u << 12,
			kEars = 1u << 13
		};

		SKSE::stl::enumeration<BipedObjectSlot, std::uint32_t> bipedObjectSlots;
	};

	class BGSBipedObjectForm
	{
	public:
		virtual ~BGSBipedObjectForm() = default;

		BIPED_MODEL bipedModelData;
	};

	class TESBoundObject : public TESForm
	{};

	class TESObjectARMO : public TESBoundObject, public TESFullName, public BGSBipedObjectForm
	{};

	class TESRace : public TESForm, public TESFullName
	{
	public:
		TESModel skeletonModels[2];
	};

	class TESNPC : public TESBoundObject, public TESFullName
	{
	public:
		TESRace* race{ nullptr };
		TESNPC* faceNPC{ nullptr };
		BGSHeadPart** headParts{ nullptr };
		std::int8_t numHeadParts{ 0 };
	};

	class TESWorldSpace : public TESForm
	{};

	class TESObjectCELL : public TESForm
	{
	public:
		struct RUNTIME_DATA
		{
			TESWorldSpace* worldSpace{ nullptr };
		};

		[[nodiscard]] RUNTIME_DATA& GetRuntimeData() noexcept { return runtimeData; }

		RUNTIME_DATA runtimeData;
	};

	class TESWeather : public TESForm
	{
	public:
		struct WeatherData
		{
			std::uint8_t windSpeed{ 0 };
			std::uint8_t windDirection{ 0 };
			std::uint8_t windDirectionRange{ 0 };
		};

		WeatherData data;
	};

	// -----------------------------------------------------------------------
	// Inventory
	// -----------------------------------------------------------------------

	namespace BSContainer
	{
		enum class ForEachResult
		{
			kContinue = 0,
			kStop = 1
		};
	}

	class ExtraDataList
	{
	public:
		template <class T>
		[[nodiscard]] T* GetByType() const
		{
			for (auto& data : _data) {
				if (auto* typed = dynamic_cast<T*>(data.get()))
					return typed;
			}
			return nullptr;
		}

		template <class T>
		[[nodiscard]] bool HasType() const
		{
			return GetByType<T>() != nullptr;
		}

		void Add(std::unique_ptr<BSExtraData> a_data) { _data.push_back(std::move(a_data)); }

	private:
		std::vector<std::unique_ptr<BSExtraData>> _data;
	};

	class InventoryEntryData
	{
	public:
		TESBoundObject* object{ nullptr };
		BSSimpleList<ExtraDataList*>* extraLists{ nullptr };
	};

	class InventoryChanges
	{
	public:
		class IItemChangeVisitor
		{
		public:
			virtual ~IItemChangeVisitor() = default;
			virtual BSContainer::ForEachResult Visit(InventoryEntryData* a_entryData) = 0;
		};

		BSSimpleList<InventoryEntryData*>* entryList{ nullptr };
	};

	class ExtraContainerChanges : public BSExtraData
	{
	public:
		InventoryChanges* changes{ nullptr };
	};

	// -----------------------------------------------------------------------
	// References and actors
	// -----------------------------------------------------------------------

	struct OBJ_REFR
	{
		TESBoundObject* objectReference{ nullptr };
		NiPoint3 angle;
		NiPoint3 location;
	};

	struct LOADED_REF_DATA;

	class TESObjectREFR : public TESForm
	{
	public:
		std::uint32_t IncRefCount() { return ++_refCount; }
		std::uint32_t DecRefCount() { return --_refCount; }

		[[nodiscard]] TESBoundObject* GetBaseObject() const noexcept { return data.objectReference; }
		[[nodiscard]] TESBoundObject* GetObjectReference() const noexcept { return data.objectReference; }

		[[nodiscard]] NiAVObject* Get3D() const noexcept { return Get3D2(); }
		[[nodiscard]] NiAVObject* Get3D(bool a_firstPerson) const noexcept { return a_firstPerson ? Get3D1(true) : Get3D2(); }
		[[nodiscard]] NiAVObject* Get3D1(bool a_firstPerson) const noexcept { return a_firstPerson ? nullptr : Get3D2(); }
		[[nodiscard]] NiAVObject* Get3D2() const noexcept { return thirdPerson3D; }
		[[nodiscard]] NiAVObject* GetNodeByName(const BSFixedString& a_name) const;

		bool GetAnimationGraphManager(BSAnimationGraphManagerPtr& a_out) const
		{
			a_out = animationGraphManager;
			return static_cast<bool>(a_out);
		}

		OBJ_REFR data;
		ExtraDataList extraList;
		TESObjectCELL* parentCell{ nullptr };
		LOADED_REF_DATA* loadedData{ nullptr };

		NiAVObject* thirdPerson3D{ nullptr };
		BSAnimationGraphManagerPtr animationGraphManager;

		std::atomic<std::uint32_t> _refCount{ 0 };
	};

	class Actor : public TESObjectREFR
	{};

	class PlayerCharacter : public Actor
	{
	public:
		[[nodiscard]] static PlayerCharacter* GetSingleton();
	};

	// -----------------------------------------------------------------------
	// Camera, UI and timing
	// -----------------------------------------------------------------------

	enum class CameraState : std::uint32_t
	{
		kFirstPerson = 0,
		kAutoVanity,
		kVATS,
		kFree,
		kIronSights,
		kFurniture,
		kPCTransition,
		kTween,
		kAnimated,
		kThirdPerson,
		kMount,
		kBleedout,
		kDragon,

		kTotal
	};

	class TESCameraState : public BSIntrusiveRefCounted
	{
	public:
		virtual ~TESCameraState() = default;

		CameraState id{ CameraState::kThirdPerson };
	};

	class PlayerCamera
	{
	public:
		struct RUNTIME_DATA
		{
			BSTSmartPointer<TESCameraState> cameraStates[static_cast<std::size_t>(CameraState::kTotal)];
		};

		struct RUNTIME_DATA2
		{
			bool isWeapSheathed{ true };
		};

		[[nodiscard]] static PlayerCamera* GetSingleton();

		[[nodiscard]] RUNTIME_DATA& GetRuntimeData() noexcept { return runtimeData; }
		[[nodiscard]] RUNTIME_DATA2& GetRuntimeData2() noexcept { return runtimeData2; }

		NiPointer<NiNode> cameraRoot;
		BSTSmartPointer<TESCameraState> currentState;
		RUNTIME_DATA runtimeData;
		RUNTIME_DATA2 runtimeData2;
	};

	class Main
	{
	public:
		[[nodiscard]] static Main* GetSingleton();
		[[nodiscard]] static NiCamera* WorldRootCamera();

		bool quitGame{ false };
		bool freezeTime{ false };
	};

	class UI : public BSTEventSource<MenuOpenCloseEvent>
	{
	public:
		[[nodiscard]] static UI* GetSingleton();

		[[nodiscard]] bool GameIsPaused() const noexcept { return paused; }

		template <class Event>
		void AddEventSink(BSTEventSink<Event>* a_sink)
		{
			static_cast<BSTEventSource<Event>*>(this)->AddEventSink(a_sink);
		}

		template <class Event>
		void RemoveEventSink(BSTEventSink<Event>* a_sink)
		{
			static_cast<BSTEventSource<Event>*>(this)->RemoveEventSink(a_sink);
		}

		bool paused{ false };
	};

	class BSTimer
	{
	public:
		[[nodiscard]] static BSTimer* GetSingleton();

		float delta{ 0.0f };
		float realTimeDelta{ 0.0f };
	};

	[[nodiscard]] float GetSecondsSinceLastFrame();

	class Sky
	{
	public:
		// The previewer has no sky.
		[[nodiscard]] static Sky* GetSingleton() { return nullptr; }

		TESWeather* currentWeather{ nullptr };
		float windSpeed{ 0.0f };
		float windAngle{ 0.0f };
	};

	class ConsoleLog
	{
	public:
		[[nodiscard]] static ConsoleLog* GetSingleton();

		void Print(const char* a_fmt, ...);
	};

	inline constexpr std::array<REL::VariantID, 1> VTABLE_BSFaceGenNiNode{ REL::VariantID(0, 0, 0) };
}

namespace std
{
	template <>
	struct formatter<RE::BSFixedString, char> : formatter<std::string_view, char>
	{
		template <class FormatContext>
		auto format(const RE::BSFixedString& a_string, FormatContext& a_ctx) const
		{
			return formatter<std::string_view, char>::format(std::string_view(a_string), a_ctx);
		}
	};
}

namespace fmt
{
	using std::format;
}

using RE::netimmerse_cast;
using RE::skyrim_cast;
