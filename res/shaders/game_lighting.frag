#version 330

/*
 * BodySlide and Outfit Studio
 * Shaders by jonwd7 and ousnius
 * https://github.com/ousnius/BodySlide-and-Outfit-Studio
 * http://www.niftools.org/
 *
 * The leveled list previewer's copy of default.frag, lit like the game
 * outdoors (src/render/GameLighting.cpp): the sun, darkened by its shadow,
 * and the directional ambient light, evaluated with the shading normal as
 * the game's Lighting.hlsl does (mul(DirectionalAmbient, float4(normal, 1))).
 * The sun's shadow is filtered like the game's shadow mask (Utility.hlsl,
 * SHADOWFILTER 4: eight spiral samples, rotated per pixel and frame); the
 * frames are averaged, as the game's temporal anti-aliasing does.
 */

uniform sampler2D texDiffuse;
uniform sampler2D texNormal;
uniform samplerCube texCubemap;
uniform sampler2D texEnvMask;
uniform sampler2D texSpecular;
uniform sampler2D texBacklight;
uniform sampler2D texLightmask;
uniform sampler2D texGlowmap;

uniform bool bLightEnabled;
uniform bool bShowTexture;
uniform bool bShowMask;
uniform bool bShowWeight;
uniform bool bWireframe;

uniform bool bNormalMap;
uniform bool bModelSpace;
uniform bool bCubemap;
uniform bool bEnvMask;
uniform bool bSpecular;
uniform bool bEmissive;
uniform bool bBacklight;
uniform bool bRimlight;
uniform bool bSoftlight;
uniform bool bGlowmap;

uniform mat4 matModel;
uniform mat4 matView;
uniform mat4 matModelViewInverse;
uniform mat3 mv_normalMatrix;

struct Properties
{
	vec2 uvOffset;
	vec2 uvScale;
	vec3 specularColor;
	float specularStrength;
	float shininess;
	float envReflection;
	vec3 emissiveColor;
	float emissiveMultiple;
	float alpha;
	float rimlightPower;
	float softlighting;
};
uniform Properties prop;
uniform float alphaThreshold;


uniform vec3 sunColor;
// Directional ambient, model space: column c holds color channel c's factors for (normal, 1)
uniform mat4 ambientMatrix;

uniform bool bSunShadow;
uniform mat4 matShadow;
uniform sampler2DShadow texShadow;
uniform float shadowBias;
uniform float shadowRadius;

in vec3 lightSun;
in vec3 modelPos;

in vec3 viewDir;
in vec3 n;
in mat3 mv_tbn;

in float maskFactor;
in vec3 weightColor;

in vec4 vColor;
in vec2 vUV;

out vec4 fragColor;

vec3 normal = vec3(0.0);
float specFactor = 0.0;

vec2 uv = vec2(0.0);
vec3 albedo = vec3(0.0);
vec3 emissive = vec3(0.0);

vec4 baseMap = vec4(0.0);
vec4 normalMap = vec4(0.0);
vec4 specMap = vec4(0.0);
vec4 envMask = vec4(0.0);
vec4 backlightMap = vec4(0.0);

