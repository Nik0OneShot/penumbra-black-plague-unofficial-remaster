// PenumbraUltrawide - widescreen/ultrawide fix for Penumbra: Black Plague (Steam build 459397).
//
// Ships as a stand-in SDL_image.dll that forwards every export to the real library
// (renamed SDL_image_orig.dll). At the game's first SDL_Init call, after the Steam stub has
// decrypted the executable, it patches the game in memory:
//
//   * Menus, HUD, text and the inventory keep their 4:3 shape, centred (pillarboxed),
//     instead of being stretched across the screen.
//   * Full-screen overlays (fades, flashes, damage, noise, letterbox bars) still fill
//     the whole screen.
//   * The in-world cursor stays on the object it points at.
//   * Horizontal mouse speed matches vertical again (it was ~1.8x slower at 21:9).
//   * The options menu offers modern resolutions, including the desktop resolution.
//   * Game logic runs at the monitor's refresh rate (HighFramerate.cpp).
//
// Every patch site is checked against the expected original bytes first; if anything
// differs (another game version, or Requiem.exe, which shares this folder), nothing is
// patched and the reason is logged.

#include "Patch.h"
#include <intrin.h>
#include <GL/gl.h>
#include <math.h>
#include <share.h>
#include <stdio.h>
#include <stdarg.h>

#pragma intrinsic(_ReturnAddress)

//////////////////////////////////////////////////////////////////////////
// Game addresses (Penumbra.exe, image base 0x400000, no ASLR)

namespace A {
	// cLowLevelGraphicsSDL: mvScreenSize (int x,y) at +4, mvVirtualSize (float x,y) at +0xC
	const DWORD SetOrtho        = 0x0055D8A0;	// vtable index 31
	const DWORD SetOrthoSlot    = 0x006883D4;
	const DWORD RetDrawAll      = 0x004F3964;	// cGraphicsDrawer::DrawAll
	const DWORD RetDrawTexture  = 0x0045CC2C;	// cGraphicsHelper::DrawTexture
	const DWORD RetIntroDraw    = 0x00469446;	// cIntroStory::OnDraw
	const DWORD RetGuiSetRender = 0x004FA150;	// cGuiSet::Render, 2D branch (computer terminals)
	// cGuiSet::Render then draws its clip regions (both 2D and 3D sets); SetClipArea turns each
	// region into a scissor box, stretching the virtual width over the whole screen
	const DWORD GuiRenderClipCall = 0x004FA152;	// call RenderClipRegions (thiscall, no args)
	const DWORD GuiRenderClip     = 0x004F9A80;
	const DWORD GuiScissorCall    = 0x004F8DAF;	// call [eax+0D8h] = SetScissorRect(const cRect2l&)
	const DWORD ScissorSlot       = 0xD8;
	const DWORD GfxVtable         = 0x00688358;	// cLowLevelGraphicsSDL
	const DWORD SetScissorRect    = 0x0055DE20;

	// cGui::SendMouseMove(absPos, relPos), fed from cButtonHandler with the mouse's absolute
	// position (stretched screen space) while a terminal is open
	const DWORD GuiMouseMoveCall = 0x0040476B;
	const DWORD GuiSendMouseMove = 0x004E3190;

	// cScene::Render calls the updaters' OnPostSceneDraw (where terminals are drawn) from two
	// branches: after drawing the 3D world, or instead of it (fullscreen terminals switch it off)
	const DWORD PostSceneDraw         = 0x004E1C40;
	const DWORD PostSceneDrawAfter3D  = 0x004EE01B;
	const DWORD PostSceneDrawNo3D     = 0x004EE033;

	// cMouseSDL::Update: mvMouseRelPos at +0x2C, mpLowLevelGraphics at +0x64
	const DWORD MouseUpdate     = 0x00597730;
	const DWORD MouseUpdateSlot = 0x0069289C;

	// cGraphicsDrawer (mpLowLevelGraphics at +0)
	const DWORD DrawLong        = 0x004F4710;	// DrawGfxObject(obj, pos, size, col, flipH, flipV, angle)
	const DWORD DrawShort       = 0x004F47E0;	// DrawGfxObject(obj, pos)
	const DWORD DrawTexture     = 0x0045CBE0;	// cGraphicsHelper::DrawTexture(tex, pos, size, col, +4 more)
	const DWORD DrawTextureRet  = 0x0045CFF3;	// its epilogue: add esp, 64h; ret 20h

	// Call sites in cPlayer::OnDraw (esi = cPlayer)
	const DWORD CursorShortCall    = 0x004A02B7;	// crosshair
	const DWORD CrossHairStateRead = 0x004A0027;	// mov eax, [esi+2E4h] in the same function
	const DWORD CrossHairDrawerLoad = 0x004A02B1;	// mov ecx, [esi+80h] right before the call
	const DWORD PlayerCrossHairState = 0x2E4;	// cPlayer::mCrossHairState
	const int   CrossHairPointer   = 5;	// eCrossHairState_Pointer: menus reuse the crosshair as their mouse pointer
	const DWORD CursorLongCalls[]  = { 0x004A0101, 0x004A0172, 0x004A01B9 };	// held-item cursor
	const DWORD NoiseTileCall      = 0x004A86F1;	// cPlayerNoiseFilter::Draw (20x15 tiles of 40x40)
	const DWORD LetterboxCalls[]   = { 0x004694DD, 0x00469558 };	// intro black bars
	// Double-vision effect: copies the screen into a texture (CopyContextToTexure, vtable +174h)
	// and draws it back over the full 800x600 canvas, slightly enlarged and pulsing
	const DWORD ScreenCopyDrawCall = 0x0040A8B6;
	const DWORD ScreenCopyGrab     = 0x0040A813;	// call [edx+174h] in the same function

	// fmul dword ptr [800.0f] feeding AddCrossHairPos(afVal * 800, 0)
	const DWORD CursorMul800[]  = { 0x004AA30B, 0x004ADBC7, 0x004AE108 };
	const DWORD Const800        = 0x00672F70;

