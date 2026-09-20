#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 interpTexCoords;
layout(location = 1) in vec4 interpDiffuse;
layout(location = 2) in vec4 interpFogColor;
layout(location = 3) in vec4 interpEnvDiffuse;
layout(location = 4) in vec4 interpFlags; // colorOp, useTexCoords, noVertexBuffer, noFog
layout(location = 5) in vec3 interpFog;   // viewZ, fogNear, fogFar

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D tex;

const float alphaThreshold = 4.0 / 255.0;

#define OP_MODULATE 0.0
#define OP_ADD 1.0

void main() {
	vec4 fragArg1 = interpFlags.y > 0.5 ? texture(tex, interpTexCoords) : interpDiffuse;
	vec4 fragArg2 = interpFlags.z > 0.5 ? interpDiffuse : interpEnvDiffuse;

	vec4 fragColor;
	if (interpFlags.x < 0.5) {
		fragColor = fragArg1 * fragArg2;
	} else if (interpFlags.x < 1.5) {
		// In EoSD, add only applies to RGB; alpha still modulates.
		fragColor = vec4(min(fragArg1.rgb + fragArg2.rgb, vec3(1.0)), fragArg1.a * fragArg2.a);
	} else {
		fragColor = fragArg1;
	}

	if (interpFlags.w < 0.5) {
		float fogCoefficient = clamp((interpFog.z - interpFog.x) / (interpFog.z - interpFog.y), 0.0, 1.0);
		fragColor.rgb = mix(interpFogColor.rgb, fragColor.rgb, fogCoefficient);
	}

	if (fragColor.a < alphaThreshold) {
		discard;
	}

	outColor = fragColor;
}
