/*
 * CommunityShaders — see CommunityShaders.h.
 */

#include "CommunityShaders.h"
#include "../utils/PlatformUtil.h"
#include "LeveledListData.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <utility>

namespace communityshaders {

namespace {

constexpr float kMaxHorizonFadeHours = 1.5f; // SkySync.h

// ---------------------------------------------------------------------------
// The JSON of Community Shaders' settings files
// ---------------------------------------------------------------------------

struct Json {
	enum class Type { Null, Bool, Number, String, Array, Object };
	Type type = Type::Null;
	bool boolean = false;
	double number = 0.0;
	std::string string;
	std::vector<Json> array;
	std::vector<std::pair<std::string, Json>> object;

	const Json* Find(const std::string& key) const {
		for (auto& [name, value] : object) {
			if (name == key)
				return &value;
		}
		return nullptr;
	}
};

class JsonParser {
public:
	explicit JsonParser(const std::string& text)
		: text(text) {}

	bool Parse(Json& out, std::string& error) {
		if (!Value(out) || (SkipSpace(), pos != text.size())) {
			error = "invalid JSON at offset " + std::to_string(pos);
			return false;
		}
		return true;
	}

private:
	const std::string& text;
	size_t pos = 0;

	void SkipSpace() {
		while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
			pos++;
	}

	bool Literal(const char* literal) {
		const size_t length = std::char_traits<char>::length(literal);
		if (text.compare(pos, length, literal) != 0)
			return false;
		pos += length;
		return true;
	}

	bool String(std::string& out) {
		if (pos >= text.size() || text[pos] != '"')
			return false;
		pos++;
		while (pos < text.size() && text[pos] != '"') {
			char c = text[pos++];
			if (c != '\\') {
				out += c;
				continue;
			}
			if (pos >= text.size())
				return false;
			c = text[pos++];
			switch (c) {
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u': {
					if (pos + 4 > text.size())
						return false;
					const unsigned long code = std::strtoul(text.substr(pos, 4).c_str(), nullptr, 16);
					pos += 4;
					if (code < 0x80)
						out += static_cast<char>(code);
					else if (code < 0x800) {
						out += static_cast<char>(0xC0 | (code >> 6));
						out += static_cast<char>(0x80 | (code & 0x3F));
					}
					else {
						out += static_cast<char>(0xE0 | (code >> 12));
						out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
						out += static_cast<char>(0x80 | (code & 0x3F));
					}
					break;
				}
				default: out += c; break;
			}
		}
		if (pos >= text.size())
			return false;
		pos++;
		return true;
	}

