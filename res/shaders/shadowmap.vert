#version 330

/*
 * Depth of a mesh as seen from a directional light, for the leveled list
 * previewer's shadow map (src/render/GameLighting.cpp).
 */

uniform mat4 matProjection;
uniform mat4 matModelView;

uniform bool bVertexAlpha;

layout(location = 0) in vec3 vertexPosition;
layout(location = 5) in float vertexAlpha;
layout(location = 6) in vec2 vertexUV;

out float vAlpha;
out vec2 vUV;

void main(void)
{
	vAlpha = bVertexAlpha ? vertexAlpha : 1.0;
	vUV = vertexUV;
	gl_Position = matProjection * matModelView * vec4(vertexPosition, 1.0);
}
