// Offline test for PenumbraUltrawide (Black Plague): maps a memory dump of the running game at
// its real address, loads the proxy SDL_image.dll so it patches that image, then drives each
// hook with fake game objects and checks the results. Never runs real game logic.
//
// Usage (from a folder holding SDL_image.dll, SDL_image_orig.dll and its dependencies):
//   patch_test.exe <bp_mem.bin> <some.jpg> <game dir>

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <math.h>
#include <string>

typedef unsigned char u8;
static int g_fail;

#define CHECK(cond, ...) do { if(!(cond)) { ++g_fail; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                              else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while(0)
static bool Near(float a, float b) { return fabsf(a - b) < 0.01f; }

static void PutJmp(DWORD at, const void* to)
{
	DWORD old; VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
	*(u8*)at = 0xE9; *(DWORD*)(at + 1) = (DWORD)to - (at + 5);
}
static DWORD CallTarget(DWORD site) { return site + 5 + *(int*)(site + 1); }

// Fake cLowLevelGraphicsSDL at 3440x1440 with the 800x600 virtual size
static u8 g_gfx[0x20];
static void InitGfx() { *(int*)(g_gfx + 4) = 3440; *(int*)(g_gfx + 8) = 1440; *(float*)(g_gfx + 0xC) = 800; *(float*)(g_gfx + 0x10) = 600; }
static const float K = (600.0f * 3440 / 1440) / 800, OFF = (600.0f * 3440 / 1440 - 800) / 2;

// Captures of what reached the "original" game functions
static float cPos[3], cSize[3]; static int cCalls;
static void __fastcall CapLong(void*, void*, void*, const float* p, const float* s, const float*, int, int, float)
{ memcpy(cPos, p, 12); memcpy(cSize, s, 8); ++cCalls; }
static void __fastcall CapShort(void*, void*, void*, const float* p) { memcpy(cPos, p, 12); ++cCalls; }
static DWORD cTexExtra[4];
static void __fastcall CapTex(void*, void*, void*, const float* p, const float* s, const float*, DWORD a4, DWORD a5, DWORD a6, DWORD a7)
{ memcpy(cPos, p, 12); memcpy(cSize, s, 12); cTexExtra[0] = a4; cTexExtra[1] = a5; cTexExtra[2] = a6; cTexExtra[3] = a7; ++cCalls; }
static void __fastcall CapOrtho(void*, void*, const float*, float, float) { ++cCalls; }
static void __fastcall CapMouse(void* m, void*) { *(float*)((u8*)m + 0x2C) = 10; *(float*)((u8*)m + 0x30) = 10; ++cCalls; }
static int g_capFrames;
static void __cdecl CapFreeze(void*, float, float, int frames) { g_capFrames = frames; }
// Stand-in for cSectorVisibilityContainer::IntersectionBV: "visible" once the frustum's left
// plane has been pushed out to at least g_fakeNeedLeftD
static int g_fakeCalls; static float g_fakeNeedLeftD;
static bool __fastcall FakePortalTest(void* container, void*, void*, void*)
{
	++g_fakeCalls;
	return *(float*)((u8*)container + 0x26C + 0x2D0 + 0xC) >= g_fakeNeedLeftD;
}
static float g_guiPos[2], g_guiRel[2]; static int g_guiCalls;
static bool __fastcall CapGuiMouse(void*, void*, const float* p, const float* r)
{ g_guiPos[0] = p[0]; g_guiPos[1] = p[1]; g_guiRel[0] = r[0]; g_guiRel[1] = r[1]; ++g_guiCalls; return true; }
static int g_postSceneCalls;
static void __fastcall CapPostScene(void*, void*) { ++g_postSceneCalls; }
static float g_capVec[3]; static void* g_capThis;
static void __fastcall CapVec(void* self, void*, const float* v) { g_capThis = self; memcpy(g_capVec, v, 12); }

// Stand-in for cSqScript::LoadCharBuffer: reads g_fakeFile into a buffer from the game's new[]
static char g_gameDir[MAX_PATH];
static const char* g_fakeFile;
static size_t g_fakeOrigLen;
static char* g_fakeLastBuf;
static char* __fastcall FakeLoadCharBuffer(void*, void*, const void*, int* len)
{
	FILE* f; fopen_s(&f, g_fakeFile, "rb");
	if(!f) return NULL;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	typedef void* (__cdecl *New_t)(size_t);
	char* buf = (char*)((New_t)GetProcAddress(GetModuleHandleA("msvcr71.dll"), "??_U@YAPAXI@Z"))(n);
	fread(buf, 1, n, f); fclose(f);
	*len = n; g_fakeOrigLen = n; g_fakeLastBuf = buf;
	return buf;
}

typedef void (__fastcall *Long_t)(void*, void*, void*, const float*, const float*, const float*, int, int, float);
typedef void (__fastcall *Short_t)(void*, void*, void*, const float*);
typedef void (__fastcall *Tex_t)(void*, void*, void*, const float*, const float*, const float*, DWORD, DWORD, DWORD, DWORD);
typedef void (__fastcall *Mouse_t)(void*, void*);

// The redirected crosshair call expects the cPlayer in esi, like the real call site.
static void CallCrosshair(void* player, void* drawer, void* obj, float* pos)
{
	void* fn = (void*)CallTarget(0x4A02B7);
	__asm {
		push esi
		mov esi, player
		push pos
		push obj
		mov ecx, drawer
		call fn
		pop esi
	}
}