	// gvResolutions[17] (cVector2l) used by the options menu
	const DWORD Resolutions     = 0x006CF030;
	const int   ResolutionNum   = 17;

	// cFrustum::SetViewProjMatrix: mOriginBV.SetSize(near*2), the box around the camera
	const DWORD OriginBoxSize   = 0x0050E26D;	// fld [esp+78h] (near); mov edx, [esp+74h]; fadd st0, st0
	// cRenderer3D::UpdateRenderList: fog-culling box = fogEnd*2 / cos(fov*aspect*0.5)
	const DWORD FogCornerCos    = 0x0052A953;	// fmul [0.5]; fcos   (edi = cCamera3D: mfFOV +10h, mfAspect +14h)

	// cSectorVisibilityContainer::SearchSector: is this doorway (portal) in view?
	const DWORD PortalTestCall  = 0x00555480;	// call IntersectionBV(&portal->mBV, parentSet); ebp = cPortal
	const DWORD IntersectionBV  = 0x00554920;
	const DWORD IntersectionFrustumTest = 0x005549A5;	// add ecx, 26Ch; call cFrustum::CollideBoundingVolume
	const DWORD FrustumCollideBV = 0x0050DE10;
	const DWORD ContainerFrustum = 0x26C;	// cSectorVisibilityContainer::mFrustum
	const DWORD FrustumPlanes    = 0x2D0;	// cPlanef[6] (a, b, c, d, normal): left, right, bottom, top, near, far
	const DWORD FrustumSphereRadius = 0x378;
	const DWORD PortalBV         = 0x90;	// cPortal::mBV; cPortal::mlId at +4

	// OALWrapper effect slot constructor: cmp byte [owner+18h], 0 (threaded?); ...; je past
	// "call SDL_CreateMutex; mov [esi+20h], eax"
	const DWORD EfxSlotMutexIf   = 0x005A9707;
	const DWORD SdlCreateMutex   = 0x00623C34;	// import thunk
}

//////////////////////////////////////////////////////////////////////////
// Logging and config

char g_dir[MAX_PATH];
static FILE* g_log;

void Log(const char* fmt, ...)
{
	if(!g_log) return;
	va_list va; va_start(va, fmt);
	vfprintf(g_log, fmt, va);
	va_end(va);
	fputc('\n', g_log);
	fflush(g_log);
}

int CfgInt(const char* key, int def)
{
	char path[MAX_PATH];
	_snprintf_s(path, sizeof(path), _TRUNCATE, "%sPenumbraUltrawide.ini", g_dir);
	return GetPrivateProfileIntA("Fix", key, def, path);
}

//////////////////////////////////////////////////////////////////////////
// Aspect helpers

// k: how much wider the screen is than 4:3 (2.389/1.333 = 1.79 at 21:9).
// off: virtual units added on each side when the 800x600 layout is centred.
struct Fit { float k, off; };

static bool GetFit(const u8* gfx, Fit& f)
{
	if(!gfx) return false;
	int W = *(const int*)(gfx + 4), H = *(const int*)(gfx + 8);
	float vx = *(const float*)(gfx + 0xC), vy = *(const float*)(gfx + 0x10);
	if(W <= 0 || H <= 0 || vx <= 0 || vy <= 0) return false;
	float vw = vy * (float)W / (float)H;
	if(vw <= vx + 0.5f) return false;	// 4:3 or narrower: leave as is
	f.k = vw / vx;
	f.off = (vw - vx) * 0.5f;
	return true;
}

static float VirtualWidth(const u8* gfx) { return *(const float*)(gfx + 0xC); }

//////////////////////////////////////////////////////////////////////////
// Hooks

typedef void (__fastcall *SetOrtho_t)(void*, void*, const float*, float, float);
typedef void (__fastcall *MouseUpdate_t)(void*, void*);
typedef void (__fastcall *DrawLong_t)(void*, void*, void*, const float*, const float*, const float*, int, int, float);
typedef void (__fastcall *DrawShort_t)(void*, void*, void*, const float*);
// Black Plague's DrawTexture takes four more arguments than Overture's (8 in all, ret 20h);
// they are passed through untouched.
typedef void (__fastcall *DrawTexture_t)(void*, void*, void*, const float*, const float*, const float*, DWORD, DWORD, DWORD, DWORD);

static DrawLong_t g_drawLong = (DrawLong_t)A::DrawLong;	// becomes the trampoline once detoured
static float g_cursorMulX = 800.0f;	// replaces the 800.0f in AddCrossHairPos(afVal * 800, 0)

static u8* g_lowGfx;	// cLowLevelGraphicsSDL, noted on every 2D pass

// Whether this frame drew the 3D world. Fullscreen terminals switch it off, and then nothing
// clears the screen: with the terminal centred, the side bars would show stale frames.
static bool g_world3DDrawn = true;
typedef void (__fastcall *PostSceneDraw_t)(void*, void*);
static void __fastcall Call_PostSceneDrawAfter3D(void* updater, void* edx)
{
	g_world3DDrawn = true;
	((PostSceneDraw_t)A::PostSceneDraw)(updater, edx);
}
static void __fastcall Call_PostSceneDrawNo3D(void* updater, void* edx)
{
	g_world3DDrawn = false;
	((PostSceneDraw_t)A::PostSceneDraw)(updater, edx);
}

// Clear the strips left and right of the centred layout to black, leaving GL state as it was.
static void ClearSideBars(int W, int H, float off, float width)
{
	int bar = (int)ceilf(off / (width + 2.0f * off) * (float)W);
	if(bar <= 0 || 2 * bar >= W) return;
	GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
	GLint box[4]; glGetIntegerv(GL_SCISSOR_BOX, box);
	GLfloat clear[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
	GLboolean mask[4]; glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0, 0, 0, 1);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, bar, H);
	glClear(GL_COLOR_BUFFER_BIT);
	glScissor(W - bar, 0, bar, H);
	glClear(GL_COLOR_BUFFER_BIT);
	glScissor(box[0], box[1], box[2], box[3]);
	if(!scissorOn) glDisable(GL_SCISSOR_TEST);
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
}

