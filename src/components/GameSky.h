/*
 * GameSky — the sun and the directional ambient light outdoors at a time of
 * day, as the game computes them with Community Shaders' Sky Sync, for the
 * leveled list previewer (src/render/GameLighting.*).
 *
 * The colors are the weather's (WTHR NAM0 Sunlight, DALC) for its four times
 * of day, blended over the climate's (CLMT TNAM) sunrise and sunset. The sun's
 * direction is Sky Sync's alternate sun path with the user's Sky Sync
 * settings, including its dimming of the sunlight near the horizon and its
 * minimum shadow elevation (Community Shaders' SkySync.cpp).
 *
 * Two parts are the engine's own and not public, see
 * docs/leveled-list-viewer.md → Lighting and shadows: how it blends the
 * weather's times of day (blended linearly here) and its sun path without
 * Sky Sync's alternate path (the alternate path with Sky Sync's Vanilla
 * angle here).
 */

#pragma once

#include "CommunityShaders.h"

#include <Object3d.hpp>

#include <array>
#include <string>
#include <vector>

namespace gamesky {

struct Color {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};

// The weather's times of day, in the order of its colors.
enum TimeOfDay { Sunrise, Day, Sunset, Night };

struct Weather {
	std::array<Color, 4> sunlight;				 // NAM0 Sunlight per time of day
	std::array<float, 4> sunlightScale{};		 // HNAM Sunlight Scale of the image space (IMSP) per time of day
	std::array<std::array<Color, 6>, 4> ambient; // DALC X+, X-, Y+, Y-, Z+, Z- per time of day
};

// Reads a WTHR record's NAM0 and DALC subrecords and the HNAM subrecords of
// its image spaces. Returns false if they are missing or too short.
bool ReadWeather(const std::vector<uint8_t>& colors,
				 const std::vector<std::vector<uint8_t>>& directionalAmbient,
				 const std::array<const std::vector<uint8_t>*, 4>& imageSpaceHdr,
				 Weather& weather);

// CLMT TNAM, in hours.
struct ClimateTiming {
	float sunriseBegin = 0.0f;
	float sunriseEnd = 0.0f;
	float sunsetBegin = 0.0f;
	float sunsetEnd = 0.0f;
};

// Reads a CLMT record's TNAM subrecord. Returns false if it is too short.
bool ReadClimateTiming(const std::vector<uint8_t>& timing, ClimateTiming& climateTiming);

using communityshaders::SkySyncSettings;

// Whether the sun follows Sky Sync's alternate path in game.
bool UsesAlternateSunPath(const SkySyncSettings& settings);

// The hours between which the sun's path is above the horizon.
void SunHours(const ClimateTiming& timing, float& sunrise, float& sunset);

struct Sky {
	nifly::Vector3 toSun; // towards the sun, as the directional light shines; world space (x east, y north, z up)
	Color sunlight;		  // the directional light's color, the image space's Sunlight Scale applied
	std::array<Color, 6> ambient;
};

Sky Evaluate(const Weather& weather, const ClimateTiming& timing, const SkySyncSettings& settings, float hour);

// A direction of the world in the model space of the previewer's actor
// (GLSurface's mesh space), the actor facing `headingDegrees` (0 north, 90 east).
nifly::Vector3 WorldToModel(const nifly::Vector3& world, float headingDegrees);

// The directional ambient light as the shader's matrix: for each color
// channel the factors of (normal.x, normal.y, normal.z, 1), model space.
// Each of the six colors is the light travelling along its axis: a surface
// facing up gets Z-, the color of the sky.
std::array<std::array<float, 4>, 3> AmbientFactors(const std::array<Color, 6>& ambient, float headingDegrees);

} // namespace gamesky
