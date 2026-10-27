/*
 * GameLighting — see GameLighting.h.
 */

#include "GameLighting.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <wx/log.h>

#include <algorithm>
#include <cmath>

using namespace nifly;

namespace {

// The engine's shadow bias and filter radius aren't public; these are the
// previewer's, in shadow map texels.
constexpr float kShadowBiasTexels = 1.5f;
constexpr float kShadowRadiusTexels = 3.0f;

} // namespace

GameLighting::ShapeFlags GameLighting::ReadShapeFlags(NifFile& nif, NiShape* shape) {
	ShapeFlags flags;
	if (!shape)
		return flags;

	auto* shader = dynamic_cast<BSLightingShaderProperty*>(nif.GetShader(shape));
	if (!shader)
		return flags;

	flags.cast = (shader->shaderFlags1 & SLSF1_CAST_SHADOWS) != 0;
	flags.receive = (shader->shaderFlags1 & SLSF1_RECEIVE_SHADOWS) != 0;
	return flags;
}

bool GameLighting::Initialize(GLSurface* surface, const std::string& shaderDir) {
	Release();

	depthShader = GLShader(shaderDir + "/shadowmap.vert", shaderDir + "/shadowmap.frag");
	std::string error;
	if (depthShader.GetError(&error)) {
		wxLogError("GameLighting: %s", error);
		return false;
	}

	while (glGetError() != GL_NO_ERROR)
		;

	glGenBuffers(1, &skinRotationBuffer);

	glGenTextures(1, &shadowTexture);
	glBindTexture(GL_TEXTURE_2D, shadowTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, MapSize, MapSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glBindTexture(GL_TEXTURE_2D, 0);

	GLint previousFramebuffer = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTexture, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);

	GLenum glError = glGetError();
	if (glError != GL_NO_ERROR) {
		wxLogError("GameLighting: creating the shadow map failed (OpenGL error 0x%04X)", glError);
		Release();
		return false;
	}

	gls = surface;
	gls->preRenderMeshes = [this]() { RenderShadowMap(); };
	gls->setupMeshShader = [this](Mesh* m, GLShader& shader) { SetupMeshShader(m, shader); };
	return true;
}

void GameLighting::Release() {
	if (gls) {
		gls->preRenderMeshes = nullptr;
		gls->setupMeshShader = nullptr;
		gls = nullptr;
	}

	if (framebuffer) {
		glDeleteFramebuffers(1, &framebuffer);
		framebuffer = 0;
	}

	if (shadowTexture) {
		glDeleteTextures(1, &shadowTexture);
		shadowTexture = 0;
	}

	if (skinRotationBuffer) {
		glDeleteBuffers(1, &skinRotationBuffer);
		skinRotationBuffer = 0;
	}

	shadowRendered = false;
	hasBounds = false;
	boundsCasters.clear();
}

void GameLighting::SetSun(const Vector3& direction, const Vector3& color, const std::array<std::array<float, 4>, 3>& ambient) {
	sunDirection = direction;
	sunColor = color;
	for (int channel = 0; channel < 3; channel++) {
		for (int i = 0; i < 4; i++)
			ambientMatrix[channel][i] = ambient[channel][i];
	}
}

void GameLighting::SetShadowsEnabled(bool enable) {
	shadowsEnabled = enable;
	hasBounds = false;
}

void GameLighting::SetShapeFlags(const std::string& meshName, const ShapeFlags& flags) {
	shapeFlags[meshName] = flags;
}

