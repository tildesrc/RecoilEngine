/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <SDL_mouse.h>

#include "StartPosSelecter.h"
#include "MouseHandler.h"
#include "Game/Game.h"
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GameSetup.h"
#include "Game/GlobalUnsynced.h"
#include "Game/InMapDraw.h"
#include "Game/Players/Player.h"
#include "Map/Ground.h"
#include "Map/ReadMap.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/GlobalRenderingInfo.h"
#include "Rendering/GL/glExtra.h"
#include "Rendering/Fonts/glFont.h"
#include "Net/Protocol/NetProtocol.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/ModInfo.h"

#include "System/Misc/TracyDefs.h"


CStartPosSelecter* CStartPosSelecter::selector = nullptr;


CStartPosSelecter::CStartPosSelecter() : CInputReceiver(BACK)
{
	RECOIL_DETAILED_TRACY_ZONE;
	showReadyBox = true;
	startPosSet = false;

	readyBox.x1 = 0.71f;
	readyBox.y1 = 0.72f;
	readyBox.x2 = 0.81f;
	readyBox.y2 = 0.76f;

	selector = this;
}

CStartPosSelecter::~CStartPosSelecter()
{
	RECOIL_DETAILED_TRACY_ZONE;
	selector = nullptr;
}


bool CStartPosSelecter::Ready(bool luaForcedReady)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!gs->PreSimFrame()) {
		delete this;
		return true;
	}

	const CTeam* mt = teamHandler.Team(gu->myTeam);
	const float3* sp = nullptr;

	// player did not set a startpos yet, so do not let
	// him ready up if he clicked on the ready-box first
	//
	// do allow in the special case where player already
	// sent a RDYSTATE_UPDATED and then rejoined the game
	if (!mt->HasValidStartPos()) {
		if (!startPosSet && !luaForcedReady)
			return false;

		sp = &setStartPos;
	} else {
		sp = &mt->GetStartPos();
	}

	if (luaForcedReady) {
		clientNet->Send(CBaseNetProtocol::Get().SendStartPos(gu->myPlayerNum, gu->myTeam, CPlayer::PLAYER_RDYSTATE_FORCED, sp->x, sp->y, sp->z));
	} else {
		clientNet->Send(CBaseNetProtocol::Get().SendStartPos(gu->myPlayerNum, gu->myTeam, CPlayer::PLAYER_RDYSTATE_READIED, sp->x, sp->y, sp->z));
	}

	delete this;
	return true;
}


bool CStartPosSelecter::MousePress(int x, int y, int button)
{
	RECOIL_DETAILED_TRACY_ZONE;

	if (button != SDL_BUTTON_LEFT)
		return false;

	if (modInfo.useStartPositionSelecter == false) {
		return false;
	}

	// Not ready to process mouse clicks yet. Pass.
	if (!game->IsDoneLoading())
		return false;

	const float mx = MouseX(x);
	const float my = MouseY(y);

	if ((showReadyBox && InBox(mx, my, readyBox)) || !gs->PreSimFrame())
		return (!Ready(false));

	const float dist = CGround::LineGroundCol(camera->GetPos(), camera->GetPos() + mouse->dir * camera->GetFarPlaneDist() * 1.4f, false);

	if (dist < 0.0f)
		return true;

	inMapDrawer->SendErase(setStartPos);
	startPosSet = true;
	setStartPos = camera->GetPos() + mouse->dir * dist;
	clientNet->Send(CBaseNetProtocol::Get().SendStartPos(gu->myPlayerNum, gu->myTeam, CPlayer::PLAYER_RDYSTATE_UPDATED, setStartPos.x, setStartPos.y, setStartPos.z));

	return true;
}

