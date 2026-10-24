// Stand-in for CommonLibSSE's SKSE logging and stl helpers. Log lines are
// formatted like spdlog does and handed to the sink the host installs.
#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace SKSE
{
	namespace log
	{
		using Sink = void (*)(spdlog::level::level_enum a_level, std::string_view a_message);
		void SetSink(Sink a_sink);
		void Write(spdlog::level::level_enum a_level, std::string_view a_message);

		template <class... Args>
		void Log(spdlog::level::level_enum a_level, std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			if (spdlog::should_log(a_level))
				Write(a_level, std::format(a_fmt, std::forward<Args>(a_args)...));
		}

		template <class... Args>
		void trace(std::format_string<Args...> a_fmt, Args&&... a_args) { Log(spdlog::level::trace, a_fmt, std::forward<Args>(a_args)...); }
		template <class... Args>
		void debug(std::format_string<Args...> a_fmt, Args&&... a_args) { Log(spdlog::level::debug, a_fmt, std::forward<Args>(a_args)...); }
		template <class... Args>
		void info(std::format_string<Args...> a_fmt, Args&&... a_args) { Log(spdlog::level::info, a_fmt, std::forward<Args>(a_args)...); }
		template <class... Args>
		void warn(std::format_string<Args...> a_fmt, Args&&... a_args) { Log(spdlog::level::warn, a_fmt, std::forward<Args>(a_args)...); }
		template <class... Args>
		void error(std::format_string<Args...> a_fmt, Args&&... a_args) { Log(spdlog::level::err, a_fmt, std::forward<Args>(a_args)...); }
		template <class... Args>
		void critical(std::format_string<Args...> a_fmt, Args&&... a_args) { Log(spdlog::level::critical, a_fmt, std::forward<Args>(a_args)...); }
	}

	namespace stl
	{
		[[noreturn]] void report_and_fail(std::string_view a_message);

		template <class E, class U = std::underlying_type_t<E>>
		class enumeration
		{
		public:
			using enum_type = E;
			using underlying_type = U;

			constexpr enumeration() noexcept = default;
			constexpr enumeration(enum_type a_value) noexcept :
				_impl(static_cast<underlying_type>(a_value)) {}

			template <class... Args>
			constexpr enumeration(Args... a_values) noexcept
				requires(std::same_as<Args, enum_type> && ...)
				:
				_impl((static_cast<underlying_type>(a_values) | ...))
			{}

			constexpr enumeration& operator=(enum_type a_value) noexcept
			{
				_impl = static_cast<underlying_type>(a_value);
				return *this;
			}

			[[nodiscard]] constexpr enum_type get() const noexcept { return static_cast<enum_type>(_impl); }
			[[nodiscard]] constexpr underlying_type underlying() const noexcept { return _impl; }
			[[nodiscard]] constexpr enum_type operator*() const noexcept { return get(); }

			template <class... Args>
			constexpr enumeration& set(Args... a_args) noexcept
				requires(std::same_as<Args, enum_type> && ...)
			{
				_impl |= (static_cast<underlying_type>(a_args) | ...);
				return *this;
			}

			template <class... Args>
			constexpr enumeration& reset(Args... a_args) noexcept
				requires(std::same_as<Args, enum_type> && ...)
			{
				_impl &= ~(static_cast<underlying_type>(a_args) | ...);
				return *this;
			}

			template <class... Args>
			[[nodiscard]] constexpr bool any(Args... a_args) const noexcept
				requires(std::same_as<Args, enum_type> && ...)
			{
				return (_impl & (static_cast<underlying_type>(a_args) | ...)) != static_cast<underlying_type>(0);
			}

			template <class... Args>
			[[nodiscard]] constexpr bool all(Args... a_args) const noexcept
				requires(std::same_as<Args, enum_type> && ...)
			{
				return (_impl & (static_cast<underlying_type>(a_args) | ...)) == (static_cast<underlying_type>(a_args) | ...);
			}

		private:
			underlying_type _impl{ 0 };
		};
	}
}
