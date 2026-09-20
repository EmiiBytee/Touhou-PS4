#version 450
#extension GL_ARB_separate_shader_objects : enable

// Fixed-function emulation for the PS4 GNM backend: same pipeline as ff.vert/ff.frag, but
// every per-draw value the pixel shader needs is passed through as a varying. psbc lays a
// stage's resources out in one table, and a uniform buffer and a texture in the same table
// overlap, so the pixel stage gets the texture and nothing else.
layout(binding = 0) uniform Constants {
	mat4 modelview;
	mat4 projection;
	mat4 texmatrix;
	vec4 fogColor;
	vec4 envDiffuse;
	vec4 fogRange;  // near, far, unused, unused
	vec4 flags;     // colorOp, useTexCoords, noVertexBuffer, noFog
} c;

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texCoords;
layout(location = 2) in vec4 diffuse;

layout(location = 0) out vec2 interpTexCoords;
layout(location = 1) out vec4 interpDiffuse;
layout(location = 2) out vec4 interpFogColor;
layout(location = 3) out vec4 interpEnvDiffuse;
layout(location = 4) out vec4 interpFlags;
layout(location = 5) out vec3 interpFog; // viewZ, fogNear, fogFar

out gl_PerVertex {
	vec4 gl_Position;
};

void main() {
	interpTexCoords = (c.texmatrix * vec4(texCoords, 1.0, 1.0)).xy;
	interpDiffuse = diffuse;
	interpFogColor = c.fogColor;
	interpEnvDiffuse = c.envDiffuse;
	interpFlags = c.flags;

	vec4 viewCoordinates = c.modelview * vec4(position, 1.0);
	interpFog = vec3(viewCoordinates.z, c.fogRange.x, c.fogRange.y);

	// No Y flip: the GPU's viewport transform already maps +Y to the top of the screen,
	// same as OpenGL shows it. Only the viewport rectangle needs converting (see Gnm.cpp).
	gl_Position = c.projection * viewCoordinates;
}
