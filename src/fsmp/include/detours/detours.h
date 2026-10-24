// Stand-in for Microsoft Detours. FSMP's Hooks.h names it; the previewer hooks
// nothing, so attaching a detour is an error (see StandIns.cpp).
#pragma once

#ifdef _WIN32
#	include <windows.h>
#else
using PVOID = void*;
#endif

long DetourAttach(PVOID* a_pointer, PVOID a_detour);
