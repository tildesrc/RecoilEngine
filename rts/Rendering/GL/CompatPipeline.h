/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef GL_COMPAT_PIPELINE_H
#define GL_COMPAT_PIPELINE_H

#include "System/Matrix44f.h"

// Software stand-ins for the parts of the fixed-function pipeline a core
// profile removed. Every entry point here passes straight through to the real
// GL call on a compatibility profile, so converting a call site changes
// nothing there; on a core profile it keeps the state in software so that the
// shaders which replaced the pipeline can be handed the same matrices.
//
// The matrix stack is the keystone: without it the immediate-mode emulation
// below has no transform to draw with, and neither does any call site that
// used to set one up with glMatrixMode/glOrtho.
namespace GLCompat {
	// --- matrix stack (glMatrixMode/glPushMatrix/glOrtho/...) ---
	void MatrixMode(unsigned int mode);
	void LoadIdentity();
	void PushMatrix();
	void PopMatrix();
	void LoadMatrixf(const float* m);
	void MultMatrixf(const float* m);
	void Translatef(float x, float y, float z);
	void Scalef(float x, float y, float z);
	void Rotatef(float angleDeg, float x, float y, float z);
	void Ortho(float l, float r, float b, float t, float n, float f);

	const CMatrix44f& GetModelView();
	const CMatrix44f& GetProjection();
	// what gl_ModelViewProjectionMatrix would have held
	CMatrix44f GetMVP();

	// Reset both stacks to identity. Called once per frame so that a call site
	// which pushed without popping cannot corrupt every later frame.
	void ResetMatrixState();

	// --- immediate mode (glBegin/glVertex/...) ---
	void Begin(unsigned int primMode);
	void End();
	bool InBegin();

	void Vertex(float x, float y, float z);
	void Color(float r, float g, float b, float a);
	void TexCoord(float s, float t);

	// spelled like the calls they replace, so converting a site is mechanical
	inline void Vertex2f(float x, float y)                   { Vertex(x, y, 0.0f); }
	inline void Vertex3f(float x, float y, float z)          { Vertex(x, y, z); }
	inline void Vertex3fv(const float* v)                    { Vertex(v[0], v[1], v[2]); }
	inline void Color3f(float r, float g, float b)           { Color(r, g, b, 1.0f); }
	inline void Color4f(float r, float g, float b, float a)  { Color(r, g, b, a); }
	inline void Color3fv(const float* c)                     { Color(c[0], c[1], c[2], 1.0f); }
	inline void Color4fv(const float* c)                     { Color(c[0], c[1], c[2], c[3]); }
	inline void TexCoord2f(float s, float t)                 { TexCoord(s, t); }
	inline void TexCoord2fv(const float* v)                  { TexCoord(v[0], v[1]); }
}

#endif // GL_COMPAT_PIPELINE_H
