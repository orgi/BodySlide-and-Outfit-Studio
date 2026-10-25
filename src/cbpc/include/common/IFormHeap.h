// The engine's form heap (SKSE's GameAPI.h). SKSE's NiTypes.h uses it without
// including GameAPI.h, which compilers other than Visual C++ don't accept.
#pragma once

#include <cstddef>

void* FormHeap_Allocate(std::size_t size);
void FormHeap_Free(void* ptr);
