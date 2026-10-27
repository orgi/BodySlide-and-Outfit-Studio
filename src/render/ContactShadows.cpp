/*
 * ContactShadows — see ContactShadows.h.
 */

#include "ContactShadows.h"
#include "../../lib/bend_sss/bend_sss_cpu.h"
#include "../utils/PlatformUtil.h"

#include <wx/log.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace {

// The OpenGL functions of the compute shader that GLExtensions doesn't load.
struct ComputeFunctions {
	PFNGLDISPATCHCOMPUTEPROC dispatchCompute = nullptr;
	PFNGLBINDIMAGETEXTUREPROC bindImageTexture = nullptr;
	PFNGLMEMORYBARRIERPROC memoryBarrier = nullptr;
	PFNGLUNIFORM4FPROC uniform4f = nullptr;
	PFNGLUNIFORM2IPROC uniform2i = nullptr;
	PFNGLDELETESHADERPROC deleteShader = nullptr;
	PFNGLDELETEPROGRAMPROC deleteProgram = nullptr;
} gl;

bool LoadComputeFunctions() {
#if defined(_WIN32)
	gl.dispatchCompute = (PFNGLDISPATCHCOMPUTEPROC)wglGetProcAddress("glDispatchCompute");
	gl.bindImageTexture = (PFNGLBINDIMAGETEXTUREPROC)wglGetProcAddress("glBindImageTexture");
	gl.memoryBarrier = (PFNGLMEMORYBARRIERPROC)wglGetProcAddress("glMemoryBarrier");
	gl.uniform4f = (PFNGLUNIFORM4FPROC)wglGetProcAddress("glUniform4f");
	gl.uniform2i = (PFNGLUNIFORM2IPROC)wglGetProcAddress("glUniform2i");
	gl.deleteShader = (PFNGLDELETESHADERPROC)wglGetProcAddress("glDeleteShader");
	gl.deleteProgram = (PFNGLDELETEPROGRAMPROC)wglGetProcAddress("glDeleteProgram");
#elif defined(__linux__)
	gl.dispatchCompute = glDispatchCompute;
	gl.bindImageTexture = glBindImageTexture;
	gl.memoryBarrier = glMemoryBarrier;
	gl.uniform4f = glUniform4f;
	gl.uniform2i = glUniform2i;
	gl.deleteShader = glDeleteShader;
	gl.deleteProgram = glDeleteProgram;
#endif
	return gl.dispatchCompute && gl.bindImageTexture && gl.memoryBarrier && gl.uniform4f && gl.uniform2i && gl.deleteShader && gl.deleteProgram;
}

} // namespace

bool ContactShadows::Initialize(const std::string& shaderDir) {
	Release();

	GLint major = 0;
	GLint minor = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	if (major < 4 || (major == 4 && minor < 3) || !LoadComputeFunctions()) {
		wxLogError("ContactShadows: Community Shaders' screen-space shadows need OpenGL 4.3 (compute shaders), the context has OpenGL %d.%d", major, minor);
		return false;
	}

	const std::string path = shaderDir + "/screenspaceshadows.comp";
	std::fstream file;
	PlatformUtil::OpenFileStream(file, path, std::ios_base::in | std::ios_base::binary);
	std::stringstream buffer;
	if (file)
		buffer << file.rdbuf();
	source = buffer.str();
	if (source.empty()) {
		wxLogError("ContactShadows: can't read the shader %s", path);
		return false;
	}
	return true;
}

void ContactShadows::Release() {
	if (program && gl.deleteProgram)
		gl.deleteProgram(program);
	program = 0;
	programSampleCount = 0;
	buildFailed = false;

	if (clearFramebuffer)
		glDeleteFramebuffers(1, &clearFramebuffer);
	clearFramebuffer = 0;

	if (outputTexture)
		glDeleteTextures(1, &outputTexture);
	outputTexture = 0;
	outputWidth = 0;
	outputHeight = 0;
	source.clear();
}

bool ContactShadows::BuildProgram(unsigned sampleCount) {
	if (program)
		gl.deleteProgram(program);
	program = 0;
	programSampleCount = sampleCount;

	// SAMPLE_COUNT after the version line
	std::string text = source;
	const size_t versionEnd = text.find('\n');
	text.insert(versionEnd == std::string::npos ? text.size() : versionEnd + 1, "#define SAMPLE_COUNT " + std::to_string(sampleCount) + "\n");

	const GLchar* src = text.c_str();
	GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
	glShaderSource(shader, 1, &src, nullptr);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		GLint length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
		std::string log(std::max(length, 1), '\0');
		glGetShaderInfoLog(shader, length, nullptr, log.data());
		wxLogError("ContactShadows: compiling screenspaceshadows.comp failed: %s", log.c_str());
		gl.deleteShader(shader);
		return false;
	}

	program = glCreateProgram();
	glAttachShader(program, shader);
	glLinkProgram(program);
	gl.deleteShader(shader);
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		GLint length = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
		std::string log(std::max(length, 1), '\0');
		glGetProgramInfoLog(program, length, nullptr, log.data());
		wxLogError("ContactShadows: linking screenspaceshadows.comp failed: %s", log.c_str());
		gl.deleteProgram(program);
		program = 0;
		return false;
	}
	return true;
}

