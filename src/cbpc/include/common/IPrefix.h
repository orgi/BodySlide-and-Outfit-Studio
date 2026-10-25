// Stand-in for SKSE's common/IPrefix.h, the header CBPC's project includes
// first in every file. The standard headers CBPC's files use come before
// Windows.h, whose min and max macros CBPC's files use as well.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
// The C headers declare the floating point overloads of abs() globally, like on Windows.
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <amp.h>
#include <concurrent_unordered_map.h>
#include <concurrent_vector.h>
#include <ppl.h>

#include "common/ITypes.h"
#include "common/IErrors.h"
#include "common/IFormHeap.h"

#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline
#endif

#ifndef _WIN32
namespace cbpc {
// clock() of the Windows C runtime: milliseconds of wall time since the program started.
std::clock_t Clock();
}
// CBPC measures gaps between frames with clock() in milliseconds; outside of
// Windows, clock() counts processor time in other units.
#define clock() cbpc::Clock()
#endif

#include <winsock2.h>
#include <Windows.h>
