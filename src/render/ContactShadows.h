/*
 * ContactShadows — Community Shaders' Screen Space Shadows for the leveled
 * list previewer's sun: Bend Studio's screen-space ray march
 * (res/shaders/screenspaceshadows.comp, dispatched as
 * lib/bend_sss/bend_sss_cpu.h lists it) over the depth the camera sees.
 * Needs OpenGL 4.3 (compute shaders).
 */

#pragma once

#include "../components/CommunityShaders.h"
#include "GLSurface.h"

#include <string>

class ContactShadows {
public:
	// Checks for OpenGL 4.3 and reads the shader. The context must be
	// current. Returns false (logged) if either is missing.
	bool Initialize(const std::string& shaderDir);
	// Deletes the shader and the shadow texture. The context must be current.
	void Release();

	// The ray march towards the sun over `depthTexture`, the depth the camera
	// sees (width x height). `lightProjection` is the direction towards the
	// sun, (direction, 0) times the camera's projection and view. Returns
	// false (logged) if the shader can't be built.
	bool Render(GLuint depthTexture, int width, int height, const glm::vec4& lightProjection, const communityshaders::ScreenSpaceShadowsSettings& settings);

	// The shadow of each pixel of the last Render (1: lit).
	GLuint Texture() const { return outputTexture; }

private:
	static constexpr GLint DepthTextureUnit = 15;

	std::string source;
	GLuint program = 0;
	unsigned programSampleCount = 0;
	bool buildFailed = false;
	GLuint outputTexture = 0;
	GLuint clearFramebuffer = 0;
	int outputWidth = 0;
	int outputHeight = 0;

	bool BuildProgram(unsigned sampleCount);
};