bool ContactShadows::Render(GLuint depthTexture, int width, int height, const glm::vec4& lightProjection, const communityshaders::ScreenSpaceShadowsSettings& settings) {
	if (source.empty() || width <= 0 || height <= 0)
		return false;

	// ScreenSpaceShadows::GetScaledSampleCount, for the previewer's view
	const float areaScale = std::sqrt(static_cast<float>(width) * height / (1920.0f * 1080.0f));
	unsigned sampleCount = static_cast<unsigned>(std::round(settings.sampleCount * 60 * areaScale));
	sampleCount = std::max(((sampleCount + 7u) / 8u) * 8u, 8u);
	if (sampleCount != programSampleCount) {
		buildFailed = !BuildProgram(sampleCount);
	}
	if (buildFailed || !program)
		return false;

	if (width != outputWidth || height != outputHeight) {
		if (!outputTexture)
			glGenTextures(1, &outputTexture);
		glBindTexture(GL_TEXTURE_2D, outputTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindTexture(GL_TEXTURE_2D, 0);
		outputWidth = width;
		outputHeight = height;

		if (!clearFramebuffer)
			glGenFramebuffers(1, &clearFramebuffer);
	}

	GLint previousFramebuffer = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
	GLint viewport[4] = {};
	glGetIntegerv(GL_VIEWPORT, viewport);
	GLfloat clearColor[4] = {};
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);

	// ScreenSpaceShadows::Prepass: white where the ray march writes nothing
	glBindFramebuffer(GL_FRAMEBUFFER, clearFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0);
	glViewport(0, 0, width, height);
	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
	glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

	// The light's pixel coordinates with OpenGL's rows (bottom up) and depth range (-1 to 1).
	float light[4] = {lightProjection.x, -lightProjection.y, lightProjection.z, lightProjection.w};
	int viewportSize[2] = {width, height};
	int minRenderBounds[2] = {0, 0};
	int maxRenderBounds[2] = {width, height};
	const Bend::DispatchList dispatchList = Bend::BuildDispatchList(light, viewportSize, minRenderBounds, maxRenderBounds, true);

	// ScreenSpaceShadows::SetupResources' point sampler, clamped to a border of the far depth
	glActiveTexture(GL_TEXTURE0 + DepthTextureUnit);
	glBindTexture(GL_TEXTURE_2D, depthTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	const GLfloat border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

	glUseProgram(program);
	gl.bindImageTexture(0, outputTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
	auto location = [this](const char* name) { return glGetUniformLocation(program, name); };
	glUniform1i(location("DepthTexture"), DepthTextureUnit);
	glUniform1i(location("OutputTexture"), 0);
	gl.uniform4f(location("LightCoordinate"),
				 dispatchList.LightCoordinate_Shader[0],
				 dispatchList.LightCoordinate_Shader[1],
				 dispatchList.LightCoordinate_Shader[2],
				 dispatchList.LightCoordinate_Shader[3]);
	// ScreenSpaceShadows::DrawShadows
	glUniform1f(location("FarDepthValue"), 1.0f);
	glUniform1f(location("NearDepthValue"), 0.0f);
	glUniform2f(location("InvDepthTextureSize"), 1.0f / width, 1.0f / height);
	glUniform2f(location("DynamicRes"), 1.0f, 1.0f);
	glUniform1f(location("SurfaceThickness"), settings.surfaceThickness);
	glUniform1f(location("BilinearThreshold"), settings.bilinearThreshold);
	glUniform1f(location("ShadowContrast"), settings.shadowContrast);

	const GLint waveOffset = location("WaveOffset");
	for (int i = 0; i < dispatchList.DispatchCount; i++) {
		const Bend::DispatchData& dispatch = dispatchList.Dispatch[i];
		gl.uniform2i(waveOffset, dispatch.WaveOffset_Shader[0], dispatch.WaveOffset_Shader[1]);
		gl.dispatchCompute(dispatch.WaveCount[0], dispatch.WaveCount[1], dispatch.WaveCount[2]);
	}

	gl.memoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
	glUseProgram(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	return true;
}
