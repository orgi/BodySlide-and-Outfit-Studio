// Stand-in for the parts of spdlog FSMP touches. FSMP's log lines go to the
// host's log through SKSE::log (see SKSE/Logger.h); spdlog only keeps the level.
#pragma once

namespace spdlog
{
	namespace level
	{
		enum level_enum : int
		{
			trace = 0,
			debug = 1,
			info = 2,
			warn = 3,
			err = 4,
			critical = 5,
			off = 6,
			n_levels
		};
	}

	void set_level(level::level_enum a_level);
	void flush_on(level::level_enum a_level);
	[[nodiscard]] level::level_enum get_level();
	[[nodiscard]] bool should_log(level::level_enum a_level);
}
