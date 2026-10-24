// Stand-in for CommonLibSSE's REL/Relocation.h, used to build FSMP outside of
// the game. There is no game executable to relocate into: an ID resolves to
// the engine function the standalone layer registered for it (see
// REL::RegisterFunction), or to nothing.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace REL
{
	// Returns the registered engine function for the SE address library ID, or 0.
	std::uintptr_t LookupFunction(std::uint64_t a_seID);
	void RegisterFunction(std::uint64_t a_seID, std::uintptr_t a_function);
	void ReportMissingFunction(std::uint64_t a_seID);

	class Version
	{
	public:
		constexpr Version() noexcept = default;
		constexpr Version(std::uint16_t a_major, std::uint16_t a_minor = 0, std::uint16_t a_patch = 0, std::uint16_t a_build = 0) noexcept :
			_impl{ a_major, a_minor, a_patch, a_build }
		{}

		[[nodiscard]] constexpr std::uint16_t major() const noexcept { return _impl[0]; }
		[[nodiscard]] constexpr std::uint16_t minor() const noexcept { return _impl[1]; }
		[[nodiscard]] constexpr std::uint16_t patch() const noexcept { return _impl[2]; }
		[[nodiscard]] constexpr std::uint16_t build() const noexcept { return _impl[3]; }

		[[nodiscard]] constexpr std::uint32_t pack() const noexcept
		{
			return static_cast<std::uint32_t>((_impl[0] & 0x0FF) << 24u | (_impl[1] & 0x0FF) << 16u | (_impl[2] & 0xFFF) << 4u | (_impl[3] & 0x00F) << 0u);
		}

		[[nodiscard]] std::string string() const
		{
			return std::to_string(_impl[0]) + "." + std::to_string(_impl[1]) + "." + std::to_string(_impl[2]) + "." + std::to_string(_impl[3]);
		}

	private:
		std::array<std::uint16_t, 4> _impl{ 0, 0, 0, 0 };
	};

	class Module
	{
	public:
		[[nodiscard]] static constexpr bool IsSE() noexcept { return true; }
		[[nodiscard]] static constexpr bool IsAE() noexcept { return false; }
		[[nodiscard]] static constexpr bool IsVR() noexcept { return false; }
	};

	class Offset
	{
	public:
		constexpr Offset() noexcept = default;
		explicit constexpr Offset(std::size_t a_offset) noexcept :
			_offset(a_offset) {}

		[[nodiscard]] constexpr std::size_t offset() const noexcept { return _offset; }
		[[nodiscard]] std::uintptr_t address() const { return 0; }

	private:
		std::size_t _offset{ 0 };
	};

	class ID
	{
	public:
		constexpr ID() noexcept = default;
		explicit constexpr ID(std::uint64_t a_id) noexcept :
			_id(a_id) {}

		[[nodiscard]] constexpr std::uint64_t id() const noexcept { return _id; }
		[[nodiscard]] std::uintptr_t address() const { return LookupFunction(_id); }

	private:
		std::uint64_t _id{ 0 };
	};

	class VariantID
	{
	public:
		constexpr VariantID() noexcept = default;
		constexpr VariantID(std::uint64_t a_seID, std::uint64_t a_aeID, std::uint64_t a_vrOffset) noexcept :
			_seID(a_seID), _aeID(a_aeID), _vrOffset(a_vrOffset)
		{}

		[[nodiscard]] constexpr std::uint64_t id() const noexcept { return _seID; }
		[[nodiscard]] std::uintptr_t address() const { return LookupFunction(_seID); }

	private:
		std::uint64_t _seID{ 0 };
		std::uint64_t _aeID{ 0 };
		std::uint64_t _vrOffset{ 0 };
	};

	class VariantOffset
	{
	public:
		constexpr VariantOffset() noexcept = default;
		constexpr VariantOffset(std::size_t a_se, std::size_t a_ae, std::size_t a_vr) noexcept :
			_se(a_se), _ae(a_ae), _vr(a_vr)
		{}

		[[nodiscard]] constexpr std::size_t offset() const noexcept { return _se; }

	private:
		std::size_t _se{ 0 };
		std::size_t _ae{ 0 };
		std::size_t _vr{ 0 };
	};

	using RelocationID = VariantID;

	template <class T>
	class Relocation
	{
	public:
		using value_type = std::conditional_t<std::is_member_pointer_v<T> || std::is_function_v<std::remove_pointer_t<T>>, std::decay_t<T>, T>;

		constexpr Relocation() noexcept = default;
		explicit constexpr Relocation(std::uintptr_t a_address) noexcept :
			_address(a_address) {}
		explicit Relocation(Offset a_offset) :
			_address(a_offset.address()) {}
		explicit Relocation(ID a_id) :
			_id(a_id.id()) {}
		explicit Relocation(VariantID a_id) :
			_id(a_id.id()) {}
		Relocation(VariantID a_id, VariantOffset a_offset) :
			_id(a_id.id()), _offset(a_offset.offset()) {}
		Relocation(ID a_id, Offset a_offset) :
			_id(a_id.id()), _offset(a_offset.offset()) {}

		constexpr Relocation& operator=(std::uintptr_t a_address) noexcept
		{
			_address = a_address;
			_id = 0;
			return *this;
		}

		[[nodiscard]] std::uintptr_t address() const
		{
			if (_id != 0) {
				const auto base = LookupFunction(_id);
				return base ? base + _offset : 0;
			}
			return _address;
		}

		[[nodiscard]] value_type get() const
		{
			if constexpr (std::is_pointer_v<value_type>)
				return reinterpret_cast<value_type>(address());
			else
				return value_type(address());
		}

		template <class... Args>
		decltype(auto) operator()(Args&&... a_args) const
			requires(std::is_invocable_v<const value_type&, Args...>)
		{
			using result_t = std::invoke_result_t<const value_type&, Args...>;
			const auto function = address();
			if (!function) {
				ReportMissingFunction(_id);
				if constexpr (std::is_void_v<result_t>)
					return;
				else
					return result_t{};
			}
			return std::invoke(reinterpret_cast<value_type>(function), std::forward<Args>(a_args)...);
		}

		template <class F>
		std::uintptr_t write_vfunc(std::size_t, F)
		{
			ReportMissingFunction(_id);
			return 0;
		}

	private:
		std::uintptr_t _address{ 0 };
		std::uint64_t _id{ 0 };
		std::size_t _offset{ 0 };
	};

	template <class T>
	void safe_write(std::uintptr_t a_dst, const T&)
	{
		ReportMissingFunction(a_dst);
	}

	template <class T>
	[[nodiscard]] T& RelocateMember(void* a_self, std::ptrdiff_t a_seAndAE, std::ptrdiff_t)
	{
		return *reinterpret_cast<T*>(static_cast<std::byte*>(a_self) + a_seAndAE);
	}
}

#define RELOCATION_ID(a_se, a_ae) REL::VariantID(a_se, a_ae, 0)