// Terminal clip regions (list boxes, scrolling text, windows) become scissor boxes in screen
// pixels, computed as if the layout were stretched over the whole width. While a GUI set's 2D
// render is centred (from its ortho call until its clip regions are drawn), map those boxes
// into the centred area. 3D GUI sets never get the centred ortho, so they are left alone.
struct GuiClip { bool on; float off, span; };	// virtual units: centring offset, visible width
static GuiClip g_guiClip;

typedef void (__fastcall *SetScissor_t)(void*, void*, const int*);
static void __fastcall Call_GuiScissor(void* gfx, void* edx, const int* rect)
{
	SetScissor_t fn = *(SetScissor_t*)(*(u8**)gfx + A::ScissorSlot);
	const u8* g = (const u8*)gfx;
	const float W = (float)*(const int*)(g + 4), vx = VirtualWidth(g);
	if(g_guiClip.on && g_guiClip.span > 0 && W > 0 && vx > 0)
	{
		float l = (rect[0] * vx / W + g_guiClip.off) / g_guiClip.span * W;
		float r = ((rect[0] + rect[2]) * vx / W + g_guiClip.off) / g_guiClip.span * W;
		int box[4] = { (int)floorf(l), rect[1], 0, rect[3] };
		box[2] = (int)ceilf(r) - box[0];
		fn(gfx, edx, box);
		return;
	}
	fn(gfx, edx, rect);
}

typedef void (__fastcall *GuiRenderClip_t)(void*, void*);
static void __fastcall Call_GuiRenderClip(void* set, void* edx)
{
	((GuiRenderClip_t)A::GuiRenderClip)(set, edx);
	g_guiClip.on = false;
}

// 2D projection: centre the 800x600 layout for the UI passes (HUD/menus, textures, intro,
// terminals), leave post effects alone. The centring is computed from the size the caller
// asks for, so a GUI set with its own canvas size is centred correctly too.
static void __fastcall Hook_SetOrtho(void* gfx, void* edx, const float* size, float afMin, float afMax)
{
	g_lowGfx = (u8*)gfx;
	DWORD ret = (DWORD)_ReturnAddress();
	if(ret == A::RetGuiSetRender) g_guiClip.on = false;
	Fit f;
	if((ret == A::RetDrawAll || ret == A::RetDrawTexture || ret == A::RetIntroDraw || ret == A::RetGuiSetRender)
	   && GetFit((u8*)gfx, f) && size[1] > 0)
	{
		const int W = *(const int*)((u8*)gfx + 4), H = *(const int*)((u8*)gfx + 8);
		float off = (size[1] * (float)W / (float)H - size[0]) * 0.5f;
		if(off > 0.25f)
		{
			if(ret == A::RetGuiSetRender)
			{
				if(!g_world3DDrawn) ClearSideBars(W, H, off, size[0]);
				g_guiClip.off = off;
				g_guiClip.span = size[0] + 2 * off;
				g_guiClip.on = true;
			}
			glMatrixMode(GL_PROJECTION);
			glLoadIdentity();
			glOrtho(-off, size[0] + off, size[1], 0, afMin, afMax);
			return;
		}
	}
	((SetOrtho_t)A::SetOrtho)(gfx, edx, size, afMin, afMax);
}

// Terminals place their pointer at the mouse's absolute position, which spans the whole screen
// width (stretched space). Move it into the centred layout so it moves at the same speed both
// ways, and confine it to the 4:3 panel like a cursor at a screen edge: travel past an edge is
// dropped (kept as an offset from the hidden system cursor), so the pointer stops there and
// moves back as soon as the mouse does, with no dead zone. The offset never exceeds the width
// of a side bar, so both edges stay reachable.
static float g_guiPointerBias;
typedef bool (__fastcall *GuiMouseMove_t)(void*, void*, const float*, const float*);
static bool __fastcall Call_GuiMouseMove(void* gui, void* edx, const float* pos, const float* rel)
{
	Fit f;
	if(g_lowGfx && GetFit(g_lowGfx, f))
	{
		float vx = VirtualWidth(g_lowGfx);
		float p[2] = { pos[0] * f.k - f.off + g_guiPointerBias, pos[1] };
		if(p[0] < 0) { g_guiPointerBias -= p[0]; p[0] = 0; }
		else if(p[0] > vx) { g_guiPointerBias -= p[0] - vx; p[0] = vx; }
		return ((GuiMouseMove_t)A::GuiSendMouseMove)(gui, edx, p, rel);
	}
	g_guiPointerBias = 0;
	return ((GuiMouseMove_t)A::GuiSendMouseMove)(gui, edx, pos, rel);
}

// Relative mouse motion is scaled by virtual/screen size per axis, so at 21:9 horizontal
// movement came out ~1.8x slower than vertical. Make it match the 4:3 behaviour.
static void __fastcall Hook_MouseUpdate(void* mouse, void* edx)
{
	((MouseUpdate_t)A::MouseUpdate)(mouse, edx);

	u8* m = (u8*)mouse;
	Fit f;
	if(GetFit(*(u8**)(m + 0x64), f))
	{
		*(float*)(m + 0x2C) *= f.k;
		// The in-world cursor lives in stretched screen space, so undo the boost there.
		g_cursorMulX = 800.0f / f.k;
	}
	else
	{
		g_cursorMulX = 800.0f;
	}
}

// Anything covering the full layout width is an overlay: stretch it to the real screen edges.
static void __fastcall Hook_DrawLong(void* drawer, void* edx, void* obj, const float* pos, const float* size,
									 const float* col, int flipH, int flipV, float angle)
{
	const u8* gfx = *(u8**)drawer;
	Fit f;
	if(GetFit(gfx, f) && pos[0] <= 0.5f && pos[0] + size[0] >= VirtualWidth(gfx) - 0.5f)
	{
		float p[3] = { pos[0] * f.k - f.off, pos[1], pos[2] };
		float s[2] = { size[0] * f.k, size[1] };
		g_drawLong(drawer, edx, obj, p, s, col, flipH, flipV, angle);
		return;
	}
	g_drawLong(drawer, edx, obj, pos, size, col, flipH, flipV, angle);
}

