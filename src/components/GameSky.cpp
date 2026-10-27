/*
 * GameSky — see GameSky.h.
 */

#include "GameSky.h"
#include "../utils/PlatformUtil.h"
#include "LeveledListData.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <utility>

namespace gamesky {

namespace {

constexpr float kPi = 3.14159265358979f;

// SkySync.h
constexpr float kSouthernSunAngle = 90.0f - 35.0f;
constexpr float kNorthernSunAngle = 90.0f + 35.0f;
constexpr float kVanillaSunAngle = 90.0f + 5.0f;
constexpr float kSunsetHeadingLockThreshold = 0.5f;
constexpr float kMaxHorizonFadeHours = 1.5f;

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

// ---------------------------------------------------------------------------
// The weather
// ---------------------------------------------------------------------------

Color ReadColor(const std::vector<uint8_t>& data, size_t offset) {
	return {data[offset] / 255.0f, data[offset + 1] / 255.0f, data[offset + 2] / 255.0f};
}

Color Lerp(const Color& a, const Color& b, float t) {
	return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// The times of day the weather's colors are blended from at `hour`, and the
// share of the second one.
void TimesOfDay(const ClimateTiming& timing, float hour, TimeOfDay& from, TimeOfDay& to, float& t) {
	const float sunriseMiddle = (timing.sunriseBegin + timing.sunriseEnd) * 0.5f;
	const float sunsetMiddle = (timing.sunsetBegin + timing.sunsetEnd) * 0.5f;
	auto share = [](float h, float begin, float end) { return end > begin ? std::clamp((h - begin) / (end - begin), 0.0f, 1.0f) : 1.0f; };

	t = 0.0f;
	if (hour < timing.sunriseBegin || hour >= timing.sunsetEnd)
		from = to = Night;
	else if (hour < sunriseMiddle) {
		from = Night;
		to = Sunrise;
		t = share(hour, timing.sunriseBegin, sunriseMiddle);
	}
	else if (hour < timing.sunriseEnd) {
		from = Sunrise;
		to = Day;
		t = share(hour, sunriseMiddle, timing.sunriseEnd);
	}
	else if (hour < timing.sunsetBegin)
		from = to = Day;
	else if (hour < sunsetMiddle) {
		from = Day;
		to = Sunset;
		t = share(hour, timing.sunsetBegin, sunsetMiddle);
	}
	else {
		from = Sunset;
		to = Night;
		t = share(hour, sunsetMiddle, timing.sunsetEnd);
	}
}

// ---------------------------------------------------------------------------
// Sky Sync (SkySync.cpp)
// ---------------------------------------------------------------------------

float SunAngle(const SkySyncSettings& settings) {
	switch (settings.sunPath) {
		case 0: return kSouthernSunAngle;
		case 1: return kNorthernSunAngle;
		case 2: return kVanillaSunAngle;
		default: return 90.0f + settings.customAngle;
	}
}

// CalculateAlternateSunDirectionAndDistance
nifly::Vector3 AlternateSunDirection(float time, float sunrise, float sunset, float sunAngle) {
	const float phi = kPi * ((time - sunrise) / (sunset - sunrise));
	const float tilt = sunAngle * kPi / 180.0f;
	nifly::Vector3 dir(std::cos(phi), -std::sin(phi) * std::cos(tilt), std::sin(phi) * std::sin(tilt));
	if (dir.length() < 1e-6f)
		return nifly::Vector3(0.0f, 0.0f, 1.0f);
	dir.Normalize();
	return dir;
}

// ShadowFader::SetDirection
nifly::Vector3 Direction(float heading, float elevation) {
	return nifly::Vector3(std::cos(elevation) * std::cos(heading), std::cos(elevation) * std::sin(heading), std::sin(elevation));
}

// SkySync::Update's dimming of the directional light.
float Dim(const ClimateTiming& timing, float hour, float fadeHours) {
	const float sunriseBegin = timing.sunriseBegin;
	const float sunriseMiddle = (timing.sunriseBegin + timing.sunriseEnd) * 0.5f;
	const float sunsetMiddle = (timing.sunsetBegin + timing.sunsetEnd) * 0.5f;
	const float sunsetEnd = timing.sunsetEnd;
	auto hoursBetween = [](float from, float to) {
		float d = to - from;
		return d < 0.0f ? d + 24.0f : d;
	};

	if (hour >= sunsetMiddle && hour < sunsetEnd) {
		float range = sunsetEnd - sunsetMiddle;
		float t = range > 0.0f ? (hour - sunsetMiddle) / range : 1.0f;
		return std::sqrt(1.0f - t);
	}
	if (fadeHours > 0.0f && hoursBetween(sunsetEnd, hour) < fadeHours)
		return hoursBetween(sunsetEnd, hour) / fadeHours;
	if (fadeHours > 0.0f && hoursBetween(hour, sunriseBegin) > 0.0f && hoursBetween(hour, sunriseBegin) <= fadeHours)
		return hoursBetween(hour, sunriseBegin) / fadeHours;
	if (hour >= sunriseBegin && hour < sunriseMiddle) {
		float range = sunriseMiddle - sunriseBegin;
		float t = range > 0.0f ? (hour - sunriseBegin) / range : 1.0f;
		return std::sqrt(t);
	}
	return 1.0f;
}

} // namespace

bool ReadWeather(const std::vector<uint8_t>& colors,
				 const std::vector<std::vector<uint8_t>>& directionalAmbient,
				 const std::array<const std::vector<uint8_t>*, 4>& imageSpaceHdr,
				 Weather& weather) {
	// NAM0 holds the color types (Sunlight is the fifth) for the four times of day.
	constexpr size_t kSunlight = 4;
	// HNAM: eye adapt speed, bloom blur radius, bloom threshold, bloom scale,
	// receive bloom threshold, white, sunlight scale, ...
	constexpr size_t kSunlightScale = 6;
	if (colors.size() < (kSunlight + 1) * 4 * 4 || directionalAmbient.size() < 4)
		return false;

	for (int time = 0; time < 4; time++) {
		weather.sunlight[time] = ReadColor(colors, (kSunlight * 4 + time) * 4);
		const std::vector<uint8_t>* hdr = imageSpaceHdr[time];
		if (!hdr || hdr->size() < (kSunlightScale + 1) * 4)
			return false;
		std::memcpy(&weather.sunlightScale[time], hdr->data() + kSunlightScale * 4, sizeof(float));
		if (directionalAmbient[time].size() < 6 * 4)
			return false;
		for (int axis = 0; axis < 6; axis++)
			weather.ambient[time][axis] = ReadColor(directionalAmbient[time], axis * 4);
	}
	return true;
}

bool ReadClimateTiming(const std::vector<uint8_t>& timing, ClimateTiming& climateTiming) {
	if (timing.size() < 4)
		return false;

	climateTiming.sunriseBegin = timing[0] / 6.0f;
	climateTiming.sunriseEnd = timing[1] / 6.0f;
	climateTiming.sunsetBegin = timing[2] / 6.0f;
	climateTiming.sunsetEnd = timing[3] / 6.0f;
	return true;
}

SkySyncSettings LoadSkySyncSettings(const std::string& dataFolder, std::vector<std::string>& errors) {
	SkySyncSettings settings;
	const std::string feature = lldata::LeveledListData::ResolveCaseInsensitive(dataFolder, "Shaders/Features/SkySync.ini");
	if (feature.empty())
		return settings;

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
		if (const Json* skySync = disabled->Find("SkySync"); skySync && skySync->type == Json::Type::Bool && skySync->boolean)
			return settings;
	}
	settings.loaded = true;

