#version 330

/*
 * Depth of a mesh as seen from a directional light, for the leveled list
 * previewer's shadow map (src/render/GameLighting.cpp). Alpha-tested meshes
 * cast the shape their diffuse texture's alpha leaves after the test.
 */

uniform sampler2D texDiffuse;

struct Properties
{
	vec2 uvOffset;
	vec2 uvScale;
};
uniform Properties prop;
uniform float alphaThreshold;

in float vAlpha;
in vec2 vUV;

void main(void)
{
	if (alphaThreshold != -1.0)
	{
		float alpha = vAlpha * texture(texDiffuse, vUV * prop.uvScale + prop.uvOffset).a;
		if (alpha <= alphaThreshold) // GL_GREATER
			discard;
	}
}