void CStartPosSelecter::Draw()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (gu->spectating) {
		delete this;
		return;
	}


	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	auto& shader = rb.GetShader();

	glEnable(GL_BLEND);

	// Upstream dropped this because BAR draws a nicer start box itself - but
	// BAR's version is a GL4 widget, and where GL4 is unavailable nothing draws
	// it at all, leaving a player with no way to see where they may start. Fall
	// back to the engine's own box exactly then, so a GL4-capable machine keeps
	// the content's version and is untouched.
	if (!globalRendering->haveGL4)
		DrawStartBox();

	if (!showReadyBox)
		return;

	const float mx =                               float(mouse->lastx)  * globalRendering->pixelX;
	const float my = (globalRendering->viewSizeY - float(mouse->lasty)) * globalRendering->pixelY;

	{
		gleDrawQuadC(readyBox, InBox(mx, my, readyBox)? SColor{0.7f, 0.2f, 0.2f, guiAlpha}: SColor{0.7f, 0.7f, 0.2f, guiAlpha}, rb);

		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}

	{
		gleDrawQuadC(readyBox, InBox(mx, my, readyBox)? SColor{0.7f, 0.2f, 0.2f, guiAlpha}: SColor{0.7f, 0.7f, 0.2f, guiAlpha}, rb);

		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	{
		shader.Enable();

		// transformMatrix persists on the shader between call sites, and
		// DrawStartBox above leaves the camera's matrix in it; this box is in
		// 0..1 screen space and has to say so.
		if (globalRenderingInfo.glContextIsCore) {
			const CMatrix44f proj = CMatrix44f::ClipOrthoProj01(globalRendering->supportClipSpaceControl * 1.0f);
			shader.SetUniformMatrix4x4("transformMatrix", false, &proj.m[0]);
		}

		rb.DrawElements(GL_TRIANGLES);
		shader.Disable();
	}

	{
		// fit text into box
		const float ySize = readyBox.y2 - readyBox.y1;
		const float xSize = readyBox.x2 - readyBox.x1;

		const float yPos = 0.5f * (readyBox.y1 + readyBox.y2);
		const float xPos = 0.5f * (readyBox.x1 + readyBox.x2);

		const float unitWidth  = font->GetSize() * font->GetTextWidth("Ready") * globalRendering->pixelX;
		const float unitHeight = font->GetSize() * font->GetLineHeight(      ) * globalRendering->pixelY;

		const float fontScale = 0.9f * std::min(xSize / unitWidth, ySize / unitHeight);

		font->SetColors(); // default
		font->glPrint(xPos, yPos, fontScale, FONT_OUTLINE | FONT_CENTER | FONT_VCENTER | FONT_SCALE | FONT_NORM | FONT_BUFFERED, "Ready");
		font->DrawBuffered();
	}
}


// Green wall around the local ally team's permitted starting area. Ported from
// the pre-RenderBuffer version that upstream disabled; the geometry is
// unchanged, only the buffer API and the explicit world transform are new.
void CStartPosSelecter::DrawStartBox() const
{
	RECOIL_DETAILED_TRACY_ZONE;

	const AllyTeam& myStartData = teamHandler.GetAllyTeam(gu->myAllyTeam);

	const float bx = myStartData.startRectLeft * mapDims.mapx * SQUARE_SIZE;
	const float by = myStartData.startRectTop  * mapDims.mapy * SQUARE_SIZE;

	const float dx = (myStartData.startRectRight  - myStartData.startRectLeft) * mapDims.mapx * SQUARE_SIZE / 10.0f;
	const float dy = (myStartData.startRectBottom - myStartData.startRectTop ) * mapDims.mapy * SQUARE_SIZE / 10.0f;

	// a degenerate rect means the whole map is allowed, so there is no edge to
	// draw and a zero-area wall would just z-fight with the ground
	if (dx <= 0.0f || dy <= 0.0f)
		return;

	static constexpr SColor boxColor = {0.2f, 0.8f, 0.2f, 0.5f};
	static constexpr float wallHeight = 100.0f;

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	auto& sh = rb.GetShader();

	const auto addWall = [&rb](float3 p1, float3 p2) {
		p1.y = CGround::GetHeightAboveWater(p1.x, p1.z, false);
		p2.y = CGround::GetHeightAboveWater(p2.x, p2.z, false);

		rb.AddQuadTriangles(
			{p1 + UpVector * wallHeight, boxColor},
			{p2 + UpVector * wallHeight, boxColor},
			{p2                        , boxColor},
			{p1                        , boxColor}
		);
	};

	// walk each edge in ten steps so the wall follows the terrain under it
	for (int a = 0; a < 10; ++a) {
		addWall({bx + (a    ) * dx, 0.0f, by               }, {bx + (a + 1) * dx, 0.0f, by               });
		addWall({bx + (a    ) * dx, 0.0f, by + dy * 10.0f  }, {bx + (a + 1) * dx, 0.0f, by + dy * 10.0f  });
		addWall({bx               , 0.0f, by + dy * (a    )}, {bx               , 0.0f, by + dy * (a + 1)});
		addWall({bx + dx * 10.0f  , 0.0f, by + dy * (a    )}, {bx + dx * 10.0f  , 0.0f, by + dy * (a + 1)});
	}

	glEnable(GL_DEPTH_TEST);
	sh.Enable();

	if (globalRenderingInfo.glContextIsCore) {
		if (const CCamera* cam = CCameraHandler::GetActiveCamera(); cam != nullptr)
			sh.SetUniformMatrix4x4("transformMatrix", false, &cam->GetViewProjectionMatrix().m[0]);
	}

	rb.DrawElements(GL_TRIANGLES);
	sh.Disable();
}
