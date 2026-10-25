// Stand-in for SKSE's common/IErrors.h. A failed assertion is logged as an
// error instead of stopping the program.
#pragma once

namespace cbpc {
void AssertionFailed(const char* file, unsigned long line, const char* desc);
}

#define ASSERT(a)			do { if (!(a)) cbpc::AssertionFailed(__FILE__, __LINE__, #a); } while (0)
#define ASSERT_STR(a, b)	do { if (!(a)) cbpc::AssertionFailed(__FILE__, __LINE__, b); } while (0)
#define HALT(a)				cbpc::AssertionFailed(__FILE__, __LINE__, a)
#define STATIC_ASSERT(a)	static_assert(a, #a)
