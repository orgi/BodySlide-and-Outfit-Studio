// Connection between the engine stand-ins (include/) and the standalone layer
// that sets them up (FsmpScene).
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace fsmp::standin
{
	// Reads a game data file by its path relative to the Data folder ("meshes/..."),
	// loose files first, then archives.
	using FileReader = std::function<bool(const std::string& a_path, std::vector<char>& a_data)>;

	void SetFileReader(FileReader a_reader);
	bool ReadDataFile(const std::string& a_path, std::vector<char>& a_data);
}
