/*
 * CommunityShaders — the settings of Community Shaders' features the
 * leveled list previewer simulates (Sky Sync, Screen Space Shadows), read
 * like Community Shaders reads them from Data/SKSE/Plugins/CommunityShaders:
 * SettingsDefault.json, then the sections of SettingsUser.json over it. A
 * feature is loaded when its Shaders/Features/<feature>.ini exists and
 * "Disable at Boot" doesn't disable it.
 */

#pragma once

#include <string>
#include <vector>

namespace communityshaders {

// Sky Sync's settings as its feature has them after Community Shaders loaded
// its settings (SkySync.h defaults for missing values, then its clamps).
struct SkySyncSettings {
	bool loaded = false; // Community Shaders loads the feature
	bool enabled = true;
	bool useAlternateSunPath = false;
	int sunPath = 0; // Southern, Northern, Vanilla, Custom
	float customAngle = -35.0f;
	float minShadowElevation = 10.0f;
	bool dimSunlightUnderHorizon = true;
	float horizonFadeHours = 0.7f;
};

// Screen Space Shadows' settings (ScreenSpaceShadows.h BendSettings defaults
// for missing values).
struct ScreenSpaceShadowsSettings {
	bool loaded = false; // Community Shaders loads the feature
	unsigned enable = 1;
	unsigned sampleCount = 1;
	float surfaceThickness = 0.02f;
	float bilinearThreshold = 0.02f;
	float shadowContrast = 1.0f;
};

// Errors (unreadable settings files) go to `errors`.
SkySyncSettings LoadSkySyncSettings(const std::string& dataFolder, std::vector<std::string>& errors);
ScreenSpaceShadowsSettings LoadScreenSpaceShadowsSettings(const std::string& dataFolder, std::vector<std::string>& errors);

} // namespace communityshaders