	const Json* json = merged.Find("Sky Sync");
	if (!json)
		return settings;
	auto boolean = [json](const char* key, bool& value) {
		if (const Json* v = json->Find(key); v && v->type == Json::Type::Bool)
			value = v->boolean;
	};
	auto number = [json](const char* key, auto& value) {
		if (const Json* v = json->Find(key); v && v->type == Json::Type::Number)
			value = static_cast<std::remove_reference_t<decltype(value)>>(v->number);
	};
	boolean("Enabled", settings.enabled);
	boolean("UseAlternateSunPath", settings.useAlternateSunPath);
	number("SunPath", settings.sunPath);
	number("CustomAngle", settings.customAngle);
	number("MinShadowElevation", settings.minShadowElevation);
	boolean("DimSunlightUnderHorizon", settings.dimSunlightUnderHorizon);
	number("HorizonFadeHours", settings.horizonFadeHours);

	// SkySync::LoadSettings
	settings.sunPath = std::clamp(settings.sunPath, 0, 3);
	settings.customAngle = std::clamp(settings.customAngle, -90.0f, 90.0f);
	settings.minShadowElevation = std::clamp(settings.minShadowElevation, 0.0f, 45.0f);
	settings.horizonFadeHours = std::clamp(settings.horizonFadeHours, 0.0f, kMaxHorizonFadeHours);
	return settings;
}

bool UsesAlternateSunPath(const SkySyncSettings& settings) {
	return settings.loaded && settings.enabled && settings.useAlternateSunPath;
}

void SunHours(const ClimateTiming& timing, float& sunrise, float& sunset) {
	// SkySync::ProcessSun
	sunrise = (timing.sunriseBegin + timing.sunriseEnd) * 0.5f - 0.25f;
	sunset = (timing.sunsetBegin + timing.sunsetEnd) * 0.5f + 0.25f;
}

Sky Evaluate(const Weather& weather, const ClimateTiming& timing, const SkySyncSettings& settings, float hour) {
	Sky sky;

	TimeOfDay from = Day;
	TimeOfDay to = Day;
	float t = 0.0f;
	TimesOfDay(timing, hour, from, to, t);
	sky.sunlight = Lerp(weather.sunlight[from], weather.sunlight[to], t);
	const float sunlightScale = weather.sunlightScale[from] + (weather.sunlightScale[to] - weather.sunlightScale[from]) * t;
	sky.sunlight.r *= sunlightScale;
	sky.sunlight.g *= sunlightScale;
	sky.sunlight.b *= sunlightScale;
	for (int axis = 0; axis < 6; axis++)
		sky.ambient[axis] = Lerp(weather.ambient[from][axis], weather.ambient[to][axis], t);

	float sunrise = 0.0f;
	float sunset = 0.0f;
	SunHours(timing, sunrise, sunset);
	const float sunAngle = UsesAlternateSunPath(settings) ? SunAngle(settings) : kVanillaSunAngle;
	sky.toSun = AlternateSunDirection(hour, sunrise, sunset, sunAngle);

	if (!settings.loaded || !settings.enabled)
		return sky;

	// SkySync::Update and OnSkyUpdateColors
	const float dim = Dim(timing, hour, settings.horizonFadeHours);
	if (settings.dimSunlightUnderHorizon && dim > 0.0f && dim < 1.0f) {
		sky.sunlight.r *= dim;
		sky.sunlight.g *= dim;
		sky.sunlight.b *= dim;
	}

	// ShadowFader::LockSunElevation, then SetLighting's ClampDirection
	const float minElevation = settings.minShadowElevation * kPi / 180.0f;
	const float sunriseMiddle = (timing.sunriseBegin + timing.sunriseEnd) * 0.5f;
	const float sunsetMiddle = (timing.sunsetBegin + timing.sunsetEnd) * 0.5f;
	const bool sunSetting = hour >= sunsetMiddle && hour < timing.sunsetEnd;
	const bool sunRising = hour >= timing.sunriseBegin && hour < sunriseMiddle;
	if (sunSetting) {
		if (dim <= kSunsetHeadingLockThreshold) {
			// The heading freezes when the dimming first passes the threshold.
			const float lockHour = sunsetMiddle + (1.0f - kSunsetHeadingLockThreshold * kSunsetHeadingLockThreshold) * (timing.sunsetEnd - sunsetMiddle);
			const nifly::Vector3 lockDir = AlternateSunDirection(lockHour, sunrise, sunset, sunAngle);
			sky.toSun = Direction(std::atan2(lockDir.y, lockDir.x), minElevation);
		}
		else
			sky.toSun = Direction(std::atan2(sky.toSun.y, sky.toSun.x), minElevation);
	}
	else if (sunRising && std::asin(std::clamp(sky.toSun.z, -1.0f, 1.0f)) < minElevation)
		sky.toSun = Direction(std::atan2(sky.toSun.y, sky.toSun.x), minElevation);

	if (std::asin(std::clamp(sky.toSun.z, -1.0f, 1.0f)) < minElevation)
		sky.toSun = Direction(std::atan2(sky.toSun.y, sky.toSun.x), minElevation);
	return sky;
}

nifly::Vector3 WorldToModel(const nifly::Vector3& world, float headingDegrees) {
	// The actor's right and forward (NIF x and y) in the world, then
	// Mesh::TransformDirNifToMesh.
	const float heading = headingDegrees * kPi / 180.0f;
	const float right = world.x * std::cos(heading) - world.y * std::sin(heading);
	const float forward = world.x * std::sin(heading) + world.y * std::cos(heading);
	return nifly::Vector3(-right, world.z, forward);
}

std::array<std::array<float, 4>, 3> AmbientFactors(const std::array<Color, 6>& ambient, float headingDegrees) {
	std::array<std::array<float, 4>, 3> factors{};
	for (int channel = 0; channel < 3; channel++) {
		auto value = [&ambient, channel](int axis) {
			const Color& c = ambient[axis];
			return channel == 0 ? c.r : (channel == 1 ? c.g : c.b);
		};
		// A surface facing along an axis gets the light travelling against it.
		// Each axis' two colors are matched by a slope along the axis, the six
		// colors' mean is the constant.
		const nifly::Vector3 slope((value(1) - value(0)) * 0.5f, (value(3) - value(2)) * 0.5f, (value(5) - value(4)) * 0.5f);
		const nifly::Vector3 model = WorldToModel(slope, headingDegrees);
		factors[channel] = {model.x, model.y, model.z, (value(0) + value(1) + value(2) + value(3) + value(4) + value(5)) / 6.0f};
	}
	return factors;
}

} // namespace gamesky