// Tiles that make up a full-screen effect (sanity noise): stretch each tile.
static void __fastcall Call_DrawLongStretch(void* drawer, void* edx, void* obj, const float* pos, const float* size,
											const float* col, int flipH, int flipV, float angle)
{
	Fit f;
	if(GetFit(*(u8**)drawer, f))
	{
		float p[3] = { pos[0] * f.k - f.off, pos[1], pos[2] };
		float s[2] = { size[0] * f.k, size[1] };
		g_drawLong(drawer, edx, obj, p, s, col, flipH, flipV, angle);
		return;
	}
	g_drawLong(drawer, edx, obj, pos, size, col, flipH, flipV, angle);
}

// The in-world cursor position is in stretched screen space (it must match 3D picking),
// so move its centre into the centred layout space without changing its size.
static inline float CursorX(float x, float half, const Fit& f) { return (x + half) * f.k - f.off - half; }

static void __fastcall Call_DrawLongCursor(void* drawer, void* edx, void* obj, const float* pos, const float* size,
										   const float* col, int flipH, int flipV, float angle)
{
	Fit f;
	if(GetFit(*(u8**)drawer, f))
	{
		float p[3] = { CursorX(pos[0], size[0] * 0.5f, f), pos[1], pos[2] };
		g_drawLong(drawer, edx, obj, p, size, col, flipH, flipV, angle);
		return;
	}
	g_drawLong(drawer, edx, obj, pos, size, col, flipH, flipV, angle);
}