	bool Value(Json& out) {
		SkipSpace();
		if (pos >= text.size())
			return false;

		const char c = text[pos];
		if (c == '{') {
			out.type = Json::Type::Object;
			pos++;
			SkipSpace();
			if (pos < text.size() && text[pos] == '}') {
				pos++;
				return true;
			}
			while (true) {
				SkipSpace();
				std::string key;
				if (!String(key))
					return false;
				SkipSpace();
				if (pos >= text.size() || text[pos] != ':')
					return false;
				pos++;
				Json value;
				if (!Value(value))
					return false;
				out.object.emplace_back(std::move(key), std::move(value));
				SkipSpace();
				if (pos < text.size() && text[pos] == ',') {
					pos++;
					continue;
				}
				if (pos < text.size() && text[pos] == '}') {
					pos++;
					return true;
				}
				return false;
			}
		}
		if (c == '[') {
			out.type = Json::Type::Array;
			pos++;
			SkipSpace();
			if (pos < text.size() && text[pos] == ']') {
				pos++;
				return true;
			}
			while (true) {
				Json value;
				if (!Value(value))
					return false;
				out.array.push_back(std::move(value));
				SkipSpace();
				if (pos < text.size() && text[pos] == ',') {
					pos++;
					continue;
				}
				if (pos < text.size() && text[pos] == ']') {
					pos++;
					return true;
				}
				return false;
			}
		}
		if (c == '"') {
			out.type = Json::Type::String;
			return String(out.string);
		}
		if (Literal("true")) {
			out.type = Json::Type::Bool;
			out.boolean = true;
			return true;
		}
		if (Literal("false")) {
			out.type = Json::Type::Bool;
			return true;
		}
		if (Literal("null"))
			return true;

		const char* start = text.c_str() + pos;
		char* end = nullptr;
		out.number = std::strtod(start, &end);
		if (end == start)
			return false;
		out.type = Json::Type::Number;
		pos += end - start;
		return true;
	}
};

bool ReadJsonFile(const std::string& path, Json& json, std::string& error) {
	std::fstream file;
	PlatformUtil::OpenFileStream(file, path, std::ios_base::in | std::ios_base::binary);
	if (!file) {
		error = "can't open " + path;
		return false;
	}
	std::stringstream buffer;
	buffer << file.rdbuf();
	const std::string text = buffer.str();
	if (!JsonParser(text).Parse(json, error)) {
		error = path + ": " + error;
		return false;
	}
	return true;
}

// The section of a feature in the merged settings; false if Community
// Shaders doesn't load the feature.
bool LoadFeature(const std::string& dataFolder, const char* name, const char* section, Json& out, std::vector<std::string>& errors) {
	if (lldata::LeveledListData::ResolveCaseInsensitive(dataFolder, std::string("Shaders/Features/") + name + ".ini").empty())
		return false;

	const std::string folder = "SKSE/Plugins/CommunityShaders/";
	const std::string defaultPath = lldata::LeveledListData::ResolveCaseInsensitive(dataFolder, folder + "SettingsDefault.json");
	const std::string userPath = lldata::LeveledListData::ResolveCaseInsensitive(dataFolder, folder + "SettingsUser.json");

	// Default settings, then the sections of the user settings over them.
	Json merged;
	merged.type = Json::Type::Object;
	for (const std::string& path : {defaultPath, userPath}) {
		if (path.empty())
			continue;
		Json json;
		std::string error;
		if (!ReadJsonFile(path, json, error)) {
			errors.push_back("Community Shaders' settings: " + error);
			continue;
		}
		for (auto& [key, value] : json.object) {
			auto it = std::find_if(merged.object.begin(), merged.object.end(), [&key](auto& entry) { return entry.first == key; });
			if (it != merged.object.end())
				it->second = value;
			else
				merged.object.emplace_back(key, value);
		}
	}

	if (const Json* disabled = merged.Find("Disable at Boot")) {
		if (const Json* feature = disabled->Find(name); feature && feature->type == Json::Type::Bool && feature->boolean)
			return false;
	}

	if (const Json* json = merged.Find(section))
		out = *json;
	return true;
}

void ReadBool(const Json& json, const char* key, bool& value) {
	if (const Json* v = json.Find(key); v && v->type == Json::Type::Bool)
		value = v->boolean;
}

template<typename T>
void ReadNumber(const Json& json, const char* key, T& value) {
	if (const Json* v = json.Find(key); v && v->type == Json::Type::Number)
		value = static_cast<T>(v->number);
}

} // namespace

SkySyncSettings LoadSkySyncSettings(const std::string& dataFolder, std::vector<std::string>& errors) {
	SkySyncSettings settings;
	Json json;
	if (!LoadFeature(dataFolder, "SkySync", "Sky Sync", json, errors))
		return settings;
	settings.loaded = true;

	ReadBool(json, "Enabled", settings.enabled);
	ReadBool(json, "UseAlternateSunPath", settings.useAlternateSunPath);
	ReadNumber(json, "SunPath", settings.sunPath);
	ReadNumber(json, "CustomAngle", settings.customAngle);
	ReadNumber(json, "MinShadowElevation", settings.minShadowElevation);
	ReadBool(json, "DimSunlightUnderHorizon", settings.dimSunlightUnderHorizon);
	ReadNumber(json, "HorizonFadeHours", settings.horizonFadeHours);

	// SkySync::LoadSettings
	settings.sunPath = std::clamp(settings.sunPath, 0, 3);
	settings.customAngle = std::clamp(settings.customAngle, -90.0f, 90.0f);
	settings.minShadowElevation = std::clamp(settings.minShadowElevation, 0.0f, 45.0f);
	settings.horizonFadeHours = std::clamp(settings.horizonFadeHours, 0.0f, kMaxHorizonFadeHours);
	return settings;
}

ScreenSpaceShadowsSettings LoadScreenSpaceShadowsSettings(const std::string& dataFolder, std::vector<std::string>& errors) {
	ScreenSpaceShadowsSettings settings;
	Json json;
	if (!LoadFeature(dataFolder, "ScreenSpaceShadows", "Screen Space Shadows", json, errors))
		return settings;
	settings.loaded = true;

	ReadNumber(json, "Enable", settings.enable);
	ReadNumber(json, "SampleCount", settings.sampleCount);
	ReadNumber(json, "SurfaceThickness", settings.surfaceThickness);
	ReadNumber(json, "BilinearThreshold", settings.bilinearThreshold);
	ReadNumber(json, "ShadowContrast", settings.shadowContrast);
	return settings;
}

} // namespace communityshaders