vec3 tonemap(in vec3 x)
{
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;

	return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// Random::SpiralSampleOffsets8 (Common/Random.hlsli)
const vec2 spiralSampleOffsets8[8] = vec2[8](
	vec2(-0.7071, 0.7071),
	vec2(-0.0000, -0.8750),
	vec2(0.5303, 0.5303),
	vec2(-0.6250, -0.0000),
	vec2(0.3536, -0.3536),
	vec2(-0.0000, 0.3750),
	vec2(-0.1768, -0.1768),
	vec2(0.1250, 0.0000));

// Random::InterleavedGradientNoise(pxCoord, frame) (Common/Random.hlsli)
float interleavedGradientNoise(in vec2 pxCoord, in int frame)
{
	float frameStep = float(frame % 16) * 0.0625;
	pxCoord.x += frameStep * 4.7526;
	pxCoord.y += frameStep * 3.1914;
	vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
	return fract(magic.z * fract(dot(pxCoord, magic.xy)));
}

// Share of the sun reaching the fragment: SampleShadowPCF (Utility.hlsl) for each of the 16 frames of the noise
float sunShadow()
{
	vec4 clipPos = matShadow * vec4(modelPos, 1.0);
	vec3 mapPos = clipPos.xyz / clipPos.w * 0.5 + 0.5;
	if (any(lessThan(mapPos, vec3(0.0))) || any(greaterThan(mapPos, vec3(1.0))))
		return 1.0;

	float compareValue = mapPos.z - shadowBias;
	float visibility = 0.0;
	for (int frame = 0; frame < 16; frame++)
	{
		float angle = 6.28318530718 * interleavedGradientNoise(gl_FragCoord.xy, frame);
		float s = sin(angle);
		float c = cos(angle);
		for (int i = 0; i < 8; i++)
		{
			vec2 v = spiralSampleOffsets8[i];
			vec2 offset = vec2(v.x * c - v.y * s, v.x * s + v.y * c) * shadowRadius;
			visibility += texture(texShadow, vec3(mapPos.xy + offset, compareValue));
		}
	}
	return visibility / 128.0;
}

void sunLight(in vec3 lightColor, in vec3 lightDir, inout vec3 outDiffuse, inout vec3 outSpec)
{
	vec3 halfDir = normalize(lightDir + viewDir);
	float NdotL = max(dot(normal, lightDir), 0.0);
	float NdotH = max(dot(normal, halfDir), 0.0);
	float NdotV = max(dot(normal, viewDir), 0.0);

	outDiffuse += NdotL * lightColor;
	outSpec += clamp(prop.specularColor * prop.specularStrength * specFactor * pow(NdotH, prop.shininess), 0.0, 1.0) * lightColor;

	// Back lighting not really useful for the current light setup of multiple directional lights
	//if (bBacklight && bShowTexture)
	//{
	//	float NdotNegL = max(dot(normal, -lightDir), 0.0);
	//	vec3 backlight = backlightMap.rgb * NdotNegL * light.diffuse;
	//	emissive += backlight;
	//}

	vec3 lightMask = normalize(vec3(0.0, 0.0, 0.5));
	if (bRimlight || bSoftlight)
	{
		lightMask = texture(texLightmask, uv).rgb;
	}

	// Rim lighting not really useful for the current light setup of multiple directional lights
	//if (bRimlight)
	//{
	//	vec3 rim = lightMask * pow(vec3(1.0 - NdotV), vec3(prop.rimlightPower));
	//	rim *= smoothstep(-0.2, 1.0, dot(-lightDir, viewDir));
	//	emissive += rim * light.diffuse;
	//}

	// Soft Lighting
	if (bSoftlight)
	{
		float wrap = (dot(normal, lightDir) + prop.softlighting) / (1.0 + prop.softlighting);
		vec3 soft = max(wrap, 0.0) * lightMask * smoothstep(1.0, 0.0, NdotL);
		soft *= sqrt(clamp(prop.softlighting, 0.0, 1.0));
		emissive += soft * lightColor;
	}
}

void main(void)
{
	uv = vUV * prop.uvScale + prop.uvOffset;
	vec4 color = vColor;
	albedo = vColor.rgb;

	if (!bWireframe)
	{
		if (bShowTexture)
		{
			// Diffuse Texture
			baseMap = texture(texDiffuse, uv);
			albedo *= baseMap.rgb;
			color.a *= baseMap.a;

			// Diffuse texture without lighting
			color.rgb = albedo;

			if (bLightEnabled)
			{
				if (bNormalMap)
				{
					normalMap = texture(texNormal, uv);

					if (bModelSpace && bSpecular)
					{
						// Dedicated Specular Map
						specMap = texture(texSpecular, uv);
					}
				}

				if (bCubemap)
				{
					if (bEnvMask)
					{
						// Environment Mask
						envMask = texture(texEnvMask, uv);
					}
				}

				if (bBacklight)
				{
					backlightMap = texture(texBacklight, uv);
				}
			}
		}

		if (bLightEnabled)
		{
			// Lighting with or without textures
			vec3 outDiffuse = vec3(0.0);
			vec3 outSpecular = vec3(0.0);

			specFactor = 0.0;

			if (bShowTexture && bNormalMap)
			{
				if (bModelSpace)
				{
					// Model Space Normal Map
					normal = normalize(normalMap.rgb * 2.0 - 1.0);
					normal.r = -normal.r;
					normal = mat3(matView) * normal;
					normal = normalize(normal);

					if (bSpecular)
					{
						specFactor = specMap.r;
					}
				}
				else
				{
					// Tangent Space Normal Map
					normal = normalize(mv_tbn * (normalMap.rgb * 2.0 - 1.0));

					if (bSpecular)
					{
						specFactor = normalMap.a;
					}
				}
			}
			else
			{
				// Vertex normal for shading with disabled maps
				normal = mv_normalMatrix * n;
				normal = normalize(normal);
			}

			// The sun, darkened by its shadow, and the directional ambient light
			vec3 sun = sunColor;
			if (bSunShadow)
				sun *= sunShadow();
			sunLight(sun, lightSun, outDiffuse, outSpecular);

			vec3 modelNormal = normalize(transpose(mat3(matView)) * normal);
			outDiffuse += max(vec3(0.0), (vec4(modelNormal, 1.0) * ambientMatrix).rgb);

			if (bCubemap && bShowTexture)
			{
				vec3 reflected = reflect(-viewDir, normal);
				vec3 reflectedWS = vec3(matModel * (matModelViewInverse * vec4(reflected, 0.0)));

				vec4 cubeMap = texture(texCubemap, reflectedWS);
				cubeMap.rgb *= prop.envReflection;

				if (bEnvMask)
				{
					cubeMap.rgb *= envMask.r;
				}
				else
				{
					// No env mask, use specular factor (0.0 if no normal map either)
					cubeMap.rgb *= specFactor;
				}

				albedo += cubeMap.rgb;
			}

			// Emissive
			if (bEmissive)
			{
				emissive += prop.emissiveColor * prop.emissiveMultiple;

				// Glowmap
				if (bGlowmap)
				{
					vec4 glowMap = texture(texGlowmap, uv);
					emissive *= glowMap.rgb;
				}
			}

			color.rgb = albedo * (outDiffuse + emissive) + outSpecular;
		}

		if (bShowMask)
		{
			color.rgb *= maskFactor;
		}

		if (bShowWeight)
		{
			color.rgb *= weightColor;
		}

		color.rgb = tonemap(color.rgb) / tonemap(vec3(1.0));
	}
	else
	{
		color = vec4(color.rgb, 0.5);
	}

	color = clamp(color, 0.0, 1.0);

	fragColor = color;

	if (!bWireframe)
	{
		if (alphaThreshold != -1.0f)
			if (fragColor.a <= alphaThreshold) // GL_GREATER
				discard;

		fragColor.a *= prop.alpha;
	}
}
