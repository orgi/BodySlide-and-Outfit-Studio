#include "StringStuff.h"
#include <cctype>
#include <filesystem>
#include <sstream>

bool StringsEqualNInsens(const char* a, const char* b, int len) {
	while (len > 0) {
		if (std::tolower(*a) != std::tolower(*b))
			return false;
		if (*a == '\0')
			return true;
		++a, ++b, --len;
	}
	return true;
}

bool StringsEqualInsens(const char* a, const char* b) {
	while (true) {
		if (std::tolower(*a) != std::tolower(*b))
			return false;
		if (*a == '\0')
			return true;
		++a, ++b;
	}
}

bool StringStartsWith(std::string_view s, std::string_view prefix) {
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool StringEndsWith(std::string_view s, std::string_view suffix) {
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ToOSSlashes(const std::string& s) {
	std::string d(s);
	size_t len = d.length();
	for (size_t i = 0; i < len; ++i)
#ifdef _WINDOWS
		if (d[i] == '/')
			d[i] = '\\';
#else
		if (d[i] == '\\')
			d[i] = '/';
#endif
	return d;
}

std::string ToBackslashes(const std::string& s) {
	std::string d(s);
	size_t len = d.length();
	for (size_t i = 0; i < len; ++i)
		if (d[i] == '/')
			d[i] = '\\';
	return d;
}

std::vector<std::string> SplitString(const std::string& s, const char delim) {
	std::stringstream sstrm(s);
	std::string segment;
	std::vector<std::string> seglist;

	while (std::getline(sstrm, segment, delim)) {
		seglist.push_back(segment);
	}

	return seglist;
}

std::string JoinStrings(const std::vector<std::string>& elements, const char* const separator) {
	switch (elements.size()) {
		case 0: return "";
		case 1: return elements[0];
		default:
			std::ostringstream os;
			std::copy(elements.begin(), elements.end() - 1, std::ostream_iterator<std::string>(os, separator));
			os << *elements.rbegin();
			return os.str();
	}
}

namespace {
bool IEqualsAscii(const std::string& a, const std::string& b) {
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	}
	return true;
}
} // namespace

std::string ResolveCaseInsensitivePath(const std::string& inPath) {
	namespace fs = std::filesystem;

	if (inPath.empty())
		return inPath;

	std::error_code ec;
	if (fs::exists(fs::u8path(inPath), ec))
		return inPath;

	// Normalize: convert backslashes to forward slashes for splitting.
	std::string normalized = inPath;
	for (char& c : normalized) {
		if (c == '\\')
			c = '/';
	}

	// Detect and preserve a root prefix so the walk starts from a real directory.
	std::string prefix;
	size_t startPos = 0;
	if (normalized.size() >= 1 && normalized[0] == '/') {
		prefix = "/";
		startPos = 1;
	}
	else if (normalized.size() >= 3 && std::isalpha(static_cast<unsigned char>(normalized[0])) && normalized[1] == ':' && normalized[2] == '/') {
		prefix = normalized.substr(0, 3);
		startPos = 3;
	}

	// Split remaining components on '/'.
	std::vector<std::string> components;
	std::string cur;
	for (size_t i = startPos; i < normalized.size(); ++i) {
		if (normalized[i] == '/') {
			if (!cur.empty())
				components.push_back(cur);
			cur.clear();
		}
		else {
			cur += normalized[i];
		}
	}
	if (!cur.empty())
		components.push_back(cur);

	if (components.empty())
		return {};

	fs::path resolved = fs::u8path(prefix.empty() ? std::string(".") : prefix);

	for (const std::string& comp : components) {
		// Try exact match first (fast path).
		fs::path candidate = resolved / fs::u8path(comp);
		if (fs::exists(candidate, ec)) {
			resolved = candidate;
			continue;
		}

		// Case-insensitive scan of the current directory.
		bool found = false;
		if (fs::is_directory(resolved, ec)) {
			for (fs::directory_iterator it(resolved, ec), end; !ec && it != end; it.increment(ec)) {
				const std::string name = it->path().filename().u8string();
				if (IEqualsAscii(name, comp)) {
					resolved = it->path();
					found = true;
					break;
				}
			}
		}
		if (!found)
			return {};
	}

	return resolved.u8string();
}
