/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef LUA_DISPLAY_LIST_RECORD_H
#define LUA_DISPLAY_LIST_RECORD_H

#include <memory>
#include <vector>

#include "Rendering/GL/VertexArrayTypes.h"
#include "System/float4.h"
#include "System/Matrix44f.h"

// Geometry captured from a Lua display list.
//
// A core profile has no display lists. Re-running the list's function on every
// call looks like the obvious substitute, but it is not what GL does: GL runs
// the function ONCE, when the list is built, and callers depend on that. BAR's
// FlowUI sets a module-level record, calls gl.CreateList, and clears the record
// on the very next line - so a deferred run always sees nil and fails on every
// frame. Run the function at creation instead, while the caller's state is
// still live, and keep what it drew to replay on each call.
//
// Text is NOT recorded. Replaying a print means driving the shared font object's
// transform state, which corrupts the engine's own buffered text (every string
// in the game, the info console included, comes out mirrored). Until that is
// solved, a list that printed anything is thrown away and handled the old way,
// so those lists are no worse than before.
struct LuaDListOp {
	unsigned int mode = 0;

	// The texture bound when this was captured, and whether the LIST bound it.
	// GL does not compile a texture binding made outside a display list into
	// it, so a list whose caller binds the texture - FlowUI's cached unit cells
	// do exactly that - must sample whatever is current at CALL time. Forcing
	// the recorded texture made a hovered build-menu cell draw with some other
	// unit's picture, or none.
	unsigned int texture = 0;
	bool ownTexture = false;

	float4 alphaCtrl;

	// Blend state at capture time. GL compiles glBlendFunc INTO a display list,
	// so a list that changes blending mid-way expects that to replay with it -
	// FlowUI's unit cells switch to additive before drawing a highlight band
	// across the top 40% of the cell. Replaying every op under the caller's
	// blend mode instead made that band double-bright whenever the caller was
	// itself drawing additively, which is what a hovered build-menu cell does.
	bool blendEnabled = true;
	unsigned int blendSrcRGB = 0, blendDstRGB = 0;
	unsigned int blendSrcA   = 0, blendDstA   = 0;

	// NB: no transform is stored. GL applies the matrix current when a list is
	// CALLED, not when it was built, so replay uses the caller's - see
	// LuaOpenGL::CallList. An earlier version baked the record-time transform
	// here, which drew FlowUI's cached cells at a tenth size in a corner,
	// because they are built inside a render-to-texture pass.

	std::vector<VA_TYPE_TC> verts;
};

using LuaDListOps = std::vector<LuaDListOp>;
using LuaDListOpsPtr = std::shared_ptr<LuaDListOps>;

// Active only while gl.CreateList is running the list's function. Drawing done
// in that window is captured rather than submitted.
namespace LuaDListRecorder {
	inline LuaDListOps* target = nullptr;
	inline bool sawText = false;
	inline bool sawMatrix = false;
	inline bool sawTexture = false;

	inline bool Recording() { return target != nullptr; }

	inline void Begin(LuaDListOps* ops) {
		target = ops;
		sawText = false;
		sawMatrix = false;
		sawTexture = false;
	}

	inline void End() { target = nullptr; }

	// a print was suppressed during the capture, so the recording is unusable
	inline void NoteText() { sawText = true; }
	inline bool SawText() { return sawText; }

	// a gl.Texture call inside the recording, so the list owns its binding
	inline void NoteTexture() { sawTexture = true; }
	inline bool SawTexture() { return sawTexture; }

	// A list is also a legitimate way to carry matrix state: BA's UI framework
	// installs its whole projection in one. Replaying only the geometry throws
	// those calls away, and everything drawn afterwards silently uses the wrong
	// transform - so such a list has to be re-run instead.
	inline void NoteMatrix() { sawMatrix = true; }
	inline bool SawMatrix() { return sawMatrix; }

	inline void Add(LuaDListOp&& op) {
		if (target != nullptr)
			target->emplace_back(std::move(op));
	}
}

#endif /* LUA_DISPLAY_LIST_RECORD_H */
