// Companion to model-uniforms-texbuf.glsl for BAR's transform SSBO. The engine
// publishes its transform storage as a GL_TEXTURE_BUFFER on reserved texture
// unit 13.
//
// NOTE the element type. The storage is NOT an array of mat4: it is an array of
// the 32-byte quaternion Transform (quat, trSc), so each element is exactly two
// RGBA32F texels. Several BAR shaders still carry a legacy `mat4 UnitPieces[]`
// declaration behind `#if USEQUATERNIONS == 0`; that branch is dead - BAR builds
// with quaternions and takes //__QUATERNIONDEFS__ instead. Converting the mat4
// branch is wasted work, and reading 4 texels per element would be wrong.
//
// This conversion is ALREADY DONE centrally, in
//   devtools/macos-port/bar-overrides/luashader-quaterniondefs.patch
// which patches BAR's own modules/graphics/luashader.lua, the file that emits
// //__QUATERNIONDEFS__. So any widget going through the quaternion path gets it
// for free and needs no shader edit of its own. This snippet is here for a
// shader that declares its own transform buffer outside that mechanism.
//
// The unit MUST be a literal - see the note in model-uniforms-texbuf.glsl.
layout(binding = 13) uniform samplerBuffer modelTransforms;

struct Transform {
	vec4 quat;
	vec4 trSc;
};

Transform fetchTransform(uint idx) {
	int b = int(idx) * 2;            // sizeof(Transform) == 32 == 2 texels
	return Transform(
		texelFetch(modelTransforms, b + 0),
		texelFetch(modelTransforms, b + 1)
	);
}
