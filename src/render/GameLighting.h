/*
 * GameLighting — lights the leveled list previewer's meshes like the game
 * outdoors: the sun with its shadow and the directional ambient light, for
 * the previewer's shader (res/shaders/game_lighting.*). The sun and the
 * ambient light come from src/components/GameSky.*.
 *
 * Before the surface draws its meshes, the sun renders the depth of the
 * shadow-casting meshes into its shadow map (res/shaders/shadowmap.*),
 * fitted to their bounds, like the engine's focus shadow map of an actor.
 * Then the camera renders the depth of the opaque meshes, over which
 * Community Shaders' screen-space shadows march towards the sun
 * (ContactShadows). While the surface draws a mesh, its shader gets the sun,
 * the ambient light, the shadow map if the mesh receives shadows and the
 * screen-space shadows if it is opaque.
 *
 * Which meshes cast and receive shadows comes from their NIF shapes: a
 * BSLightingShaderProperty's Cast_Shadows and Receive_Shadows flags. Shapes
 * with another shader neither cast nor receive.
 */

#pragma once

#include "../components/CommunityShaders.h"
#include "ContactShadows.h"
#include "GLSurface.h"

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class GameLighting {
public:
	struct ShapeFlags {
		bool cast = false;
		bool receive = false;
	};

	// The shadow flags of a NIF shape.
	static ShapeFlags ReadShapeFlags(nifly::NifFile& nif, nifly::NiShape* shape);

	// Creates the shadow map and hooks the lighting into the frames of
	// `surface`. The surface's context must be current. Returns false (logged)
	// if the shadow map can't be created.
	bool Initialize(GLSurface* surface, const std::string& shaderDir);
	// Unhooks the lighting from the surface and deletes the shadow map. The
	// surface's context must be current.
	void Release();

	// The sun: the direction towards it and its color, and the directional
	// ambient light (for each color channel the factors of normal.x, normal.y,
	// normal.z and 1), all in the meshes' model space.
	void SetSun(const nifly::Vector3& direction, const nifly::Vector3& color, const std::array<std::array<float, 4>, 3>& ambient);

	// Where a mesh with a model space normal map gets the rotation of each
	// vertex' skinning while it is posed (x and y axis, six floats per
	// vertex, mesh space); nullptr: not posed.
	void SetSkinRotationSource(std::function<const std::vector<float>*(Mesh*)> source) { skinRotationSource = std::move(source); }

	void SetShadowsEnabled(bool enable);
	bool ShadowsEnabled() const { return shadowsEnabled; }

	// Community Shaders' screen-space shadows, as its settings have them.
	// They run when the settings and SetContactShadowsEnabled() allow it and
	// OpenGL 4.3 is there (ContactShadowsAvailable()).
	void SetContactShadowSettings(const communityshaders::ScreenSpaceShadowsSettings& settings) { contactSettings = settings; }
	void SetContactShadowsEnabled(bool enable) { contactShadowsEnabled = enable; }
	bool ContactShadowsAvailable() const { return contactShadowsAvailable; }

	// The flags of the surface's mesh named `meshName`. Meshes without flags
	// neither cast nor receive shadows.
	void SetShapeFlags(const std::string& meshName, const ShapeFlags& flags);

private:
	static constexpr int MapSize = 2048;
	static constexpr int ShadowTextureUnit = 13; // the materials use units 0 to 7 and 20
	static constexpr int ContactShadowTextureUnit = 14;
	static constexpr GLuint SkinRotationXAttribute = 9; // GLSurface uses the attributes 0 to 8
	static constexpr GLuint SkinRotationYAttribute = 10;

	GLSurface* gls = nullptr;
	GLShader depthShader;
	GLuint framebuffer = 0;
	GLuint shadowTexture = 0;
	GLuint skinRotationBuffer = 0; // the skin rotation of the mesh being drawn
	std::function<const std::vector<float>*(Mesh*)> skinRotationSource;
	glm::mat4x4 shadowMatrix = glm::identity<glm::mat4x4>(); // model space to shadow map clip space
	bool shadowsEnabled = false;
	bool shadowRendered = false;
	std::unordered_map<std::string, ShapeFlags> shapeFlags;

	ContactShadows contactShadows;
	communityshaders::ScreenSpaceShadowsSettings contactSettings;
	bool contactShadowsAvailable = false;
	bool contactShadowsEnabled = false;
	bool contactShadowsRendered = false;
	GLuint cameraFramebuffer = 0;
	GLuint cameraDepthTexture = 0;
	int cameraWidth = 0;
	int cameraHeight = 0;

	nifly::Vector3 sunDirection = nifly::Vector3(0.0f, 1.0f, 0.0f);
	nifly::Vector3 sunColor;
	glm::mat4x4 ambientMatrix = glm::mat4x4(0.0f); // column c: color channel c

	// Bounds the shadow map covers: the casters' bounds of every frame since
	// the casters changed.
	std::vector<std::string> boundsCasters;
	bool hasBounds = false;
	nifly::Vector3 boundsMin;
	nifly::Vector3 boundsMax;

	void RenderShadowMap();
	void RenderContactShadows(const glm::mat4x4& projection, const glm::mat4x4& view);
	// Draws the depth of a mesh with the depth shader in use.
	void DrawDepth(Mesh* m, const glm::mat4x4& view, GLint vertexAlphaLoc);
	void SetupMeshShader(Mesh* m, GLShader& shader);
};
