// Stand-in for <d3d11.h> on platforms other than Windows. FSMP's precompiled
// header gets the Win32 declarations its sources use through this header.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef MAX_PATH
#	define MAX_PATH 260
#endif

using BYTE = unsigned char;
using UINT = unsigned int;

#define ZeroMemory(a_destination, a_length) std::memset((a_destination), 0, (a_length))

#ifndef _CRT_ALIGN
#	define _CRT_ALIGN(a_alignment) alignas(a_alignment)
#endif

#ifndef __forceinline
#	define __forceinline inline __attribute__((always_inline))
#endif

union LARGE_INTEGER
{
	struct
	{
		std::uint32_t LowPart;
		std::int32_t HighPart;
	} u;
	std::int64_t QuadPart;
};

inline int QueryPerformanceCounter(LARGE_INTEGER* a_count)
{
	a_count->QuadPart = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	return 1;
}

inline int QueryPerformanceFrequency(LARGE_INTEGER* a_frequency)
{
	a_frequency->QuadPart = 1000000000;
	return 1;
}

template <std::size_t N>
int strcpy_s(char (&a_dst)[N], const char* a_src)
{
	if (!a_src || std::strlen(a_src) >= N) {
		if (N > 0)
			a_dst[0] = '\0';
		return 34;  // ERANGE
	}
	std::strcpy(a_dst, a_src);
	return 0;
}

template <std::size_t N, class... Args>
int sprintf_s(char (&a_dst)[N], const char* a_format, Args... a_args)
{
	return std::snprintf(a_dst, N, a_format, a_args...);
}
