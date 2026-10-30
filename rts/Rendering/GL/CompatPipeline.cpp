/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "CompatPipeline.h"

#include <array>
#include <vector>

#include "Rendering/GL/myGL.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/GlobalRenderingInfo.h"
#include "Rendering/Shaders/Shader.h"
#include "System/Log/ILog.h"
#include "System/Misc/TracyDefs.h"

namespace {
	bool Emulated() { return globalRenderingInfo.glContextIsCore; }

	struct MatrixStack {
		std::vector<CMatrix44f> s = { CMatrix44f() };

		CMatrix44f& Top() { return s.back(); }

		void Push() {
			// a runaway push is a call-site bug, but dropping it quietly beats
			// growing without bound
			if (s.size() < 64)
				s.push_back(s.back());
			else
				LOG_L(L_ERROR, "[GLCompat] matrix stack overflow");
		}
		void Pop() {
			if (s.size() > 1)
				s.pop_back();
			else
				LOG_L(L_ERROR, "[GLCompat] matrix stack underflow");
		}
		void Reset() {
			s.clear();
			s.emplace_back();
		}
	};

	MatrixStack modelViewStack;
	MatrixStack projectionStack;
	unsigned int matrixMode = GL_MODELVIEW;

	MatrixStack& CurStack() {
		return (matrixMode == GL_PROJECTION) ? projectionStack : modelViewStack;
	}

	// immediate-mode accumulation
	bool          immActive = false;
	unsigned int  immMode   = 0;
	SColor        immColor  = SColor{1.0f, 1.0f, 1.0f, 1.0f};
	float         immS = 0.0f, immT = 0.0f;
	std::vector<VA_TYPE_TC> immVerts;
}

namespace GLCompat {

void MatrixMode(unsigned int mode)
{
	if (!Emulated()) { glMatrixMode(mode); return; }
	matrixMode = mode;
}

void LoadIdentity()
{
	if (!Emulated()) { glLoadIdentity(); return; }
	CurStack().Top() = CMatrix44f();
}

void PushMatrix()
{
	if (!Emulated()) { glPushMatrix(); return; }
	CurStack().Push();
}

void PopMatrix()
{
	if (!Emulated()) { glPopMatrix(); return; }
	CurStack().Pop();
}

void LoadMatrixf(const float* m)
{
	if (!Emulated()) { glLoadMatrixf(m); return; }
	CurStack().Top() = CMatrix44f(m);
}

void MultMatrixf(const float* m)
{
	if (!Emulated()) { glMultMatrixf(m); return; }
	CurStack().Top() = CurStack().Top() * CMatrix44f(m);
}

void Translatef(float x, float y, float z)
{
	if (!Emulated()) { glTranslatef(x, y, z); return; }
	CurStack().Top() = CurStack().Top() * CMatrix44f(float3(x, y, z));
}

void Scalef(float x, float y, float z)
{
	if (!Emulated()) { glScalef(x, y, z); return; }

	CMatrix44f m;
	m.Scale(float3(x, y, z));
	CurStack().Top() = CurStack().Top() * m;
}

void Rotatef(float angleDeg, float x, float y, float z)
{
	if (!Emulated()) { glRotatef(angleDeg, x, y, z); return; }

	CMatrix44f m;
	m.Rotate(-angleDeg * math::DEG_TO_RAD, float3(x, y, z).ANormalize());
	CurStack().Top() = CurStack().Top() * m;
}

void Ortho(float l, float r, float b, float t, float n, float f)
{
	if (!Emulated()) { glOrtho(l, r, b, t, n, f); return; }
	CurStack().Top() = CurStack().Top() * CMatrix44f::ClipOrthoProj(l, r, b, t, n, f, globalRenderingInfo.glContextIsCore * 1.0f);
}

const CMatrix44f& GetModelView()  { return modelViewStack.Top(); }
const CMatrix44f& GetProjection() { return projectionStack.Top(); }
CMatrix44f GetMVP() { return projectionStack.Top() * modelViewStack.Top(); }

void ResetMatrixState()
{
	modelViewStack.Reset();
	projectionStack.Reset();
	matrixMode = GL_MODELVIEW;
}

/******************************************************************************/

void Begin(unsigned int primMode)
{
	if (!Emulated()) { glBegin(primMode); return; }

	immActive = true;
	immMode   = primMode;
	immVerts.clear();
}

bool InBegin() { return immActive; }

void Vertex(float x, float y, float z)
{
	if (!immActive) { glVertex3f(x, y, z); return; }
	immVerts.emplace_back(VA_TYPE_TC{{x, y, z}, immS, immT, immColor});
}

void Color(float r, float g, float b, float a)
{
	if (!immActive) { glColor4f(r, g, b, a); return; }
	immColor = SColor{r, g, b, a};
}

void TexCoord(float s, float t)
{
	if (!immActive) { glTexCoord2f(s, t); return; }
	immS = s; immT = t;
}

void End()
{
	if (!Emulated()) { glEnd(); return; }

	immActive = false;

	if (immVerts.empty())
		return;

	// GL_QUADS, GL_QUAD_STRIP and GL_POLYGON went with the fixed pipeline
	unsigned int mode = immMode;
	std::vector<VA_TYPE_TC> out;

	switch (immMode) {
		case GL_QUADS: {
			out.reserve((immVerts.size() / 4) * 6);
			for (size_t i = 0; (i + 3) < immVerts.size(); i += 4) {
				out.push_back(immVerts[i + 0]); out.push_back(immVerts[i + 1]); out.push_back(immVerts[i + 2]);
				out.push_back(immVerts[i + 0]); out.push_back(immVerts[i + 2]); out.push_back(immVerts[i + 3]);
			}
			mode = GL_TRIANGLES;
		} break;
		case GL_QUAD_STRIP: {
			for (size_t i = 0; (i + 3) < immVerts.size(); i += 2) {
				out.push_back(immVerts[i + 0]); out.push_back(immVerts[i + 1]); out.push_back(immVerts[i + 2]);
				out.push_back(immVerts[i + 2]); out.push_back(immVerts[i + 1]); out.push_back(immVerts[i + 3]);
			}
			mode = GL_TRIANGLES;
		} break;
		case GL_POLYGON: {
			out  = immVerts;
			mode = GL_TRIANGLE_FAN;
		} break;
		default: {
			out = immVerts;
		} break;
	}

	if (out.empty())
		return;

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_TC>();
	for (const VA_TYPE_TC& v: out)
		rb.AddVertex(VA_TYPE_TC{v});

	GLint boundProg = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &boundProg);

	if (boundProg != 0) {
		// the call site bound its own shader, so it owns the transform too
		rb.Submit(mode);
		return;
	}

	Shader::IProgramObject& sh = rb.GetShader();
	const CMatrix44f mvp = GetMVP();

	sh.Enable();
	sh.SetUniformMatrix4x4("transformMatrix", false, &mvp.m[0]);
	rb.Submit(mode);
	sh.Disable();
}

} // namespace GLCompat
