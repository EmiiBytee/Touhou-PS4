#version 450
#extension GL_ARB_separate_shader_objects : enable

// PCB fixed-function emulation for PS4 GNM. Values needed by the pixel shader travel as
// varyings because the PS4 shader compiler cannot share one descriptor table between the
// constant buffer and texture without overlapping them.
layout(binding = 0) uniform Constants {
    mat4 modelview;
    mat4 projection;
    mat4 texmatrix;
    vec4 fogColor;
    vec4 textureFactor;
    vec4 fogRange;
    vec4 flags0;   // RGB op, alpha op, texture argument, texture enabled
    vec4 flags1;   // screen space, alpha test, alpha reference, fog enabled
    vec4 viewport;
    vec4 debug;    // x: picture-in-the-shader debug mode, 0 = off
} c;

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texCoords;
layout(location = 2) in vec4 diffuse;

layout(location = 0) out vec2 interpTexCoords;
layout(location = 1) out vec4 interpDiffuse;
layout(location = 2) out vec4 interpFogColor;
layout(location = 3) out vec4 interpTextureFactor;
layout(location = 4) out vec4 interpFlags0;
layout(location = 5) out vec4 interpFlags1;
layout(location = 6) out vec3 interpFog;
layout(location = 7) out vec4 interpDebug;

out gl_PerVertex {
    vec4 gl_Position;
};

void main() {
    interpDiffuse = diffuse;
    interpFogColor = c.fogColor;
    interpTextureFactor = c.textureFactor;
    interpFlags0 = c.flags0;
    interpFlags1 = c.flags1;
    interpDebug = c.debug;

    if (c.flags1.x > 0.5) {
        float x = (position.x - c.viewport.x) / c.viewport.z * 2.0 - 1.0;
        float y = 1.0 - (position.y - c.viewport.y) / c.viewport.w * 2.0;
        gl_Position = vec4(x, y, position.z, 1.0);
        interpTexCoords = texCoords;
        interpFog = vec3(position.z, c.fogRange.x, c.fogRange.y);
    } else {
        vec4 viewCoordinates = c.modelview * vec4(position, 1.0);
        gl_Position = c.projection * viewCoordinates;
        interpTexCoords = (c.texmatrix * vec4(texCoords, 1.0, 0.0)).xy;
        interpFog = vec3(length(viewCoordinates.xyz), c.fogRange.x, c.fogRange.y);
    }
}
