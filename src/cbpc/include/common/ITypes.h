// Stand-in for SKSE's common/ITypes.h. SKSE's UInt32 and SInt32 are long,
// which has 64 bits outside of Windows; these have the sizes of the Windows build.
#pragma once

#include <cstdint>

typedef std::uint8_t UInt8;
typedef std::uint16_t UInt16;
typedef std::uint32_t UInt32;
typedef std::uint64_t UInt64;
typedef std::int8_t SInt8;
typedef std::int16_t SInt16;
typedef std::int32_t SInt32;
typedef std::int64_t SInt64;
typedef float Float32;
typedef double Float64;
