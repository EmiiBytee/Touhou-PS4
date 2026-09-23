#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 interpTexCoords;
layout(location = 1) in vec4 interpDiffuse;
layout(location = 2) in vec4 interpFogColor;
layout(location = 3) in vec4 interpTextureFactor;
layout(location = 4) in vec4 interpFlags0;
layout(location = 5) in vec4 interpFlags1;
layout(location = 6) in vec4 interpFlags2;
layout(location = 7) in vec3 interpFog;
layout(location = 8) in vec4 interpDebug;

layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D tex;

// Keep these in step with the enumerations in d3d8_gnm.cpp.
const float ARG_DIFFUSE = 0.0;
const float ARG_TEXTURE = 1.0;
const float ARG_TFACTOR = 2.0;
const float OP_MODULATE = 0.0;
const float OP_SELECTARG1 = 1.0;
const float OP_DISABLE = 2.0;

vec4 SelectArgument(float which, vec4 texColor) {
    if (which > ARG_TFACTOR - 0.5) return interpTextureFactor;
    if (which > ARG_TEXTURE - 0.5) return texColor;
    return interpDiffuse;
}

void main() {
    bool useTexture = interpFlags1.x > 0.5;
    vec4 texColor = useTexture ? texture(tex, interpTexCoords) : vec4(1.0);

    // Direct3D 8 resolves the colour and the alpha channel through separate operations with
    // separate arguments, and TH08 relies on that: the same draw commonly takes its colour
    // from the texture factor and its alpha from the texture, or the reverse.
    vec3 rgbArg1 = SelectArgument(interpFlags0.z, texColor).rgb;
    vec3 rgbArg2 = SelectArgument(interpFlags0.w, texColor).rgb;
    float alphaArg1 = SelectArgument(interpFlags2.x, texColor).a;
    float alphaArg2 = SelectArgument(interpFlags2.y, texColor).a;

    vec4 finalColor;
    if (interpFlags0.x > OP_DISABLE - 0.5) finalColor.rgb = interpDiffuse.rgb;
    else if (interpFlags0.x > OP_SELECTARG1 - 0.5) finalColor.rgb = rgbArg1;
    else finalColor.rgb = rgbArg1 * rgbArg2;

    if (interpFlags0.y > OP_DISABLE - 0.5) finalColor.a = interpDiffuse.a;
    else if (interpFlags0.y > OP_SELECTARG1 - 0.5) finalColor.a = alphaArg1;
    else finalColor.a = alphaArg1 * alphaArg2;

    // The alpha test is emulated here rather than with the GPU's own, whose reference lives
    // in a register this command stream does not otherwise touch.
    float alphaFunc = interpFlags1.y;
    if (alphaFunc > 0.5) {
        float reference = interpFlags1.z;
        bool passes = true;
        if (alphaFunc < 1.5) passes = false;                            // D3DCMP_NEVER
        else if (alphaFunc < 2.5) passes = finalColor.a < reference;     // LESS
        else if (alphaFunc < 3.5) passes = finalColor.a == reference;    // EQUAL
        else if (alphaFunc < 4.5) passes = finalColor.a <= reference;    // LESSEQUAL
        else if (alphaFunc < 5.5) passes = finalColor.a > reference;     // GREATER
        else if (alphaFunc < 6.5) passes = finalColor.a != reference;    // NOTEQUAL
        else if (alphaFunc < 7.5) passes = finalColor.a >= reference;    // GREATEREQUAL
        if (!passes) discard;
    }

    if (interpFlags1.w > 0.5) {
        float f = clamp((interpFog.z - interpFog.x) / (interpFog.z - interpFog.y), 0.0, 1.0);
        finalColor.rgb = mix(interpFogColor.rgb, finalColor.rgb, f);
    }

    // Debug views, switched by a file on /data (see Direct3DCreate8): they answer what the
    // shader actually receives, which no CPU-side log can show.
    const float mode = interpDebug.x;
    if (mode > 0.5) {
        if (mode < 1.5) outColor = vec4(vec3(interpDiffuse.a), 1.0);   // vertex alpha
        else if (mode < 2.5) outColor = vec4(vec3(texColor.a), 1.0);   // texture alpha
        else if (mode < 3.5) outColor = vec4(interpDiffuse.rgb, 1.0);  // vertex colour
        else if (mode < 4.5) outColor = vec4(interpTexCoords, 0.0, 1.0);
        else if (mode < 5.5) outColor = vec4(vec3(finalColor.a), 1.0); // alpha the blender uses
        else if (mode < 6.5) outColor = vec4(texColor.rgb, 1.0);       // texture colour
        else outColor = vec4(finalColor.rgb, 1.0);                     // colour before blending
        return;
    }
    outColor = finalColor;
}
