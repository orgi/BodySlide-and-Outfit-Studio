// Stand-in for the Win32 implementation of dirent.h that CBPC ships with: lists
// a folder of the game's paths the way Windows does, without regard to case
// and with the entries in the order of NTFS ("." and ".." first, then by name
// in upper case).
#pragma once

#include <string>
#include <vector>

struct dirent {
	char d_name[512];
};

struct DIR {
	std::vector<std::string> names;
	size_t next = 0;
	dirent entry{};
};

DIR* opendir(const char* name);
dirent* readdir(DIR* dir);
int closedir(DIR* dir);