void GameLighting::RenderShadowMap() {
	shadowRendered = false;
	if (!shadowsEnabled || !gls)
		return;

	std::vector<Mesh*> casters;
	std::vector<std::string> casterNames;
	for (auto& m : gls->GetMeshes()) {
		if (!m->bVisible || m->nTris == 0 || !m->genBuffers || m->rendermode != Mesh::RenderMode::Normal)
			continue;

		auto it = shapeFlags.find(m->shapeName);
		if (it == shapeFlags.end() || !it->second.cast)
			continue;

		casters.push_back(m);
		casterNames.push_back(m->shapeName);
	}

	if (casters.empty())
		return;

	// The vertices the frame shows: the posed ones while an actor frame
	// renders, the rest pose otherwise. The bounds keep both.
	std::sort(casterNames.begin(), casterNames.end());
	if (casterNames != boundsCasters) {
		boundsCasters = casterNames;
		hasBounds = false;
	}
	for (auto& m : casters) {
		for (int i = 0; i < m->nVerts; i++) {
			Vector3 p = m->TransformPosMeshToModel(m->verts[i]);
			if (!hasBounds) {
				boundsMin = p;
				boundsMax = p;
				hasBounds = true;
				continue;
			}
			boundsMin.x = std::min(boundsMin.x, p.x);
			boundsMin.y = std::min(boundsMin.y, p.y);
			boundsMin.z = std::min(boundsMin.z, p.z);
			boundsMax.x = std::max(boundsMax.x, p.x);
			boundsMax.y = std::max(boundsMax.y, p.y);
			boundsMax.z = std::max(boundsMax.z, p.z);
		}
	}
	if (!hasBounds)
		return;

	glm::vec3 toSun(sunDirection.x, sunDirection.y, sunDirection.z);
	if (glm::length(toSun) <= 0.0f)
		return;
	toSun = glm::normalize(toSun);

	// The map covers the sphere around the bounds, its radius rounded up to
	// half units so that small changes of the bounds keep the texel size.
	glm::vec3 center((boundsMin.x + boundsMax.x) * 0.5f, (boundsMin.y + boundsMax.y) * 0.5f, (boundsMin.z + boundsMax.z) * 0.5f);
	float radius = (boundsMax - boundsMin).length() * 0.5f;
	radius = std::max(std::ceil(radius * 2.0f) * 0.5f, 0.5f);

	glm::vec3 up = std::abs(toSun.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
	glm::mat4x4 view = glm::lookAt(center + toSun * (2.0f * radius), center, up);
	glm::mat4x4 projection = glm::ortho(-radius, radius, -radius, radius, radius, 3.0f * radius);

	// Texels stay where they are on the model while the bounds move.
	glm::vec4 origin = projection * view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	glm::vec2 originTexels = glm::vec2(origin) * (MapSize * 0.5f);
	glm::vec2 snap = (glm::round(originTexels) - originTexels) / (MapSize * 0.5f);
	projection[3][0] += snap.x;
	projection[3][1] += snap.y;

	GLint previousFramebuffer = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
	GLint viewport[4] = {};
	glGetIntegerv(GL_VIEWPORT, viewport);

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glViewport(0, 0, MapSize, MapSize);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_TRUE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(2.0f, 4.0f);
	glClear(GL_DEPTH_BUFFER_BIT);

	depthShader.Begin();
	GLint program = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	GLint vertexAlphaLoc = glGetUniformLocation(program, "bVertexAlpha");
	depthShader.SetMatrixProjection(projection);

	for (auto& m : casters) {
		m->UpdateBuffers();

		GLuint diffuse = m->material ? m->material->GetTexID(0) : 0;
		bool alphaTest = (m->alphaFlags & (1 << 9)) != 0 && diffuse && m->textured && m->texcoord;
		bool vertexAlpha = alphaTest && m->vertexColors && m->vertexAlpha && m->valpha;

		depthShader.SetMatrixModelView(view, m->matModel);
		depthShader.SetAlphaThreshold(alphaTest ? m->alphaThreshold / 255.0f : -1.0f);
		depthShader.SetProperties(m->prop);
		if (vertexAlphaLoc >= 0)
			glUniform1i(vertexAlphaLoc, vertexAlpha ? GL_TRUE : GL_FALSE);
		if (alphaTest)
			depthShader.BindTexture(0, diffuse, "texDiffuse");

		glBindVertexArray(m->vao);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);

		// The skin rotation another mesh's draw left in the buffer isn't this mesh's.
		glDisableVertexAttribArray(SkinRotationXAttribute);
		glDisableVertexAttribArray(SkinRotationYAttribute);

		glBindBuffer(GL_ARRAY_BUFFER, m->vbo[0]);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (GLvoid*)0);

		if (vertexAlpha) {
			glBindBuffer(GL_ARRAY_BUFFER, m->vbo[5]);
			glEnableVertexAttribArray(5);
			glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, 0, (GLvoid*)0);
		}

		if (alphaTest) {
			glBindBuffer(GL_ARRAY_BUFFER, m->vbo[6]);
			glEnableVertexAttribArray(6);
			glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid*)0);
		}

		glDrawElements(GL_TRIANGLES, m->nTris * 3, GL_UNSIGNED_SHORT, (GLvoid*)0);

		glDisableVertexAttribArray(6);
		glDisableVertexAttribArray(5);
		glDisableVertexAttribArray(0);
	}

	depthShader.End();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE0);

	glDisable(GL_POLYGON_OFFSET_FILL);
	glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

	shadowMatrix = projection * view;
	shadowRendered = true;
}

