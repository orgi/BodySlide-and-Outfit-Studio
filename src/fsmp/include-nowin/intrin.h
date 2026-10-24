// Stand-in for <intrin.h> on platforms other than Windows.
#pragma once

#include <immintrin.h>

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
// MSVC's CPU feature intrinsics, which Bullet's CPU feature detection calls.
inline void __cpuid(int a_info[4], int a_leaf)
{
	__asm__ __volatile__("cpuid"
						 : "=a"(a_info[0]), "=b"(a_info[1]), "=c"(a_info[2]), "=d"(a_info[3])
						 : "a"(a_leaf), "c"(0));
}

inline unsigned long long FsmpXgetbv(unsigned int a_register)
{
	unsigned int eax = 0;
	unsigned int edx = 0;
	__asm__ __volatile__("xgetbv"
						 : "=a"(eax), "=d"(edx)
						 : "c"(a_register));
	return (static_cast<unsigned long long>(edx) << 32) | eax;
}

// GCC's own _xgetbv only compiles with -mxsave.
#	define _xgetbv(a_register) FsmpXgetbv(a_register)
#endif
