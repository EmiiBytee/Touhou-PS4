#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 interpTexCoords;
layout(location = 1) in vec4 interpDiffuse;
layout(location = 2) in vec4 interpFogColor;
layout(location = 3) in vec4 interpTextureFactor;
layout(location = 4) in vec4 interpFlags0;
layout(location = 5) in vec4 interpFlags1;
layout(location = 6) in vec3 interpFog;
layout(location = 7) in vec4 interpDebug;

layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D tex;

void main() {
    bool useTexture = interpFlags0.w > 0.5;
    vec4 texColor = useTexture ? texture(tex, interpTexCoords) : vec4(1.0);
    vec4 argColor = interpDiffuse;
    if (interpFlags0.z > 0.5 && interpFlags0.z < 1.5) {
        argColor = vec4(1.0);
    } else if (interpFlags0.z >= 1.5) {
        argColor = interpTextureFactor;
    }

    vec4 finalColor = argColor;
    if (useTexture) {
        if (interpFlags0.x < 0.5) finalColor.rgb = texColor.rgb * argColor.rgb;
        else if (interpFlags0.x < 1.5) finalColor.rgb = min(texColor.rgb + argColor.rgb, vec3(1.0));
        else if (interpFlags0.x < 2.5) finalColor.rgb = texColor.rgb;
        else finalColor.rgb = argColor.rgb;

        if (interpFlags0.y < 0.5) finalColor.a = texColor.a * argColor.a;
        else if (interpFlags0.y < 1.5) finalColor.a = min(texColor.a + argColor.a, 1.0);
        else if (interpFlags0.y < 2.5) finalColor.a = texColor.a;
        else finalColor.a = argColor.a;
    }

    if (interpFlags1.y > 0.5 && finalColor.a < interpFlags1.z) {
        discard;
    }
    if (interpFlags1.w > 0.5) {
        float f = clamp((interpFog.z - interpFog.x) / (interpFog.z - interpFog.y), 0.0, 1.0);
        finalColor.rgb = mix(interpFogColor.rgb, finalColor.rgb, f);
    }
    // Debug views, switched by a file on /data (see Gnm::Setup): they answer what the
    // shader actually receives, which no CPU-side log can show.
    const float mode = interpDebug.x;
    if (mode > 0.5) {
        if (mode < 1.5) {
            outColor = vec4(vec3(interpDiffuse.a), 1.0);       // vertex alpha
        } else if (mode < 2.5) {
            outColor = vec4(vec3(texColor.a), 1.0);            // texture alpha
        } else if (mode < 3.5) {
            outColor = vec4(interpDiffuse.rgb, 1.0);           // vertex colour
        } else if (mode < 4.5) {
            outColor = vec4(interpTexCoords, 0.0, 1.0);        // texture coordinates
        } else if (mode < 5.5) {
            outColor = vec4(vec3(finalColor.a), 1.0);          // alpha the blender would use
        } else if (mode < 6.5) {
            outColor = vec4(texColor.rgb, 1.0);                // texture colour, untouched
        } else if (mode < 7.5) {
            outColor = vec4(finalColor.rgb, 1.0);              // colour before blending
        } else {
            // Which path drew this pixel: green = screen space (the game's batched
            // sprites), red = world space (the unit quad placed by the model matrix).
            outColor = interpFlags1.x > 0.5 ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
        }
        return;
    }
    outColor = finalColor;
}
