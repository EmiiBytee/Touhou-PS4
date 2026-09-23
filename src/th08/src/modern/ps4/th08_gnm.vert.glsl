#version 450
#extension GL_ARB_separate_shader_objects : enable

// Direct3D 8 fixed-function emulation for PS4 GNM, the TH08 variant.
//
// The adapter applies the world, view and projection matrices on the CPU, but it stops at
// clip space and leaves the perspective divide to the GPU. That matters for the stages with
// 3D backgrounds: dividing on the CPU and handing over plain screen coordinates makes the
// rasterizer interpolate texture coordinates linearly in screen space instead of
// perspective-correctly, and it removes the near-plane clip, so geometry the camera passes
// through smears across the screen.
//
//   flags2.z = 0  position is clip space, straight from the projection matrix
//   flags2.z = 1  position is a pre-transformed D3DFVF_XYZRHW pixel coordinate
//
// Fog distance is eye-space and is computed by that same CPU transform, so it arrives per
// vertex instead of being derived here.
//
// Values the pixel shader needs travel as varyings: the PS4 shader compiler cannot share
// one descriptor table between a constant buffer and a texture without overlapping them.
layout(binding = 0) uniform Constants {
    vec4 fogColor;
    vec4 textureFactor;
    vec4 fogRange;     // start, end, unused, unused
    vec4 flags0;       // RGB op, alpha op, RGB arg1, RGB arg2
    vec4 flags1;       // texture enabled, alpha test func, alpha reference, fog enabled
    vec4 flags2;       // alpha arg1, alpha arg2, screen space, unused
    vec4 viewport;     // x, y, width, height
    vec4 debug;        // x: picture-in-the-shader debug mode, 0 = off
} c;

layout(location = 0) in vec4 position;
layout(location = 1) in float fogDistance;
layout(location = 2) in vec2 texCoords;
layout(location = 3) in vec4 diffuse;

layout(location = 0) out vec2 interpTexCoords;
layout(location = 1) out vec4 interpDiffuse;
layout(location = 2) out vec4 interpFogColor;
layout(location = 3) out vec4 interpTextureFactor;
layout(location = 4) out vec4 interpFlags0;
layout(location = 5) out vec4 interpFlags1;
layout(location = 6) out vec4 interpFlags2;
layout(location = 7) out vec3 interpFog;
layout(location = 8) out vec4 interpDebug;

out gl_PerVertex {
    vec4 gl_Position;
};

void main() {
    interpTexCoords = texCoords;
    interpDiffuse = diffuse;
    interpFogColor = c.fogColor;
    interpTextureFactor = c.textureFactor;
    interpFlags0 = c.flags0;
    interpFlags1 = c.flags1;
    interpFlags2 = c.flags2;
    interpFog = vec3(fogDistance, c.fogRange.x, c.fogRange.y);
    interpDebug = c.debug;

    if (c.flags2.z > 0.5) {
        // Already a pixel coordinate inside the Direct3D viewport rectangle. The hardware
        // viewport covers exactly that rectangle on screen, so this only has to reach NDC.
        float x = (position.x - c.viewport.x) / c.viewport.z * 2.0 - 1.0;
        float y = 1.0 - (position.y - c.viewport.y) / c.viewport.w * 2.0;
        gl_Position = vec4(x, y, position.z, 1.0);
    } else {
        // Clip space, straight from the projection matrix. Direct3D and this pipeline agree
        // that y = +1 is the top of the viewport -- the screen-space branch above maps the
        // top of the rectangle to +1 as well -- so it is passed through untouched and the
        // GPU does the divide, the near-plane clip and the perspective-correct interpolation.
        gl_Position = position;
    }
}