static float ImageHalfWidth(void* obj)
{
	__try
	{
		// cGfxObject::mpMat -> image[diffuse] -> size.x (same path the game uses)
		u8* mat = *(u8**)((u8*)obj + 0x10);
		u8* img = **(u8***)(mat + 0xB0);
		return (float)*(int*)(img + 0x6C) * 0.5f;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

// Inventory, notebook, numeric keypads and the death menu set the crosshair to their own mouse
// position, which is already in layout space, and show it as the pointer icon. Nothing sets that
// icon in the world, so it tells the two cases apart. (The "no crosshair" centre dot is drawn at
// the layout centre, which maps to itself.)
static u8* g_player;	// cPlayer making the crosshair draw call

static void __fastcall Call_DrawShortCursor(void* drawer, void* edx, void* obj, const float* pos)
{
	bool menuPointer = g_player && *(int*)(g_player + A::PlayerCrossHairState) == A::CrossHairPointer;
	Fit f;
	if(!menuPointer && GetFit(*(u8**)drawer, f))
	{
		float p[3] = { CursorX(pos[0], ImageHalfWidth(obj), f), pos[1], pos[2] };
		((DrawShort_t)A::DrawShort)(drawer, edx, obj, p);
		return;
	}
	((DrawShort_t)A::DrawShort)(drawer, edx, obj, pos);
}

// The crosshair call site holds the cPlayer in esi; note it, then continue as the draw call.
__declspec(naked) static void Thunk_DrawShortCursor()
{
	__asm
	{
		mov g_player, esi
		jmp Call_DrawShortCursor
	}
}

// Full-screen DrawTexture overlays (intro letterbox bars, the double-vision screen copy):
// stretch them to the real screen edges like the in-game overlays.
static void __fastcall Call_DrawTextureStretch(void* helper, void* edx, void* tex, const float* pos,
											   const float* size, const float* col, DWORD a4, DWORD a5, DWORD a6, DWORD a7)
{
	Fit f;
	if(GetFit(*(u8**)((u8*)helper + 0x14), f))
	{
		float p[3] = { pos[0] * f.k - f.off, pos[1], pos[2] };
		float s[3] = { size[0] * f.k, size[1], size[2] };
		((DrawTexture_t)A::DrawTexture)(helper, edx, tex, p, s, col, a4, a5, a6, a7);
		return;
	}
	((DrawTexture_t)A::DrawTexture)(helper, edx, tex, pos, size, col, a4, a5, a6, a7);
}

// The engine keeps a box around the camera, sized near*2, and uses it to pick Z-fail stencil
// shadows when the near plane might be inside a shadow volume. That box only reaches the near
// plane's corners up to about 4:3. Wider, a side corner can sit in a wall's shadow unnoticed,
// and that strip of the screen renders black. Size the box to contain the whole near plane.
static float __cdecl NearPlaneBoxSize(float nearPlane, float fov, float aspect)
{
	float t = tanf(fov * 0.5f);
	return 2.0f * nearPlane * sqrtf(1.0f + t * t * (1.0f + aspect * aspect));
}

// Replaces "fld [esp+78h]; mov edx, [esp+74h]; fadd st0, st0" (10 bytes).
__declspec(naked) static void Stub_OriginBoxSize()
{
	__asm
	{
		push eax
		push ecx
		push dword ptr [esp+8Ch]	// aspect: the game's [esp+80h]
		push dword ptr [esp+8Ch]	// fov:    the game's [esp+7Ch]
		push dword ptr [esp+8Ch]	// near:   the game's [esp+78h]
		call NearPlaneBoxSize
		add esp, 0Ch
		pop ecx
		pop eax
		mov edx, [esp+78h]			// the replaced "mov edx, [esp+74h]"
		ret
	}
}

// Fog culling keeps what lies within fogEnd*2/cos(fov*aspect/2) of the camera. From about
// 2.6:1 the cosine goes negative and everything is culled. Use the vanilla value where it is
// valid and at least as generous as the real view corner; otherwise use the real corner.
static float __cdecl FogCornerCos(float fov, float aspect)
{
	float t = tanf(fov * 0.5f);
	float exact = 1.0f / sqrtf(1.0f + t * t * (1.0f + aspect * aspect));
	float vanilla = cosf(fov * aspect * 0.5f);
	return (vanilla > 0 && vanilla < exact) ? vanilla : exact;
}

// Replaces "fmul [0.5]; fcos" (8 bytes). On entry st0 = fov*aspect, st1 = fogEnd*2.
static float g_fogEnd2;
__declspec(naked) static void Stub_FogCornerCos()
{
	__asm
	{
		fstp st(0)
		fstp g_fogEnd2				// keep the x87 stack empty across the C call
		push eax
		push ecx
		push edx
		push dword ptr [edi+14h]	// mfAspect
		push dword ptr [edi+10h]	// mfFOV
		call FogCornerCos
		add esp, 8
		pop edx
		pop ecx
		pop eax
		fld g_fogEnd2
		fxch st(1)					// st0 = result, st1 = fogEnd*2, as after fcos
		ret
	}
}

// A room is only drawn if a doorway (portal quad) leading to it is in view. Some levels'
// room meshes reach a little past their doorway quad, so near a doorway you can see into the
// next room while the quad itself is just outside the view. At 4:3 that never happens on
// screen; at 21:9 the room shows up black at the edge. Count doorways within a margin outside
// the view as visible; everything inside the room is still culled with the exact view.
static float g_portalMargin;	// metres; 0 = off
static int g_portalRescueLogs;

typedef bool (__fastcall *IntersectionBV_t)(void*, void*, void*, void*);

static bool __fastcall Call_PortalTest(void* container, void* edx, void* bv, void* parentSet)
{
	IntersectionBV_t test = (IntersectionBV_t)A::IntersectionBV;
	bool exact = test(container, edx, bv, parentSet);
	if(exact || g_portalMargin <= 0) return exact;

	u8* frustum = (u8*)container + A::ContainerFrustum;
	float saved[6];
	for(int i = 0; i < 5; ++i)	// left, right, bottom, top, near
	{
		float* d = (float*)(frustum + A::FrustumPlanes + i * 0x1C + 0xC);
		saved[i] = *d;
		*d += g_portalMargin;
	}
	float* radius = (float*)(frustum + A::FrustumSphereRadius);
	saved[5] = *radius;
	*radius += g_portalMargin;

	bool visible = test(container, edx, bv, parentSet);

	for(int i = 0; i < 5; ++i) *(float*)(frustum + A::FrustumPlanes + i * 0x1C + 0xC) = saved[i];
	*radius = saved[5];

	if(visible && g_portalRescueLogs < 40)
	{
		++g_portalRescueLogs;
		const float* eye = (const float*)(frustum + 0x14);
		Log("  doorway %d drawn thanks to the margin (camera %.1f %.1f %.1f)",
			*(int*)((u8*)bv - A::PortalBV + 4), eye[0], eye[1], eye[2]);
	}
	return visible;
}

//////////////////////////////////////////////////////////////////////////
// Patching

static bool SameBytes(DWORD addr, const void* bytes, size_t n)
{
	__try { return memcmp((void*)addr, bytes, n) == 0; }
	__except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Expect(DWORD addr, const void* bytes, size_t n, const char* what)
{
	if(SameBytes(addr, bytes, n)) return true;
	Log("  mismatch at %08X (%s)", addr, what);
	return false;
}

bool ExpectDword(DWORD addr, DWORD val, const char* what) { return Expect(addr, &val, 4, what); }

bool ExpectCall(DWORD site, DWORD target, const char* what)
{
	u8 b[5]; b[0] = 0xE8;
	*(DWORD*)(b + 1) = target - (site + 5);
	return Expect(site, b, 5, what);
}

void Write(DWORD addr, const void* bytes, size_t n)
{
	DWORD old;
	VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old);
	memcpy((void*)addr, bytes, n);
	VirtualProtect((void*)addr, n, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
}

void WriteDword(DWORD addr, DWORD val) { Write(addr, &val, 4); }

void RedirectCall(DWORD site, const void* fn)
{
	DWORD rel = (DWORD)fn - (site + 5);
	Write(site + 1, &rel, 4);
}

// Overwrite the first n bytes of a function with a jump to fn; return a trampoline that
// runs those bytes and continues in the original.
static void* Detour(DWORD addr, const void* fn, size_t n)
{
	u8* tramp = (u8*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if(!tramp) return NULL;
	memcpy(tramp, (void*)addr, n);
	tramp[n] = 0xE9;
	*(DWORD*)(tramp + n + 1) = (addr + n) - ((DWORD)tramp + n + 5);

	u8 jmp[16];
	memset(jmp, 0x90, sizeof(jmp));
	jmp[0] = 0xE9;
	*(DWORD*)(jmp + 1) = (DWORD)fn - (addr + 5);
	Write(addr, jmp, n);
	return tramp;
}

static bool VerifyGame()
{
	static const u8 drawLongHead[] = { 0x83, 0xEC, 0x3C, 0x56, 0x8B, 0x74, 0x24, 0x44 };
	static const u8 orthoCallEdi[] = { 0xFF, 0x57, 0x7C };
	static const u8 orthoCallEbp[] = { 0xFF, 0x55, 0x7C };
	static const int firstRes[] = { 640, 480, 800, 600, 1024, 768 };
	const float f800 = 800.0f;

	bool ok = true;
	ok &= ExpectDword(A::SetOrthoSlot, A::SetOrtho, "SetOrthoProjection vtable");
	ok &= ExpectDword(A::MouseUpdateSlot, A::MouseUpdate, "cMouseSDL::Update vtable");
	ok &= Expect(A::RetDrawTexture - 3, orthoCallEdi, 3, "DrawTexture ortho call");
	ok &= Expect(A::RetIntroDraw - 3, orthoCallEdi, 3, "IntroStory ortho call");
	ok &= Expect(A::RetDrawAll - 3, orthoCallEbp, 3, "DrawAll ortho call");
	static const u8 orthoCallEdx[] = { 0xFF, 0x52, 0x7C };
	ok &= Expect(A::RetGuiSetRender - 3, orthoCallEdx, 3, "GuiSet ortho call");
	ok &= ExpectCall(A::GuiMouseMoveCall, A::GuiSendMouseMove, "terminal mouse move");
	ok &= ExpectCall(A::GuiRenderClipCall, A::GuiRenderClip, "terminal clip regions");
	static const u8 scissorCall[] = { 0x8B, 0x06, 0x8D, 0x4C, 0x24, 0x14, 0x51, 0x8B, 0xCE, 0xFF, 0x90, 0xD8, 0x00, 0x00, 0x00 };
	ok &= Expect(A::GuiScissorCall - 9, scissorCall, sizeof(scissorCall), "terminal scissor call")
		&& ExpectDword(A::GfxVtable + A::ScissorSlot, A::SetScissorRect, "SetScissorRect vtable slot");
	ok &= ExpectCall(A::PostSceneDrawAfter3D, A::PostSceneDraw, "post-scene draw after 3D")
		&& ExpectCall(A::PostSceneDrawNo3D, A::PostSceneDraw, "post-scene draw without 3D");
	ok &= Expect(A::DrawLong, drawLongHead, sizeof(drawLongHead), "DrawGfxObject head");
	ok &= ExpectCall(A::CursorShortCall, A::DrawShort, "crosshair draw");
	static const u8 stateRead[] = { 0x8B, 0x86, 0xE4, 0x02, 0x00, 0x00 };
	static const u8 drawerLoad[] = { 0x8B, 0x8E, 0x80, 0x00, 0x00, 0x00 };
	ok &= Expect(A::CrossHairStateRead, stateRead, sizeof(stateRead), "crosshair state read via esi");
	ok &= Expect(A::CrossHairDrawerLoad, drawerLoad, sizeof(drawerLoad), "crosshair drawer load via esi");
	for(DWORD c : A::CursorLongCalls) ok &= ExpectCall(c, A::DrawLong, "item cursor draw");
	ok &= ExpectCall(A::NoiseTileCall, A::DrawLong, "noise tile draw");
	for(DWORD c : A::LetterboxCalls) ok &= ExpectCall(c, A::DrawTexture, "intro letterbox draw");
	static const u8 grabScreen[] = { 0xFF, 0x92, 0x74, 0x01, 0x00, 0x00 };	// call [edx+174h]
	ok &= Expect(A::ScreenCopyGrab, grabScreen, sizeof(grabScreen), "double vision screen copy")
		&& ExpectCall(A::ScreenCopyDrawCall, A::DrawTexture, "double vision draw");
	static const u8 drawTextureRet[] = { 0x83, 0xC4, 0x64, 0xC2, 0x20, 0x00 };	// add esp, 64h; ret 20h (8 arguments)
	ok &= Expect(A::DrawTextureRet, drawTextureRet, sizeof(drawTextureRet), "DrawTexture argument count");
	for(DWORD c : A::CursorMul800)
	{
		const u8 fmul[] = { 0xD8, 0x0D };
		ok &= Expect(c, fmul, 2, "cursor fmul") && ExpectDword(c + 2, A::Const800, "cursor fmul operand");
	}
	ok &= Expect(A::Const800, &f800, 4, "800.0f constant");
	ok &= Expect(A::Resolutions, firstRes, sizeof(firstRes), "resolution table");
	static const u8 originBox[] = { 0xD9, 0x44, 0x24, 0x78, 0x8B, 0x54, 0x24, 0x74, 0xDC, 0xC0 };
	ok &= Expect(A::OriginBoxSize, originBox, sizeof(originBox), "frustum origin box");
	static const u8 fogCos[] = { 0xD8, 0x0D, 0xD8, 0x2D, 0x67, 0x00, 0xD9, 0xFF };
	ok &= Expect(A::FogCornerCos, fogCos, sizeof(fogCos), "fog culling cosine");
	static const u8 portalBV[] = { 0x8D, 0x95, 0x90, 0x00, 0x00, 0x00, 0x52, 0x8B, 0xCE };	// lea edx,[ebp+90h]; push edx; mov ecx,esi
	ok &= Expect(A::PortalTestCall - 9, portalBV, sizeof(portalBV), "portal test arguments")
		&& ExpectCall(A::PortalTestCall, A::IntersectionBV, "portal test call");
	static const u8 frustumAdd[] = { 0x81, 0xC1, 0x6C, 0x02, 0x00, 0x00 };	// add ecx, 26Ch
	ok &= Expect(A::IntersectionFrustumTest, frustumAdd, sizeof(frustumAdd), "visibility frustum")
		&& ExpectCall(A::IntersectionFrustumTest + 6, A::FrustumCollideBV, "visibility frustum test");
	return ok;
}

static void PatchResolutions()
{
	static const int common[][2] = {
		{1024,768}, {1280,720}, {1280,1024}, {1366,768}, {1440,900}, {1600,900}, {1680,1050},
		{1920,1080}, {1920,1200}, {2560,1080}, {2560,1440}, {2560,1600}, {3440,1440},
		{3840,1600}, {3840,2160}, {5120,1440}, {5120,2160},
	};
	const int N = A::ResolutionNum;
	int dw = GetSystemMetrics(SM_CXSCREEN), dh = GetSystemMetrics(SM_CYSCREEN);

	int list[N + 1][2];
	int n = 0;
	bool haveDesktop = false;
	for(auto& r : common)
	{
		if(r[0] == dw && r[1] == dh) haveDesktop = true;
		list[n][0] = r[0]; list[n][1] = r[1]; ++n;
	}
	if(!haveDesktop && dw > 0 && dh > 0)
	{
		// Keep the list at N entries: drop the largest entry that the desktop can't show.
		int drop = n - 1;
		for(int i = n - 1; i >= 0; --i)
			if(list[i][0] > dw || list[i][1] > dh) { drop = i; break; }
		for(int i = drop; i < n - 1; ++i) { list[i][0] = list[i + 1][0]; list[i][1] = list[i + 1][1]; }
		--n;
		int at = n;
		while(at > 0 && (list[at - 1][0] > dw || (list[at - 1][0] == dw && list[at - 1][1] > dh))) --at;
		for(int i = n; i > at; --i) { list[i][0] = list[i - 1][0]; list[i][1] = list[i - 1][1]; }
		list[at][0] = dw; list[at][1] = dh; ++n;
	}
	Write(A::Resolutions, list, sizeof(int) * 2 * N);
}

static void ApplyPatches()
{
	if(!VerifyGame())
	{
		Log("Unexpected game code: this fix targets the Steam build of Black Plague (459397). Nothing was patched.");
		return;
	}

	if(CfgInt("Pillarbox2D", 1))
	{
		WriteDword(A::SetOrthoSlot, (DWORD)&Hook_SetOrtho);
		void* tramp = Detour(A::DrawLong, &Hook_DrawLong, 8);
		if(tramp) g_drawLong = (DrawLong_t)tramp;
		RedirectCall(A::CursorShortCall, &Thunk_DrawShortCursor);
		for(DWORD c : A::CursorLongCalls) RedirectCall(c, &Call_DrawLongCursor);
		RedirectCall(A::NoiseTileCall, &Call_DrawLongStretch);
		for(DWORD c : A::LetterboxCalls) RedirectCall(c, &Call_DrawTextureStretch);
		RedirectCall(A::ScreenCopyDrawCall, &Call_DrawTextureStretch);
		RedirectCall(A::GuiMouseMoveCall, &Call_GuiMouseMove);
		RedirectCall(A::GuiRenderClipCall, &Call_GuiRenderClip);
		u8 scissor[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
		*(DWORD*)(scissor + 1) = (DWORD)&Call_GuiScissor - (A::GuiScissorCall + 5);
		Write(A::GuiScissorCall, scissor, sizeof(scissor));
		RedirectCall(A::PostSceneDrawAfter3D, &Call_PostSceneDrawAfter3D);
		RedirectCall(A::PostSceneDrawNo3D, &Call_PostSceneDrawNo3D);
		Log("  2D layout pillarboxed (incl. terminals), overlays full width%s", tramp ? "" : " (overlay detour FAILED)");
	}
	if(CfgInt("FixMouse", 1))
	{
		WriteDword(A::MouseUpdateSlot, (DWORD)&Hook_MouseUpdate);
		for(DWORD c : A::CursorMul800) WriteDword(c + 2, (DWORD)&g_cursorMulX);
		Log("  mouse speed fixed");
	}
	if(CfgInt("MoreResolutions", 1))
	{
		PatchResolutions();
		Log("  options menu resolutions updated");
	}

	u8 call[10] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
	*(DWORD*)(call + 1) = (DWORD)&Stub_OriginBoxSize - (A::OriginBoxSize + 5);
	Write(A::OriginBoxSize, call, 10);
	*(DWORD*)(call + 1) = (DWORD)&Stub_FogCornerCos - (A::FogCornerCos + 5);
	Write(A::FogCornerCos, call, 8);
	g_portalMargin = (float)CfgInt("DoorwayMargin", 2);
	if(g_portalMargin > 0) RedirectCall(A::PortalTestCall, &Call_PortalTest);
	Log("  wide-view shadow and fog culling fixed; doorway margin %.0f m", g_portalMargin);

	// Bug in the original game, with Environmental Audio on: the effect slots are built before
	// their manager switches threading on, so they never create their mutex. The slot updater
	// thread then locks, and shutdown frees, whatever that memory held, which crashes whenever
	// it isn't zero. Always create the mutex (it is only used once threading is on).
	static const u8 slotMutexIf[] = { 0x38, 0x48, 0x18, 0x89, 0x4C, 0x24, 0x10, 0x74, 0x08 };
	if(Expect(A::EfxSlotMutexIf, slotMutexIf, sizeof(slotMutexIf), "effect slot mutex") &&
	   ExpectCall(A::EfxSlotMutexIf + 9, A::SdlCreateMutex, "effect slot SDL_CreateMutex"))
	{
		static const u8 nops[] = { 0x90, 0x90 };
		Write(A::EfxSlotMutexIf + 7, nops, sizeof(nops));
		Log("  Environmental Audio crash (original game bug) fixed");
	}

	int rate = ChooseLogicRate();
	if(rate != 60)
	{
		if(VerifyHighFramerate()) ApplyHighFramerate(rate);
		else Log("  high framerate skipped: unexpected code, game logic stays at 60 Hz");
	}
	Log("Done.");
}

//////////////////////////////////////////////////////////////////////////
// SDL_image proxy: everything but IMG_Load is forwarded to the real DLL.

#define FORWARD(name, ord) __pragma(comment(linker, "/EXPORT:" #name "=SDL_image_orig." #name ",@" #ord))
FORWARD(IMG_InvertAlpha, 1)
FORWARD(IMG_Linked_Version, 2)
FORWARD(IMG_LoadBMP_RW, 4)
FORWARD(IMG_LoadGIF_RW, 5)
FORWARD(IMG_LoadJPG_RW, 6)
FORWARD(IMG_LoadLBM_RW, 7)
FORWARD(IMG_LoadPCX_RW, 8)
FORWARD(IMG_LoadPNG_RW, 9)
FORWARD(IMG_LoadPNM_RW, 10)
FORWARD(IMG_LoadTGA_RW, 11)
FORWARD(IMG_LoadTIF_RW, 12)
FORWARD(IMG_LoadTyped_RW, 13)
FORWARD(IMG_LoadXCF_RW, 14)
FORWARD(IMG_LoadXPM_RW, 15)
FORWARD(IMG_LoadXV_RW, 16)
FORWARD(IMG_Load_RW, 17)
FORWARD(IMG_ReadXPMFromArray, 18)
FORWARD(IMG_isBMP, 19)
FORWARD(IMG_isGIF, 20)
FORWARD(IMG_isJPG, 21)
FORWARD(IMG_isLBM, 22)
FORWARD(IMG_isPCX, 23)
FORWARD(IMG_isPNG, 24)
FORWARD(IMG_isPNM, 25)
FORWARD(IMG_isTIF, 26)
FORWARD(IMG_isXCF, 27)
FORWARD(IMG_isXPM, 28)
FORWARD(IMG_isXV, 29)

//////////////////////////////////////////////////////////////////////////
// Crash report
//
// If the game crashes, write where (module + offset), the registers and the code addresses on
// the crashing thread's stack to the log, so a report can be traced. Only faults in modules
// from the game's folder are reported, at most three, and nothing is handled here.

static void* g_crashHandler;

static bool ModuleOf(DWORD addr, HMODULE& mod, char* path)
{
	if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   (LPCSTR)addr, &mod)) return false;
	return GetModuleFileNameA(mod, path, MAX_PATH) != 0;
}

static void Describe(DWORD addr, char* out, size_t n)
{
	HMODULE mod; char path[MAX_PATH];
	if(!ModuleOf(addr, mod, path)) { _snprintf_s(out, n, _TRUNCATE, "%08X", addr); return; }
	const char* name = strrchr(path, '\\');
	_snprintf_s(out, n, _TRUNCATE, "%s+%X (%08X)", name ? name + 1 : path, addr - (DWORD)mod, addr);
}

static bool IsImageCode(DWORD addr)
{
	MEMORY_BASIC_INFORMATION mbi;
	if(addr < 0x10000 || !VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
	return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
		(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static LONG CALLBACK CrashReport(EXCEPTION_POINTERS* ep)
{
	const EXCEPTION_RECORD* er = ep->ExceptionRecord;
	const DWORD code = er->ExceptionCode;
	if(code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
	   code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO) return EXCEPTION_CONTINUE_SEARCH;
	const CONTEXT* c = ep->ContextRecord;
	HMODULE mod; char path[MAX_PATH];
	if(!ModuleOf(c->Eip, mod, path) || _strnicmp(path, g_dir, strlen(g_dir)) != 0) return EXCEPTION_CONTINUE_SEARCH;
	static volatile LONG reports;
	if(InterlockedIncrement(&reports) > 3) return EXCEPTION_CONTINUE_SEARCH;

	char where[MAX_PATH + 32];
	Describe(c->Eip, where, sizeof(where));
	Log("Crash %08X on thread %u at %s (address %08X)", code, GetCurrentThreadId(), where,
		er->NumberParameters >= 2 ? (DWORD)er->ExceptionInformation[1] : 0);
	Log("  eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X",
		c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
	const DWORD* sp = (const DWORD*)c->Esp;
	const DWORD* top = (const DWORD*)((NT_TIB*)NtCurrentTeb())->StackBase;
	int shown = 0;
	for(const DWORD* p = sp; p < top && p < sp + 2048 && shown < 40; ++p)
	{
		if(!IsImageCode(*p)) continue;
		Describe(*p, where, sizeof(where));
		Log("  [esp+%03X] %s", (DWORD)((p - sp) * 4), where);
		++shown;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// Patch once, as early as possible: some fixes change values the game stores while building
// its objects. The game's first SDL_Init call (engine start-up, before any game object
// exists) is the preferred moment; the first IMG_Load is the fallback.
static void EnsurePatched(const char* when)
{
	static bool done;
	if(done) return;
	done = true;
	g_crashHandler = AddVectoredExceptionHandler(0, CrashReport);	// after the Steam stub has run
	Log("Patching at %s (main thread %u)", when, GetCurrentThreadId());
	if(CfgInt("Enabled", 1)) ApplyPatches();
	else Log("Disabled in PenumbraUltrawide.ini");
}

// The real SDL_image. The game only imports IMG_Load, which this DLL implements itself, so
// Windows doesn't load the original with the game. Load it at engine start-up, before the
// sound system starts its thread, as it would have been without the proxy.
typedef void* (__cdecl *IMG_Load_t)(const char*);
static IMG_Load_t g_imgLoad;

static bool LoadRealImageLib()
{
	if(g_imgLoad) return true;
	char path[MAX_PATH];
	_snprintf_s(path, sizeof(path), _TRUNCATE, "%sSDL_image_orig.dll", g_dir);
	HMODULE orig = LoadLibraryA(path);
	if(orig) g_imgLoad = (IMG_Load_t)GetProcAddress(orig, "IMG_Load");
	if(!g_imgLoad) Log("Could not load IMG_Load from %s", path);
	return g_imgLoad != NULL;
}

const DWORD IatSDLInit = 0x006726F0;	// Penumbra.exe import slot for SDL_Init
typedef int (__cdecl *SDL_Init_t)(unsigned);
static SDL_Init_t g_sdlInit;

static int __cdecl Hook_SDLInit(unsigned flags)
{
	LoadRealImageLib();
	EnsurePatched("engine start");
	return g_sdlInit(flags);
}

extern "C" void* __cdecl IMG_Load(const char* file)
{
	if(!LoadRealImageLib()) return NULL;
	EnsurePatched("first image load");
	return g_imgLoad(file);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if(reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(inst);
		GetModuleFileNameA(inst, g_dir, MAX_PATH);
		char* slash = strrchr(g_dir, '\\');
		if(slash) slash[1] = 0;

		char path[MAX_PATH];
		_snprintf_s(path, sizeof(path), _TRUNCATE, "%sPenumbraUltrawide.log", g_dir);
		g_log = _fsopen(path, "w", _SH_DENYNO);	// readable while the game runs

		char exe[MAX_PATH] = "";
		GetModuleFileNameA(NULL, exe, MAX_PATH);
		const char* name = strrchr(exe, '\\');
		Log("Penumbra: Black Plague ultrawide fix, loaded by %s", name ? name + 1 : exe);

		// The game's code is still encrypted here, but its import table is already filled in.
		HMODULE sdl = GetModuleHandleA("SDL.dll");
		g_sdlInit = sdl ? (SDL_Init_t)GetProcAddress(sdl, "SDL_Init") : NULL;
		if(g_sdlInit && SameBytes(IatSDLInit, &g_sdlInit, 4)) WriteDword(IatSDLInit, (DWORD)&Hook_SDLInit);
	}
	else if(reason == DLL_PROCESS_DETACH && g_crashHandler)
	{
		RemoveVectoredExceptionHandler(g_crashHandler);
		g_crashHandler = NULL;
	}
	return TRUE;
}
