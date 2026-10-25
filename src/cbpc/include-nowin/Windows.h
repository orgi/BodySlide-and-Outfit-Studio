// Stand-in for Windows.h on other platforms: the Win32 types, functions and
// macros CBPC's files use.
#pragma once

#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef std::uint32_t DWORD;
typedef std::int32_t LONG;
typedef std::uint32_t ULONG;
typedef std::int64_t LONGLONG;
typedef unsigned int UINT;
typedef std::uint8_t UINT8;
typedef std::uint16_t UINT16;
typedef std::uint32_t UINT32;
typedef std::uint64_t UINT64;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define MAX_PATH 260

union LARGE_INTEGER {
	struct {
		DWORD LowPart;
		LONG HighPart;
	};
	LONGLONG QuadPart;
};

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) {
	frequency->QuadPart = std::chrono::steady_clock::period::den / std::chrono::steady_clock::period::num;
	return TRUE;
}

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* count) {
	count->QuadPart = std::chrono::steady_clock::now().time_since_epoch().count();
	return TRUE;
}

inline long InterlockedIncrement(volatile long* value) {
	return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST);
}

inline long InterlockedDecrement(volatile long* value) {
	return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST);
}

inline int _strnicmp(const char* a, const char* b, std::size_t count) {
	for (std::size_t i = 0; i < count; i++) {
		const int ca = std::tolower(static_cast<unsigned char>(a[i]));
		const int cb = std::tolower(static_cast<unsigned char>(b[i]));
		if (ca != cb)
			return ca - cb;
		if (ca == 0)
			break;
	}
	return 0;
}

inline int _stricmp(const char* a, const char* b) {
	return _strnicmp(a, b, static_cast<std::size_t>(-1));
}

inline int vsprintf_s(char* buffer, std::size_t size, const char* format, va_list args) {
	return std::vsnprintf(buffer, size, format, args);
}

#ifndef NOMINMAX
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#endif