// The fog-culling stub at its call site: edi = camera, st0 = fov*aspect, st1 = fogEnd*2
static float CallFogStub(float fov, float aspect, float* fogEnd2Out)
{
	void* fn = (void*)CallTarget(0x52A953);
	float cam[8] = {}; cam[4] = fov; cam[5] = aspect;	// mfFOV +10h, mfAspect +14h
	float* pCam = cam;
	float prod = fov * aspect, two = 40.0f, r = 0, f2 = 0;
	__asm {
		push edi
		mov edi, pCam
		fld two
		fld prod
		call fn
		fstp r
		fstp f2
		pop edi
	}
	*fogEnd2Out = f2;
	return r;
}

static HGLRC g_rc;
static void DropGLContext() { if(g_rc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_rc); g_rc = NULL; } }

static bool MakeGLContext()
{
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "uwtest";
	RegisterClassA(&wc);
	HWND w = CreateWindowA("uwtest", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	HDC dc = GetDC(w);
	PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
	SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
	g_rc = wglCreateContext(dc);
	return g_rc && wglMakeCurrent(dc, g_rc);
}

static int TestRate()
{
	DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
	EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
	int rate = (int)dm.dmDisplayFrequency; if(rate < 60) rate = 60; if(rate > 360) rate = 360;
	return rate;
}

static std::string ReadLog()
{
	std::string s; FILE* f;
	if(fopen_s(&f, "PenumbraUltrawide.log", "rb") == 0) { char b[4096]; size_t n; while((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n); fclose(f); }
	return s;
}

int main(int argc, char** argv)
{
	if(argc < 4) { printf("usage: patch_test <dump> <jpg> <game dir>\n"); return 2; }
	strcpy_s(g_gameDir, argv[3]);
	InitGfx();

	// 1. Map the dump at the game's image base
	FILE* f; fopen_s(&f, argv[1], "rb");
	if(!f) { printf("cannot open dump\n"); return 2; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	u8* img = (u8*)VirtualAlloc((void*)0x400000, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if(img != (u8*)0x400000)
	{
		// Address-space randomisation sometimes puts a heap there first; a fresh process usually won't.
		char tries[8] = "0";
		GetEnvironmentVariableA("UWTEST_TRIES", tries, sizeof(tries));
		int t = atoi(tries);
		if(t >= 10) { printf("could not map at 0x400000 after %d tries\n", t); return 2; }
		sprintf_s(tries, "%d", t + 1);
		SetEnvironmentVariableA("UWTEST_TRIES", tries);
		STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi;
		if(!CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return 2;
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD rc = 2; GetExitCodeProcess(pi.hProcess, &rc);
		return (int)rc;
	}
	fread(img, 1, n, f); fclose(f);

	// The game's CRT and Newton are loaded in the real process; the high-framerate checks
	// look for them, so load them and point the dump's Newton import at the real export.
	HMODULE crt = LoadLibraryA("msvcr71.dll");
	HMODULE newton = LoadLibraryA("Newton.dll");
	CHECK(crt && newton, "game CRT and Newton load");
	FARPROC setFreeze = newton ? GetProcAddress(newton, "NewtonBodySetFreezeTreshold") : NULL;
	*(FARPROC*)0x6724CC = setFreeze;
	// Same for SDL_Init: the proxy hooks it at load time to patch before game objects exist
	HMODULE sdl = LoadLibraryA("SDL.dll");
	FARPROC sdlInit = sdl ? GetProcAddress(sdl, "SDL_Init") : NULL;
	*(FARPROC*)0x6726F0 = sdlInit;

	// 2. Load the proxy; patching happens at the game's first SDL_Init
	HMODULE dll = LoadLibraryA("SDL_image.dll");
	CHECK(dll != NULL, "proxy SDL_image.dll loads");
	CHECK(dll && *(FARPROC*)0x6726F0 != sdlInit, "SDL_Init import hooked at DLL load (code still encrypted then)");
	CHECK(*(u8*)0x4F4710 != 0xE9, "nothing patched before SDL_Init");
	typedef int (__cdecl *SDL_Init_t)(unsigned);
	(*(SDL_Init_t*)0x6726F0)(0);
	CHECK(*(u8*)0x4F4710 == 0xE9, "first SDL_Init applies the patches");
	{
		std::string log = ReadLog();
		CHECK(log.find("mismatch") == std::string::npos && log.find("Unexpected") == std::string::npos && log.find("skipped") == std::string::npos,
			  "log reports no mismatched patch sites");
	}
	typedef void* (__cdecl *IMG_Load_t)(const char*);
	IMG_Load_t imgLoad = (IMG_Load_t)GetProcAddress(dll, "IMG_Load");
	void* surf = imgLoad ? imgLoad(argv[2]) : NULL;
	CHECK(surf != NULL, "IMG_Load forwards to the real SDL_image and returns a surface");
	CHECK(GetProcAddress(dll, "IMG_isJPG") != NULL, "forwarded export IMG_isJPG resolves");

	// 3. Patched sites
	DWORD dllLo = (DWORD)dll, dllHi = dllLo + 0x100000;
	auto inDll = [&](DWORD a) { return a >= dllLo && a < dllHi; };
	CHECK(inDll(*(DWORD*)0x6883D4), "SetOrthoProjection vtable slot hooked");
	CHECK(inDll(*(DWORD*)0x69289C), "cMouseSDL::Update vtable slot hooked");
	CHECK(*(u8*)0x4F4710 == 0xE9 && inDll(CallTarget(0x4F4710)), "DrawGfxObject detoured");
	DWORD sites[] = { 0x4A02B7, 0x4A0101, 0x4A0172, 0x4A01B9, 0x4A86F1, 0x4694DD, 0x469558, 0x40A8B6, 0x40476B, 0x4EE01B, 0x4EE033 };
	for(DWORD s : sites) CHECK(inDll(CallTarget(s)), "call site %08X redirected", s);
	float* mul = *(float**)(0x4AA30B + 2);
	CHECK(inDll((DWORD)mul) && *(float**)(0x4ADBC7 + 2) == mul && *(float**)(0x4AE108 + 2) == mul, "cursor fmul operands point at the DLL's multiplier");
	int* res = (int*)0x6CF030;
	bool sorted = true;
	for(int i = 1; i < 17; ++i)
		if(res[i*2] < res[i*2-2] || (res[i*2] == res[i*2-2] && res[i*2+1] <= res[i*2-1])) sorted = false;
	bool hasDesk = false; for(int i = 0; i < 17; ++i) if(res[i*2] == GetSystemMetrics(SM_CXSCREEN) && res[i*2+1] == GetSystemMetrics(SM_CYSCREEN)) hasDesk = true;
	CHECK(hasDesk && sorted, "resolution list has the desktop resolution and is sorted (first %dx%d, last %dx%d)", res[0], res[1], res[32], res[33]);

	// 4. Overlay rule through the real detour + trampoline. Unwind the trampoline's stolen
	//    "sub esp,3Ch / push esi / mov esi,[esp+44h]" at 0x4F4718 and hand the call to the capture function.
	{
		DWORD old; VirtualProtect((void*)0x4F4718, 8, PAGE_EXECUTE_READWRITE, &old);
		u8 unwind[] = { 0x5E, 0x83, 0xC4, 0x3C };
		memcpy((void*)0x4F4718, unwind, 4);
		PutJmp(0x4F471C, &CapLong);
	}
	u8* drawer[1] = { g_gfx };
	float col[4] = { 1, 1, 1, 1 };
	Long_t drawLong = (Long_t)0x4F4710;
	{
		float p[3] = { 0, 0, 150 }, s[2] = { 800, 600 };
		cCalls = 0; drawLong(drawer, 0, (void*)1, p, s, col, 0, 0, 0);
		CHECK(cCalls == 1 && Near(cPos[0], -OFF) && Near(cSize[0], 800 * K) && cSize[1] == 600 && cPos[2] == 150,
			  "full-screen fade stretched to screen edges (x=%.2f w=%.2f)", cPos[0], cSize[0]);
		float p2[3] = { 0, 525, 40 }, s2[2] = { 800, 75 };
		drawLong(drawer, 0, (void*)1, p2, s2, col, 0, 0, 0);
		CHECK(Near(cPos[0], -OFF) && Near(cSize[0], 800 * K) && cPos[1] == 525, "letterbox bar stretched");
		float p4[3] = { -120, -120, 4 }, s4[2] = { 1040, 840 };	// fear vignette grown past the edges
		drawLong(drawer, 0, (void*)1, p4, s4, col, 0, 0, 0);
		CHECK(Near(cPos[0] + cSize[0] * 0.5f, 400) && cPos[0] < -OFF, "oversized vignette stays centred and covers the screen");
		float p3[3] = { 120, 200, 5 }, s3[2] = { 64, 64 };
		drawLong(drawer, 0, (void*)1, p3, s3, col, 0, 0, 0);
		CHECK(cPos[0] == 120 && cSize[0] == 64, "ordinary HUD sprite untouched");
	}

	// 5. Held-item cursor (long) and noise tile wrappers
	{
		Long_t cursor = (Long_t)CallTarget(0x4A0101);
		float p[3] = { 0 - 20, 300 - 20, 100 }, s[2] = { 40, 40 };	// centred on stretched x=0
		cursor(drawer, 0, (void*)1, p, s, col, 0, 0, 0);
		CHECK(Near(cPos[0] + 20, -OFF) && cSize[0] == 40, "item cursor at screen-left edge lands on the left edge (centre %.2f)", cPos[0] + 20);
		float pc[3] = { 400 - 20, 300 - 20, 100 };
		cursor(drawer, 0, (void*)1, pc, s, col, 0, 0, 0);
		CHECK(Near(cPos[0], 380), "item cursor at centre stays centred");

		Long_t noise = (Long_t)CallTarget(0x4A86F1);
		float pn[3] = { 760, 0, 0 }, sn[2] = { 40, 40 };
		noise(drawer, 0, (void*)1, pn, sn, col, 0, 0, 0);
		CHECK(Near(cPos[0] + cSize[0], 800 + OFF), "last noise tile reaches the right screen edge (%.2f)", cPos[0] + cSize[0]);
		float p0[3] = { 0, 0, 0 };
		noise(drawer, 0, (void*)1, p0, sn, col, 0, 0, 0);
		CHECK(Near(cPos[0], -OFF), "first noise tile starts at the left screen edge (%.2f)", cPos[0]);
	}

	// 6. Crosshair (short overload): image width read through mpMat -> image -> size
	{
		PutJmp(0x4F47E0, &CapShort);
		u8 image[0x80] = {}; *(int*)(image + 0x6C) = 32;
		u8* imgs[1] = { image };
		u8 mat[0xC0] = {}; *(u8***)(mat + 0xB0) = imgs;
		u8 obj[0x20] = {}; *(u8**)(obj + 0x10) = mat;
		u8 player[0x300] = {};
		float p[3] = { 800 - 16, 300 - 16, 100 };	// centred on stretched right edge
		*(int*)(player + 0x2E4) = 1;	// Active
		cCalls = 0; CallCrosshair(player, drawer, obj, p);
		CHECK(cCalls == 1 && Near(cPos[0] + 16, 800 + OFF), "in-world crosshair at screen-right edge lands on the right edge (centre %.2f)", cPos[0] + 16);
		float pm[3] = { 250 - 16, 200 - 16, 100 };	// inventory mouse at layout x=250
		*(int*)(player + 0x2E4) = 5;	// Pointer
		CallCrosshair(player, drawer, obj, pm);
		CHECK(cCalls == 2 && cPos[0] == 250 - 16, "menu pointer (inventory/notebook) drawn where its clicks land (x=%.2f)", cPos[0] + 16);
		float pd[3] = { 400 - 16, 300 - 16, 100 };	// "no crosshair" centre dot
		*(int*)(player + 0x2E4) = 10;
		CallCrosshair(player, drawer, obj, pd);
		CHECK(cCalls == 3 && Near(cPos[0], 400 - 16), "centre dot stays in the middle (x=%.2f)", cPos[0] + 16);
	}

	// 7. Intro letterbox via cGraphicsHelper::DrawTexture (helper->mpLowLevelGfx at +0x14).
	//    Black Plague's DrawTexture takes 8 stack arguments (ret 20h); push them like the game
	//    does and check the wrapper passes the extra four through and pops all eight.
	{
		CHECK(*(u8*)0x45CFF6 == 0xC2 && *(WORD*)0x45CFF7 == 0x20, "game's DrawTexture pops 8 arguments (ret 20h)");
		PutJmp(0x45CBE0, &CapTex);
		u8 helper[0x20] = {}; *(u8**)(helper + 0x14) = g_gfx;
		void* tex = (void*)CallTarget(0x4694DD);
		float p[3] = { 0, 0, 140 }, s[3] = { 800, 75, 0 };
		float uv[2] = { 1, 1 };
		float* pp = p; float* ps = s; float* pc = col; float* puv = uv; void* ph = helper;
		DWORD espBefore, espAfter;
		cCalls = 0; memset(cTexExtra, 0, sizeof(cTexExtra));
		__asm {
			mov espBefore, esp
			push 0				// a7: use uv
			push puv			// a6
			push 0				// a5
			push 1				// a4
			push pc
			push ps
			push pp
			push 1				// texture
			mov ecx, ph
			call tex
			mov espAfter, esp
		}
		CHECK(espBefore == espAfter, "letterbox wrapper pops all 8 DrawTexture arguments (stack balanced)");
		CHECK(cTexExtra[0] == 1 && cTexExtra[1] == 0 && cTexExtra[2] == (DWORD)uv && cTexExtra[3] == 0, "extra DrawTexture arguments passed through");
		CHECK(cCalls == 1 && Near(cPos[0], -OFF) && Near(cSize[0], 800 * K) && cSize[1] == 75, "intro letterbox stretched");

		// Double vision: the screen copy, slightly enlarged around the centre, covers the real screen
		void* dv = (void*)CallTarget(0x40A8B6);
		float pd[3] = { -5, -3.75f, 0 }, sd[3] = { 810, 607.5f, 0 };
		pp = pd; ps = sd;
		cCalls = 0;
		__asm {
			mov espBefore, esp
			push 1
			push puv
			push 1
			push 1
			push pc
			push ps
			push pp
			push 1
			mov ecx, ph
			call dv
			mov espAfter, esp
		}
		CHECK(cCalls == 1 && espBefore == espAfter && Near(cPos[0] + cSize[0] * 0.5f, 400) && Near(cSize[0], 810 * K) && cPos[0] < -OFF && cSize[1] == 607.5f,
			  "double-vision screen copy covers the whole screen, centred (x=%.2f w=%.2f)", cPos[0], cSize[0]);
	}

	// 8. Mouse: horizontal relative motion boosted, in-world cursor multiplier compensated
	{
		PutJmp(0x597730, &CapMouse);
		u8 mouse[0x70] = {}; *(u8**)(mouse + 0x64) = g_gfx;
		cCalls = 0; ((Mouse_t)*(DWORD*)0x69289C)(mouse, 0);
		float rx = *(float*)(mouse + 0x2C), ry = *(float*)(mouse + 0x30);
		CHECK(cCalls == 1 && Near(rx, 10 * K) && ry == 10, "mouse rel x scaled by %.3f (x=%.2f y=%.2f)", K, rx, ry);
		CHECK(Near(*mul * K, 800), "in-world cursor multiplier = 800/k (%.2f)", *mul);
	}

	const int rate = TestRate();
	const float s = rate / 60.0f;
	auto ticks = [&](int n) { int v = (int)(n * s + 0.5f); return v < 1 ? 1 : v; };

	// 8b. Logic rate: the first GetStepSize call retunes the live timer to the monitor rate
	{
		double timer[4] = { 12345.0, 1000.0 / 60, 0, 0 };
		float step = ((float (__fastcall*)(void*))0x540A80)(timer);
		CHECK(Near(step * rate, 1) && fabs(timer[1] - 1000.0 / rate) < 1e-9 && timer[0] == 12345.0,
			  "logic timer retuned to %d Hz (step %.5f s, tick %.3f ms)", rate, step, timer[1]);
		typedef float (__fastcall *GameStep_t)(void*);
		void* game[0x40] = {}; *(double**)((u8*)game + *(u8*)(0x4C87E0 + 2)) = timer;
		CHECK(Near(((GameStep_t)0x4C87E0)(game) * rate, 1), "cGame::GetStepSize (used by game code) returns the new step");
	}

	// 8c. Level scripts are rewritten as they load
	{
		PutJmp(0x5412A0, &FakeLoadCharBuffer);
		typedef char* (__fastcall *Load_t)(void*, void*, const void*, int*);
		Load_t load = (Load_t)CallTarget(0x5414DF);
		CHECK(inDll((DWORD)load), "script load call redirected");

		struct { const char* path; char expect[128]; } files[] = {
			{ "maps\\level01_cells.hps" }, { "maps\\level04_messhall.hps" }, { "maps\\level11_infirmary.hps" },
			{ "maps\\level19_chemical_laboratory.hps" }, { "interfaces\\18_terminal_gui.hps" }, { "interfaces\\19_terminal_gui.hps" },
			{ "maps\\level00_tutorial.hps" }, { "maps\\level12_mental_realm.hps" }, { "maps\\level15_outside.hps" },
		};
		sprintf_s(files[0].expect, "fDiff < -%.6ff || fDiff > %.6ff)", 0.4 * 60 / rate, 0.4 * 60 / rate);
		sprintf_s(files[1].expect, "glCount %% %d == 0)", rate);
		sprintf_s(files[2].expect, "lForceCount == %d)", ticks(10));
		sprintf_s(files[3].expect, "gfVertVal -= %.6ff;", 0.003 * 60 / rate);
		sprintf_s(files[4].expect, "glCount == %d)", ticks(4));
		sprintf_s(files[5].expect, "glCount == %d)", ticks(2));
		sprintf_s(files[6].expect, "AddBodyForce(\"shelf01\",\"world\",%.1ff,%.1ff, 0);", -20000.0 * rate / 60, 10000.0 * rate / 60);
		sprintf_s(files[7].expect, "AddBodyForce(\"wndoor03_doorShape2\",\"World\",%.1ff,%.1ff,%.1ff);", -4000.0 * rate / 60, 1000.0 * rate / 60, -1000.0 * rate / 60);
		sprintf_s(files[8].expect, "AddBodyForce(\"icebridge\",\"World\",%.1ff,%.1ff,%.1ff);", -20000.0 * rate / 60, -5000.0 * rate / 60, 20000.0 * rate / 60);
		for(auto& fl : files)
		{
			char path[MAX_PATH]; sprintf_s(path, "%s\\%s", g_gameDir, fl.path);
			g_fakeFile = path;
			u8 name[0x1C] = {}; char nameBuf[96]; sprintf_s(nameBuf, "%s", fl.path);
			for(char* c = nameBuf; *c; ++c) if(*c == '\\') *c = '/';
			*(char**)(name + 4) = nameBuf; *(DWORD*)(name + 0x14) = (DWORD)strlen(nameBuf); *(DWORD*)(name + 0x18) = 95;
			int len = 0;
			char* text = load(NULL, NULL, name, &len);
			std::string str(text ? text : "", text ? len : 0);
			CHECK(text && str.find(fl.expect) != std::string::npos, "%s rewritten (contains '%s')", fl.path, fl.expect);
		}
		// level19 also carries the barricade pushes and all six robot-arm steps
		{
			char path[MAX_PATH]; sprintf_s(path, "%s\\maps\\level19_chemical_laboratory.hps", g_gameDir);
			g_fakeFile = path;
			u8 name[0x1C] = {}; char nameBuf[] = "maps/level19_chemical_laboratory.hps";
			*(char**)(name + 4) = nameBuf; *(DWORD*)(name + 0x14) = (DWORD)strlen(nameBuf); *(DWORD*)(name + 0x18) = 63;
			int len = 0;
			char* text = load(NULL, NULL, name, &len);
			std::string str(text ? text : "", text ? len : 0);
			char e1[96]; sprintf_s(e1, "\"world\", 0, 0, %.1ff);", -40000.0 * rate / 60);
			CHECK(str.find(e1) != std::string::npos && str.find("0.004f;") == std::string::npos && str.find("0, 0, 60000);") == std::string::npos,
				  "level19 barricade push and arm steps all rewritten");
		}
		std::string log = ReadLog();
		CHECK(log.find("script ") == std::string::npos, "every script edit matched the expected number of times");

		// Untouched script: same buffer back
		char path[MAX_PATH]; sprintf_s(path, "%s\\maps\\level02_vents.hps", g_gameDir);
		g_fakeFile = path;
		u8 name[0x1C] = {}; strcpy_s((char*)name + 4, 16, "vents.hps"); *(DWORD*)(name + 0x18) = 15;
		int len = 0;
		char* text = load(NULL, NULL, name, &len);
		CHECK(text == g_fakeLastBuf && len == (int)g_fakeOrigLen, "other scripts load unchanged");

		// Engine constants
		double slope = **(double**)(0x4D7E0D + 2);
		CHECK(inDll(*(DWORD*)(0x4D7E0D + 2)) && fabs(slope + 0.15 * 60 / rate) < 1e-9, "slope threshold scaled (%.4f)", slope);
		CHECK(*(u8*)(0x459507 + 1) == (u8)ticks(20), "door PID window %d ticks", *(u8*)(0x459507 + 1));

		void* probe = (void*)CallTarget(0x4D73CA);
		float sumSq = 4.0f, reach = 0;
		__asm {
			sub esp, 30h
			fld sumSq
			call probe
			fld dword ptr [esp+24h]
			fstp reach
			add esp, 30h
		}
		CHECK(inDll((DWORD)probe) && Near(reach, 2.0f * s), "stair probe reach x%.2f (%.3f)", s, reach);

		// Newton freeze frames: catch the call where it reaches Newton
		PutJmp((DWORD)setFreeze, &CapFreeze);
		typedef void (__cdecl *Freeze_t)(void*, float, float, int);
		CHECK(inDll(*(DWORD*)0x6724CC), "Newton freeze import hooked");
		(*(Freeze_t*)0x6724CC)((void*)1, 0.1f, 0.1f, 10);
		CHECK(g_capFrames == ticks(10), "prop freeze delay 10 -> %d physics steps", g_capFrames);
	}

	// 8d. Tick compensations
	{
		CHECK(Near(**(float**)(0x4AAB4B + 2), 100 * s), "door drag force x%.2f (%.1f)", s, **(float**)(0x4AAB4B + 2));
		CHECK(fabs(**(float**)(0x4A9B8B + 2) - 0.01f / s) < 1e-7, "hide 'standing still' threshold /%.2f", s);
		CHECK(fabs(**(float**)(0x428467 + 2) - 1e-5f / (s * s)) < 1e-10, "worm movement threshold /%.2f^2", s);
		CHECK(*(float*)0x6795D4 == 0.01f && *(float*)0x675B5C == 100.0f && *(float*)0x6731A8 == 0.6f, "shared game constants untouched");
		CHECK(*(int*)0x49FCA9 == ticks(25) && *(int*)0x4A4B14 == ticks(16) && *(int*)0x4071AA == ticks(150) && *(int*)0x428BF6 == ticks(20),
			  "tick counts scaled (ground grace %d, hand lag %d, path samples %d, worm %d)", *(int*)0x49FCA9, *(int*)0x4A4B14, *(int*)0x4071AA, *(int*)0x428BF6);
		CHECK(*(int*)0x4AAAA7 == ticks(20) && *(int*)0x4AAB80 == ticks(20) && *(int*)0x4AAC58 == ticks(20), "drag brake delay %d ticks", *(int*)0x4AAB80);
		CHECK(Near(*(float*)0x49CA01 * rate, 1) && Near(*(float*)0x48E048 * rate, 1), "sound trigger life = 1 tick (%.5f s)", *(float*)0x49CA01);

		// Jump: stub replaces "mov edx,[edi+80h]" between a cmp and its jne
		void* jump = (void*)CallTarget(0x49C49C);
		float state[0x30] = {}; state[0x80 / 4] = 350.0f;
		void* pState = state; DWORD edxOut; u8 zf;
		__asm {
			push edi
			mov edi, pState
			xor eax, eax
			cmp eax, eax			// ZF = 1
			call jump
			setz zf
			mov edxOut, edx
			pop edi
		}
		CHECK(Near(*(float*)&edxOut, 350 * s) && zf == 1, "jump force x%.2f (%.1f), flags kept", s, *(float*)&edxOut);

		// Knockback / spider leap: one-tick character force, original vector untouched
		PutJmp(0x4D5220, &CapVec);
		typedef void (__fastcall *Force_t)(void*, void*, const float*);
		float fv[3] = { 1, 2, 3 };
		((Force_t)CallTarget(0x42554E))((void*)0x55, 0, fv);
		CHECK(g_capThis == (void*)0x55 && Near(g_capVec[0], s) && Near(g_capVec[2], 3 * s) && fv[0] == 1,
			  "spider leap force x%.2f, game's vector untouched", s);
		((Force_t)CallTarget(0x4030B3))((void*)0x56, 0, fv);
		CHECK(g_capThis == (void*)0x56 && Near(g_capVec[1], 2 * s), "enemy hit knockback x%.2f", s);

		// Torque stub at "call [edx+80h]" sites
		void* vtbl[0x30] = {}; vtbl[0x80 / 4] = (void*)&CapVec; vtbl[0x78 / 4] = (void*)&CapVec;
		void* pVtbl = vtbl; float tq[3] = { 2, 4, 6 }; float* pTq = tq;
		DWORD espBefore, espAfter;
		DWORD torqueSites[] = { 0x45F27D, 0x458BF6 };
		for(DWORD site : torqueSites)
		{
			void* torque = (void*)CallTarget(site);
			g_capThis = 0;
			__asm {
				mov espBefore, esp
				push pTq
				mov ecx, 0x66
				mov edx, pVtbl
				call torque			// AddTorque pops the pointer, like the original call
				mov espAfter, esp
			}
			CHECK(espBefore == espAfter && g_capThis == (void*)0x66 && Near(g_capVec[1], 4 * s) && tq[1] == 4,
				  "one-tick torque at %08X x%.2f, stack balanced", site, s);
		}

		// Explosion stub: vector at [esp+40h..48h], pointer at [esp], y still on the FPU stack
		void* expl = (void*)CallTarget(0x402B3F);
		float y = 5; DWORD ecxOut; float ex[3];
		__asm {
			push esi
			sub esp, 50h
			lea eax, [esp+40h]
			mov [esp], eax
			mov dword ptr [esp+40h], 3F800000h	// x = 1
			mov dword ptr [esp+48h], 40400000h	// z = 3
			mov esi, 77h
			fld y
			call expl
			mov ecxOut, ecx
			mov eax, [esp]
			mov edx, [eax]
			mov ex, edx
			mov edx, [eax+4]
			mov ex+4, edx
			mov edx, [eax+8]
			mov ex+8, edx
			add esp, 50h
			pop esi
		}
		CHECK(ecxOut == 0x77 && Near(ex[0], s) && Near(ex[1], 5 * s) && Near(ex[2], 3 * s), "explosion push x%.2f", s);

		// Debris stub: vector at [esp+38h..40h], pointer at [esp], y on the FPU stack; tail-calls [eax+78h]
		void* brk = (void*)CallTarget(0x4437B1);
		g_capThis = 0;
		__asm {
			mov espBefore, esp
			sub esp, 44h
			lea eax, [esp+38h]
			mov [esp], eax
			mov dword ptr [esp+38h], 3F800000h	// x = 1
			mov dword ptr [esp+40h], 40400000h	// z = 3
			fld y
			mov ecx, 88h
			mov eax, pVtbl
			call brk			// AddForce pops the pointer
			add esp, 40h
			mov espAfter, esp
		}
		CHECK(espBefore == espAfter && g_capThis == (void*)0x88 && Near(g_capVec[0], s) && Near(g_capVec[1], 5 * s) && Near(g_capVec[2], 3 * s),
			  "debris force x%.2f, stack balanced", s);
	}

	// 8e. Wide-view culling: camera near-plane box and fog-culling box
	{
		void* origin = (void*)CallTarget(0x50E26D);
		CHECK(inDll((DWORD)origin) && *(u8*)(0x50E26D + 5) == 0x90, "frustum origin box patched");
		float nearP = 0.05f, fov = 1.2217305f /* 70 deg */, aspect = 3440.0f / 1440, farP = 100.0f;
		float box = 0; DWORD eaxOut, edxOut;
		__asm {
			sub esp, 90h
			mov eax, nearP
			mov [esp+78h], eax
			mov eax, fov
			mov [esp+7Ch], eax
			mov eax, aspect
			mov [esp+80h], eax
			mov eax, farP
			mov [esp+74h], eax
			mov eax, 12345678h
			call origin
			fstp box
			mov eaxOut, eax
			mov edxOut, edx
			add esp, 90h
		}
		float t = tanf(fov * 0.5f), corner = nearP * sqrtf(1 + t * t * (1 + aspect * aspect));
		CHECK(Near(box * 1000, 2 * corner * 1000) && box > 2 * nearP * 2 && eaxOut == 0x12345678 && *(float*)&edxOut == farP,
			  "near-plane box %.4f covers 21:9 corners (vanilla %.4f), registers kept", box, 2 * nearP);

		CHECK(inDll(CallTarget(0x52A953)), "fog culling cosine patched");
		float f2 = 0;
		float c219 = CallFogStub(1.2217305f, 3440.0f / 1440, &f2);
		CHECK(Near(c219, cosf(1.2217305f * (3440.0f / 1440) * 0.5f)) && f2 == 40.0f, "fog box at 21:9 unchanged from vanilla (cos %.4f), x87 stack intact", c219);
		float c329 = CallFogStub(1.2217305f, 5120.0f / 1440, &f2);
		CHECK(c329 > 0.2f && c329 < 1.0f, "fog box at 32:9 valid (cos %.4f; vanilla would be %.4f)", c329, cosf(1.2217305f * (5120.0f / 1440) * 0.5f));
	}

	// 8f. Doorway (portal) test: retried with the view widened by the margin, then restored
	{
		CHECK(inDll(CallTarget(0x555480)), "doorway visibility test redirected");
		PutJmp(0x554920, &FakePortalTest);
		static u8 container[0x700]; static u8 portal[0x120];
		*(int*)(portal + 4) = 7;
		float* leftD = (float*)(container + 0x26C + 0x2D0 + 0xC);
		float* radius = (float*)(container + 0x26C + 0x378);
		*leftD = 0; *radius = 10;
		typedef bool (__fastcall *Test_t)(void*, void*, void*, void*);
		Test_t portalTest = (Test_t)CallTarget(0x555480);
		g_fakeCalls = 0; g_fakeNeedLeftD = 1.5f;
		bool vis = portalTest(container, 0, portal + 0x90, NULL);
		CHECK(vis && g_fakeCalls == 2 && *leftD == 0 && *radius == 10, "doorway just outside the view is drawn; view restored afterwards");
		g_fakeCalls = 0; g_fakeNeedLeftD = -1;
		vis = portalTest(container, 0, portal + 0x90, NULL);
		CHECK(vis && g_fakeCalls == 1, "doorway in view: exact test only");
		g_fakeCalls = 0; g_fakeNeedLeftD = 5.0f;
		vis = portalTest(container, 0, portal + 0x90, NULL);
		CHECK(!vis && g_fakeCalls == 2 && *leftD == 0, "doorway far outside the view stays hidden");
	}

	// 9. Ortho: post effects pass through; UI pass gets the centred projection
	{
		PutJmp(0x55D8A0, &CapOrtho);
		DWORD hook = *(DWORD*)0x6883D4;
		float vs[2] = { 800, 600 };
		cCalls = 0; ((void (__fastcall*)(void*, void*, const float*, float, float))hook)(g_gfx, 0, vs, -1000, 1000);
		CHECK(cCalls == 1, "non-UI caller (post effects) gets the original projection");

		// Terminal mouse: absolute position (stretched space) moved into the centred layout
		{
			PutJmp(0x4E3190, &CapGuiMouse);
			typedef bool (__fastcall *GuiMove_t)(void*, void*, const float*, const float*);
			GuiMove_t move = (GuiMove_t)CallTarget(0x40476B);
			float rel[2] = { 3, 4 };
			float panelLeft = (3440 - 1920) / 2.0f / 3440 * 800;	// cursor on the panel's left edge
			auto at = [&](float ax, float ay) { float a[2] = { ax, ay }; move((void*)1, 0, a, rel); return g_guiPos[0]; };
			g_guiCalls = 0;
			float x0 = at(panelLeft, 300);
			float x1 = at(400, 123); float y1 = g_guiPos[1];
			CHECK(g_guiCalls == 2 && Near(x0, 0) && Near(x1, 400) && y1 == 123 && g_guiRel[0] == 3 && g_guiRel[1] == 4,
				  "terminal pointer maps into the centred panel (panel edge %.2f, centre %.2f)", x0, x1);
			float xl = at(5, 10);		// mouse pushed deep into the left bar
			float xl2 = at(15, 10);		// then back right a little
			CHECK(xl == 0 && Near(xl2, 10 * K), "left edge stops the pointer and lets go at once (%.2f -> %.2f)", xl, xl2);
			float xr = at(795, 10);		// across and deep into the right bar
			float xr2 = at(785, 10);	// then back left a little
			CHECK(xr == 800 && Near(xr2, 800 - 10 * K), "right edge stops the pointer and lets go at once (%.2f -> %.2f)", xr, xr2);
			float xm = at(400, 10);
			CHECK(xm > 0 && xm < 800 && fabsf(xm - 400) <= OFF + 0.01f, "pointer drift from the edges stays under a side bar's width (%.2f)", xm);
		}

		if(getenv("UWTEST_NO_GL")) { printf("skipping projection check (UWTEST_NO_GL)\n"); goto done; }
		CHECK(MakeGLContext(), "GL context for projection check");
		DWORD old; VirtualProtect((void*)0x4F3964, 1, PAGE_EXECUTE_READWRITE, &old);
		*(u8*)0x4F3964 = 0xC3;	// ret, back to us
		void* gfx = g_gfx; float* pvs = vs; float mn = -1000, mx = 1000;
		cCalls = 0;
		__asm {
			lea eax, back
			push eax
			push mx
			push mn
			push pvs
			mov ecx, gfx
			push 0x4F3964		; return address of the DrawAll call site
			jmp hook
		back:
		}
		float m[16]; glGetFloatv(GL_PROJECTION_MATRIX, m);
		float l = (-1 - m[12]) / m[0], r = (1 - m[12]) / m[0];
		CHECK(cCalls == 0 && Near(l, -OFF) && Near(r, 800 + OFF), "DrawAll projection spans x %.2f..%.2f", l, r);

		// Terminal (cGuiSet::Render) with the 3D world switched off: centred, side bars cleared,
		// GL state left as it was
		PutJmp(0x4E1C40, &CapPostScene);
		typedef void (__fastcall *Post_t)(void*, void*);
		g_postSceneCalls = 0;
		((Post_t)CallTarget(0x4EE033))((void*)1, 0);
		CHECK(g_postSceneCalls == 1, "post-scene draw (no 3D) still reaches the game");
		VirtualProtect((void*)0x4FA150, 1, PAGE_EXECUTE_READWRITE, &old);
		*(u8*)0x4FA150 = 0xC3;
		glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
		glDisable(GL_SCISSOR_TEST);
		cCalls = 0;
		__asm {
			lea eax, back2
			push eax
			push mx
			push mn
			push pvs
			mov ecx, gfx
			push 0x4FA150		; return address of the GuiSet ortho call
			jmp hook
		back2:
		}
		glGetFloatv(GL_PROJECTION_MATRIX, m);
		l = (-1 - m[12]) / m[0]; r = (1 - m[12]) / m[0];
		GLfloat cc[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
		CHECK(cCalls == 0 && Near(l, -OFF) && Near(r, 800 + OFF), "terminal projection spans x %.2f..%.2f", l, r);
		CHECK(glGetError() == GL_NO_ERROR && !glIsEnabled(GL_SCISSOR_TEST) && cc[0] == 0.25f && cc[2] == 0.75f,
			  "terminal side-bar clear leaves GL state as it was");
		g_postSceneCalls = 0;
		((Post_t)CallTarget(0x4EE01B))((void*)1, 0);
		CHECK(g_postSceneCalls == 1, "post-scene draw (after 3D) still reaches the game");
		DropGLContext();
	}

done:
	printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
	fflush(stdout);
	if(getenv("UWTEST_SKIP_UNLOAD")) ExitProcess(g_fail ? 1 : 0);
	FreeLibrary(dll);
	printf("proxy unloaded cleanly\n");
	fflush(stdout);
	return g_fail ? 1 : 0;
}