void GameLighting::SetupMeshShader(Mesh* m, GLShader& WXUNUSED(shader)) {
	GLint program = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	GLint loc = glGetUniformLocation(program, "sunColor");
	if (loc < 0)
		return;

	glUniform3f(loc, sunColor.x, sunColor.y, sunColor.z);

	loc = glGetUniformLocation(program, "sunDirection");
	if (loc >= 0)
		glUniform3f(loc, sunDirection.x, sunDirection.y, sunDirection.z);

	loc = glGetUniformLocation(program, "ambientMatrix");
	if (loc >= 0)
		glUniformMatrix4fv(loc, 1, GL_FALSE, (GLfloat*)&ambientMatrix);

	// The skin rotation goes with the mesh's vertex arrays, which RenderMesh binds for the draw.
	const std::vector<float>* rotation = skinRotationSource ? skinRotationSource(m) : nullptr;
	loc = glGetUniformLocation(program, "bSkinRotation");
	if (loc >= 0)
		glUniform1i(loc, rotation ? GL_TRUE : GL_FALSE);
	glBindVertexArray(m->vao);
	if (rotation) {
		glBindBuffer(GL_ARRAY_BUFFER, skinRotationBuffer);
		glBufferData(GL_ARRAY_BUFFER, rotation->size() * sizeof(float), rotation->data(), GL_STREAM_DRAW);
		glEnableVertexAttribArray(SkinRotationXAttribute);
		glVertexAttribPointer(SkinRotationXAttribute, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (GLvoid*)0);
		glEnableVertexAttribArray(SkinRotationYAttribute);
		glVertexAttribPointer(SkinRotationYAttribute, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (GLvoid*)(3 * sizeof(float)));
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
	else {
		glDisableVertexAttribArray(SkinRotationXAttribute);
		glDisableVertexAttribArray(SkinRotationYAttribute);
	}
	glBindVertexArray(0);

	// The shadow sampler always gets its own unit: it may not share one with the materials' samplers.
	loc = glGetUniformLocation(program, "texShadow");
	if (loc >= 0) {
		glUniform1i(loc, ShadowTextureUnit);
		glActiveTexture(GL_TEXTURE0 + ShadowTextureUnit);
		glBindTexture(GL_TEXTURE_2D, shadowTexture);
		glActiveTexture(GL_TEXTURE0);
	}

	auto it = shapeFlags.find(m->shapeName);
	bool receive = shadowRendered && it != shapeFlags.end() && it->second.receive;
	loc = glGetUniformLocation(program, "bSunShadow");
	if (loc >= 0)
		glUniform1i(loc, receive ? GL_TRUE : GL_FALSE);
	if (!receive)
		return;

	loc = glGetUniformLocation(program, "matShadow");
	if (loc >= 0)
		glUniformMatrix4fv(loc, 1, GL_FALSE, (GLfloat*)&shadowMatrix);

	loc = glGetUniformLocation(program, "shadowBias");
	if (loc >= 0)
		glUniform1f(loc, kShadowBiasTexels / MapSize);

	loc = glGetUniformLocation(program, "shadowRadius");
	if (loc >= 0)
		glUniform1f(loc, kShadowRadiusTexels / MapSize);
}
