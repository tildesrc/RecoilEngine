// Drop-in replacement for BAR's engine-side per-object SSBO on a GL 4.1 core
// profile, where SSBOs do not exist. The engine publishes the same bytes as a
// GL_TEXTURE_BUFFER on reserved texture unit 14; rebuilding the struct from its
// texels means nothing downstream of the declaration has to change.
//
// Replaces, verbatim:
//     layout(std140, binding=1) readonly buffer UniformsBuffer {
//         SUniformsBuffer uni[];
//     };
//
// Then rewrite every `uni[i]` as `fetchUni(i)`. Keep the SUniformsBuffer struct
// declaration above this block - it is what fetchUni fills in.
//
// The unit MUST be a literal: the engine lifts layout(binding) out of the source
// textually, before the preprocessor runs, so a #define is still an identifier
// at that point.
//
// See devtools/macos-port/SSBO-FALLBACK.md. The texel layout is dictated by
// ModelUniformData in rts/Rendering/Models/ModelsMemStorageDefs.h; its
// static_assert on sizeof == 128 is the only tripwire if that struct changes.
layout(binding = 14) uniform samplerBuffer modelUniforms;

SUniformsBuffer fetchUni(uint idx) {
    int b = int(idx) * 8;            // sizeof(ModelUniformData) == 128 == 8 texels

    vec4 t0 = texelFetch(modelUniforms, b + 0);
    vec4 t1 = texelFetch(modelUniforms, b + 1);

    SUniformsBuffer u;
    // t0 holds packed integers, and the texture is float-formatted
    u.composite = floatBitsToUint(t0.x);
    u.unused2   = floatBitsToUint(t0.y);
    u.unused3   = floatBitsToUint(t0.z);
    u.unused4   = floatBitsToUint(t0.w);

    u.maxHealth = t1.x;
    u.health    = t1.y;
    u.unused5   = t1.z;
    u.unused6   = t1.w;

    u.drawPos = texelFetch(modelUniforms, b + 2);
    u.speed   = texelFetch(modelUniforms, b + 3);

    u.userDefined[0] = texelFetch(modelUniforms, b + 4);
    u.userDefined[1] = texelFetch(modelUniforms, b + 5);
    u.userDefined[2] = texelFetch(modelUniforms, b + 6);
    u.userDefined[3] = texelFetch(modelUniforms, b + 7);

    return u;
}
