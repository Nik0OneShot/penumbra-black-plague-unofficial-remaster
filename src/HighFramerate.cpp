// High framerate for Penumbra: Black Plague.
//
// Game logic (player, camera, physics, scripts) runs on a fixed 60 Hz timer and frames are
// only drawn after a logic tick, so the game never looked smoother than 60 FPS. This runs the
// timer at the monitor's refresh rate instead.
//
// Most of the game scales by the time step, but some code and level scripts count ticks or
// apply a force for a single tick. Each of those is compensated here so the game plays the
// same as at 60 Hz. The step size is fixed once the rate is chosen, so every compensation is
// a constant scaled by rate/60 (or 60/rate).

#include "Patch.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace H {
	// cLogicTimer: mlLocalTime (double ms) at +0, mlLocalTimeAdd (double ms per tick) at +8.
	// Everything that asks for the step size ends up here (cGame::GetStepSize tail-jumps to it).
	const DWORD TimerGetStepSize = 0x00540A80;	// fld qword [ecx+8]; fmul [0.001f]; ret
	const DWORD GameGetStepSize  = 0x004C87E0;	// mov ecx, [ecx+20h]; jmp TimerGetStepSize

	// cSqScript::CreateFromFile -> LoadCharBuffer(const tString& file, int& len)
	const DWORD ScriptLoadCall   = 0x005414DF;
	const DWORD LoadCharBuffer   = 0x005412A0;

	// cCharacterBody::Update
	const DWORD SlopeCompare     = 0x004D7E0D;	// fcomp qword [-0.15]: anti-slide on gentle slopes
	const DWORD SlopeConst       = 0x0067F828;
	const DWORD StepProbe        = 0x004D73CA;	// fsqrt; fstp [esp+24h]: fForwadAdd = |vPosAdd|

	// cGameSwingDoor: Stop controller SetPidIntegralSize(20)
	const DWORD DoorPidSize      = 0x00459507;	// push 14h
	const DWORD SetPidIntegralSize = 0x00528FD0;

	// Newton.dll import used for auto-freezing bodies (frames counted per physics step)
	const DWORD IatSetFreezeTreshold = 0x006724CC;

	// cPlayerMoveState_Jump::EnterState: fForce = mfStartForce, applied as a one-tick force
	const DWORD JumpForceRead    = 0x0049C49C;	// mov edx, [edi+80h]

	// iCharacterBody::AddForce(const cVector3f&): adds to a force that is applied for one tick
	const DWORD CharAddForce     = 0x004D5220;
	const DWORD OneTickCharForceCalls[] = {
		0x004030B3,	// cAttackHandler::CreateShapeAttack: player knockback from enemy hits
		0x004027B7,	// cAttackHandler::CreateSplashDamage: player knockback from explosions
		0x0042554E,	// spider attack leap
	};
	// call [edx+80h] = iPhysicsBody::AddTorque, given once
	const DWORD OneTickTorqueCalls[] = {
		0x0046036E,	// melee weapon: spin on hit enemies
		0x0045F27D,	// melee weapon: spin on hit objects
		0x0045DD0D,	// thrown items
		0x004A85C2,	// thrown flare
		0x00458BF6,	// pieces of a broken door
	};
	const DWORD ExplosionForce   = 0x00402B3F;	// mov ecx, esi; fstp [esp+44h]; then call [edx+78h] (AddForce)
	const DWORD BreakPieceForce  = 0x004437B1;	// fstp [esp+3Ch]; call [eax+78h] (AddForce on debris)
}

// A constant read through an x87 memory operand (fmul/fcomp dword [const]). The instruction is
// pointed at a scaled copy, so other code sharing the constant is unaffected.
// PerTick: x 60/rate. OneTick (force applied once): x rate/60. PerTickSquared: x (60/rate)^2.
enum Scale { PerTick, OneTick, PerTickSquared };
struct FloatOperand { DWORD insn; u8 op0, op1; DWORD constAddr; float orig; Scale scale; const char* what; };

static const FloatOperand g_floatOperands[] = {
	// Dragging doors, drawers, levers, valves: force from the per-tick mouse delta
	{ 0x004AAB4B, 0xD8, 0x0D, 0x00675B5C,  100.0f,     OneTick, "move: yaw drag force" },
	{ 0x004AABE7, 0xD8, 0x0D, 0x0067CFEC, -100.0f,     OneTick, "move: pitch drag force" },
	{ 0x004AABDA, 0xD8, 0x0D, 0x0067CFF0,  -80.0f,     OneTick, "move: pitch drag forward force" },
	// Dragging: camera turns every tick while the grab point is past the screen edge
	{ 0x004AA891, 0xD8, 0x0D, 0x0067CFE8,  1.0f/800,   PerTick, "move: edge turn left" },
	{ 0x004AA8CC, 0xD8, 0x0D, 0x0067CFE4, -1.0f/800,   PerTick, "move: edge turn right" },
	{ 0x004AA91F, 0xD8, 0x0D, 0x0067CFE0,  1.0f/600,   PerTick, "move: edge turn up" },
	{ 0x004AA95A, 0xD8, 0x0D, 0x0067CFDC, -1.0f/600,   PerTick, "move: edge turn down" },
	// Pushing: one-tick kick that gets a resting box moving, and the right-click shoves
	{ 0x004ACD8D, 0xD8, 0x0D, 0x006731A8,  0.6f,       OneTick, "push: start kick" },
	{ 0x004ACFAE, 0xD8, 0x0D, 0x00673084,  1.2f,       OneTick, "pull: start kick" },
	{ 0x004AB22F, 0xD8, 0x0D, 0x0067D05C,  233.0f,     OneTick, "push: shove x" },
	{ 0x004AB238, 0xD8, 0x0D, 0x0067D05C,  233.0f,     OneTick, "push: shove y" },
	{ 0x004AB245, 0xD8, 0x0D, 0x0067D05C,  233.0f,     OneTick, "push: shove z" },
	{ 0x004AA9F9, 0xD8, 0x0D, 0x00673220,  500.0f,     OneTick, "move: fling x" },
	{ 0x004AAA04, 0xD8, 0x0D, 0x00673220,  500.0f,     OneTick, "move: fling y" },
	{ 0x004AAA12, 0xD8, 0x0D, 0x00673220,  500.0f,     OneTick, "move: fling z" },
	// Hiding in shadow: "standing still" means moving less than 0.01 per tick
	{ 0x004A9B8B, 0xD8, 0x1D, 0x006795D4,  0.01f,      PerTick, "hidden: still x" },
	{ 0x004A9BA2, 0xD8, 0x1D, 0x006795D4,  0.01f,      PerTick, "hidden: still y" },
	{ 0x004A9BB9, 0xD8, 0x1D, 0x006795D4,  0.01f,      PerTick, "hidden: still z" },
	// Hammer/pickaxe: a swing is a mouse flick faster than 0.015 per tick
	{ 0x0045E8A9, 0xD8, 0x1D, 0x006796D4,  0.015f,     PerTick, "melee: flick threshold" },
	{ 0x0045E8DC, 0xD8, 0x1D, 0x006796D4,  0.015f,     PerTick, "melee: flick threshold" },
	{ 0x0045EA9F, 0xD8, 0x1D, 0x006796D4,  0.015f,     PerTick, "melee: flick threshold" },
	{ 0x0045E8C0, 0xD8, 0x1D, 0x006796D0, -0.015f,     PerTick, "melee: flick threshold" },
	{ 0x0045EABD, 0xD8, 0x1D, 0x006796D0, -0.015f,     PerTick, "melee: flick threshold" },
	// Worm: segments only follow when the head moved more than this (squared) per tick
	{ 0x00428467, 0xD8, 0x1D, 0x00675128,  1e-5f,      PerTickSquared, "worm: root moved" },
	{ 0x00428849, 0xD8, 0x1D, 0x00675128,  1e-5f,      PerTickSquared, "worm: segment moved" },
};

// A tick count stored as an immediate: imm points at the 32-bit (or 8-bit) value, after the
// instruction bytes in "code" which are checked first.
struct TickImmediate { DWORD insn; u8 code[8]; int codeLen; int size; int orig; const char* what; };

static const TickImmediate g_tickImmediates[] = {
	// mov [esi+268h], 25: grace period after leaving the ground (moving, jumping, footsteps)
	{ 0x0049FCA3, { 0xC7, 0x86, 0x68, 0x02, 0x00, 0x00 }, 6, 4, 25, "player: ground grace ticks" },
	// mov [eax+120h], 6: camera position smoothing window (inlined SetCameraSmoothPosNum)
	{ 0x0049D144, { 0xC7, 0x80, 0x20, 0x01, 0x00, 0x00 }, 6, 4, 6,  "player: camera smoothing ticks" },
	// cPlayerHands constructor: lag of the held item behind the camera
	{ 0x004A4B11, { 0xC7, 0x46, 0x44 }, 3, 4, 16, "hands: rotation smoothing ticks" },
	{ 0x004A4B0A, { 0xC7, 0x46, 0x40 }, 3, 4, 3,  "hands: position smoothing ticks" },
	// cmp [esi+3Ch], 0Eh: intro camera pan smoothing window
	{ 0x0046978F, { 0x83, 0x7E, 0x3C }, 3, 1, 14, "intro: pan smoothing ticks" },
	// Enemy path following: distance samples kept to decide a node was passed
	{ 0x004071A4, { 0xC7, 0x86, 0x78, 0x02, 0x00, 0x00 }, 6, 4, 150, "enemy: path node samples" },
	// Enemy mesh position smoothing (inlined SetEntitySmoothPosNum)
	{ 0x00418E97, { 0xC7, 0x86, 0xB4, 0x01, 0x00, 0x00 }, 6, 4, 10, "enemy: mesh smoothing ticks" },
	// mov ebp, 20: worm segment position history (how its body trails the head)
	{ 0x00428BF5, { 0xBD }, 1, 4, 20, "worm: segment history ticks" },
	// Dragging doors/drawers: mov [esi+58h], 20 on every mouse/move input; each idle mouse
	// axis counts it down, and at 0 the dragged object is braked to a stop
	{ 0x004AAAA4, { 0xC7, 0x46, 0x58 }, 3, 4, 20, "move: brake delay (forward)" },
	{ 0x004AAB04, { 0xC7, 0x46, 0x58 }, 3, 4, 20, "move: brake delay (sideways)" },
	{ 0x004AAB7D, { 0xC7, 0x46, 0x58 }, 3, 4, 20, "move: brake delay (yaw)" },
	{ 0x004AAC55, { 0xC7, 0x46, 0x58 }, 3, 4, 20, "move: brake delay (pitch)" },
};

// A float pushed as an immediate.
struct FloatImmediate { DWORD insn; u8 op; float orig; Scale scale; const char* what; };

static const FloatImmediate g_floatImmediates[] = {
	// Sound triggers that enemies hear live for "one tick" (1/60 s)
	{ 0x0049CA00, 0x68, 1.0f/60, PerTick, "footstep sound trigger life" },
	{ 0x0048E047, 0x68, 1.0f/60, PerTick, "world sound trigger life" },
};

static float g_scaledFloats[64];	// targets for redirected operands

static int g_rate = 60;
static float g_ticksPer60th = 1.0f;	// rate / 60

static int ScaleTicks(int n)
{
	int s = (int)(n * g_ticksPer60th + 0.5f);
	return s < 1 ? 1 : s;
}

//////////////////////////////////////////////////////////////////////////
// Logic rate

static double g_tickMs;

// Set the live timer's ms-per-tick the first time anyone asks for the step size. That happens
// during startup, before the first tick, so the schedule and every step size (including ones
// the game caches at startup) follow.
static float __fastcall Hook_GetStepSize(void* timer)
{
	double* tickMs = (double*)((u8*)timer + 8);
	if(*tickMs != g_tickMs) *tickMs = g_tickMs;
	return (float)(*tickMs * 0.001);
}

int ChooseLogicRate()
{
	int rate = CfgInt("LogicRate", 0);
	if(rate == 0)
	{
		DEVMODEA dm = {};
		dm.dmSize = sizeof(dm);
		if(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) rate = (int)dm.dmDisplayFrequency;
	}
	if(rate < 60) rate = 60;
	if(rate > 360) rate = 360;
	return rate;
}

//////////////////////////////////////////////////////////////////////////
// Level scripts
//
// Map scripts can't see the step size. Every map and terminal script has an OnUpdate that the
// game runs once per tick ("This function is run 60 times a sec"); a few of them were written
// for exactly that. Script forces (AddBodyForce) also last a single tick. Their text is
// adjusted when it is loaded, before the script compiler sees it.

struct ScriptEdit { const char* file; std::string find, replace; int count; };
static std::vector<ScriptEdit> g_scriptEdits;

static std::string Num(const char* fmt, double v)
{
	char buf[64];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, v);
	return buf;
}

static void BuildScriptEdits()
{
	const double k = 60.0 / g_rate;	// per-tick amounts
	auto PerTick = [&](double v) { return Num("%.6ff", v * k); };
	auto OneTick = [&](double v) { return Num("%.1ff", v / k); };	// one-tick forces
	auto Ticks = [&](int n) { return Num("%.0f", (double)ScaleTicks(n)); };
	std::string rate = Num("%.0f", (double)g_rate);

	g_scriptEdits = {
		// Cell 1: the cord snaps when its speed jumps by more than 0.4 between two ticks
		{ "level01_cells.hps", "fDiff < -0.4f || fDiff > 0.4f)",
		  "fDiff < -" + PerTick(0.4) + " || fDiff > " + PerTick(0.4) + ")", 1 },
		// Mess hall: once-a-second check
		{ "level04_messhall.hps", "glCount % 60 == 0)", "glCount % " + rate + " == 0)", 1 },
		// Infirmary: the head tears off after 10 ticks of pulling
		{ "level11_infirmary.hps", "lForceCount == 10)", "lForceCount == " + Ticks(10) + ")", 1 },
		// Chemical lab: the robot arm moves a fixed step per tick
		{ "level19_chemical_laboratory.hps", "gfDepthVal += 0.004f;", "gfDepthVal += " + PerTick(0.004) + ";", 1 },
		{ "level19_chemical_laboratory.hps", "gfDepthVal -= 0.004f;", "gfDepthVal -= " + PerTick(0.004) + ";", 1 },
		{ "level19_chemical_laboratory.hps", "gfHoriVal += 0.004f;", "gfHoriVal += " + PerTick(0.004) + ";", 1 },
		{ "level19_chemical_laboratory.hps", "gfHoriVal -= 0.004f;", "gfHoriVal -= " + PerTick(0.004) + ";", 1 },
		{ "level19_chemical_laboratory.hps", "gfVertVal += 0.003f;", "gfVertVal += " + PerTick(0.003) + ";", 1 },
		{ "level19_chemical_laboratory.hps", "gfVertVal -= 0.003f;", "gfVertVal -= " + PerTick(0.003) + ";", 1 },
		// Terminals: typewriter text, one character every 4 / 2 ticks
		{ "18_terminal_gui.hps", "glCount == 4)", "glCount == " + Ticks(4) + ")", 1 },
		{ "19_terminal_gui.hps", "glCount == 2)", "glCount == " + Ticks(2) + ")", 1 },

		// One-off pushes (one tick of force)
		{ "level00_tutorial.hps", "AddBodyForce(\"shelf01\",\"world\",-20000,10000, 0);",
		  "AddBodyForce(\"shelf01\",\"world\"," + OneTick(-20000) + "," + OneTick(10000) + ", 0);", 1 },
		{ "level12_mental_realm.hps", "AddBodyForce(\"wndoor03_doorShape1\",\"World\",-4000,1000,1000);",
		  "AddBodyForce(\"wndoor03_doorShape1\",\"World\"," + OneTick(-4000) + "," + OneTick(1000) + "," + OneTick(1000) + ");", 1 },
		{ "level12_mental_realm.hps", "AddBodyForce(\"wndoor03_doorShape2\",\"World\",-4000,1000,-1000);",
		  "AddBodyForce(\"wndoor03_doorShape2\",\"World\"," + OneTick(-4000) + "," + OneTick(1000) + "," + OneTick(-1000) + ");", 1 },
		{ "level15_outside.hps", "AddBodyForce(\"icebridge\",\"World\",-20000,-5000,20000);",
		  "AddBodyForce(\"icebridge\",\"World\"," + OneTick(-20000) + "," + OneTick(-5000) + "," + OneTick(20000) + ");", 1 },
		{ "level19_chemical_laboratory.hps", "AddBodyForce(\"beam01_beamShape\", \"world\", 0, 0, 60000);",
		  "AddBodyForce(\"beam01_beamShape\", \"world\", 0, 0, " + OneTick(60000) + ");", 1 },
		{ "level19_chemical_laboratory.hps", "AddBodyForce(\"beam01_beamShape\", \"world\", 0, 0, 40000);",
		  "AddBodyForce(\"beam01_beamShape\", \"world\", 0, 0, " + OneTick(40000) + ");", 1 },
		{ "level19_chemical_laboratory.hps", "AddBodyForce(\"beam01_beamShape\", \"world\", 0, 0, -40000);",
		  "AddBodyForce(\"beam01_beamShape\", \"world\", 0, 0, " + OneTick(-40000) + ");", 1 },
	};
}

static const char* StdStringCStr(const void* s)
{
	// VC7.1 std::string: 16-byte inline buffer at +4, capacity at +0x18
	const u8* p = (const u8*)s;
	return *(const DWORD*)(p + 0x18) >= 16 ? *(const char* const*)(p + 4) : (const char*)(p + 4);
}

static bool EndsWithNoCase(const char* s, const char* suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && _stricmp(s + n - m, suffix) == 0 && (n == m || s[n - m - 1] == '/' || s[n - m - 1] == '\\');
}

typedef char* (__fastcall *LoadCharBuffer_t)(void*, void*, const void*, int*);
typedef void* (__cdecl *GameNewArray_t)(size_t);
typedef void (__cdecl *GameDeleteArray_t)(void*);

static char* __fastcall Call_LoadScript(void* script, void* edx, const void* name, int* len)
{
	char* buf = ((LoadCharBuffer_t)H::LoadCharBuffer)(script, edx, name, len);
	if(!buf) return buf;

	const char* file = StdStringCStr(name);
	std::string text;
	bool changed = false;
	for(ScriptEdit& e : g_scriptEdits)
	{
		if(!EndsWithNoCase(file, e.file)) continue;
		if(!changed) { text.assign(buf, *len); changed = true; }
		int found = 0;
		for(size_t at = text.find(e.find); at != std::string::npos; at = text.find(e.find, at + e.replace.size()))
		{
			text.replace(at, e.find.size(), e.replace);
			++found;
		}
		if(found != e.count) Log("  script %s: expected %d of '%s', found %d", e.file, e.count, e.find.c_str(), found);
	}
	if(!changed) return buf;

	// The game frees this buffer with its own delete[], so allocate it with the game's new[].
	HMODULE crt = GetModuleHandleA("MSVCR71.dll");
	GameNewArray_t gameNew = crt ? (GameNewArray_t)GetProcAddress(crt, "??_U@YAPAXI@Z") : NULL;
	GameDeleteArray_t gameDelete = crt ? (GameDeleteArray_t)GetProcAddress(crt, "??_V@YAXPAX@Z") : NULL;
	if(!gameNew || !gameDelete) { Log("  script %s left unchanged (no game allocator)", file); return buf; }

	char* out = (char*)gameNew(text.size());
	memcpy(out, text.data(), text.size());
	gameDelete(buf);
	*len = (int)text.size();
	Log("  adjusted %s for %d Hz", file, g_rate);
	return out;
}

//////////////////////////////////////////////////////////////////////////
// Engine

// Character "don't slide on gentle slopes" test: the vertical speed it compares is built from
// one tick of gravity, so the threshold scales with the step.
static double g_slopeThreshold = -0.15;

// Stair step-up probe: looks ahead by this tick's movement. Keep the 60 Hz reach so enemies
// (one probe ray) still find stairs they approach at an angle.
__declspec(naked) static void Stub_StepProbe()
{
	__asm
	{
		fsqrt
		fmul g_ticksPer60th
		fstp dword ptr [esp+28h]	// [esp+24h] in the caller, plus our return address
		ret
	}
}

// Jump take-off is a force applied for one tick; scale it so the take-off speed stays the same.
// Replaces "mov edx, [edi+80h]" between a cmp and its jne, so only flag-neutral instructions.
static float g_jumpForce;
__declspec(naked) static void Stub_JumpForce()
{
	__asm
	{
		fld dword ptr [edi+80h]
		fmul g_ticksPer60th
		fstp g_jumpForce
		mov edx, g_jumpForce
		ret
	}
}

// One-tick character forces (knockback, spider leap): pass a scaled copy.
typedef void (__fastcall *CharAddForce_t)(void*, void*, const float*);
static void __fastcall Call_CharForceOneTick(void* body, void* edx, const float* f)
{
	float v[3] = { f[0] * g_ticksPer60th, f[1] * g_ticksPer60th, f[2] * g_ticksPer60th };
	((CharAddForce_t)H::CharAddForce)(body, edx, v);
}

// One-tick torque: replaces "call [edx+80h]" (6 bytes). Swaps the pushed vector pointer for a
// scaled copy and tail-jumps into AddTorque, which returns to the game and pops the pointer.
static float g_torque[3];
__declspec(naked) static void Stub_TorqueOneTick()
{
	__asm
	{
		push eax
		mov eax, [esp+8]			// the game's pushed &torque
		fld dword ptr [eax]
		fmul g_ticksPer60th
		fstp g_torque
		fld dword ptr [eax+4]
		fmul g_ticksPer60th
		fstp g_torque+4
		fld dword ptr [eax+8]
		fmul g_ticksPer60th
		fstp g_torque+8
		mov dword ptr [esp+8], offset g_torque
		pop eax
		jmp dword ptr [edx+80h]
	}
}

// Explosion push on objects: replaces "mov ecx, esi; fstp [esp+44h]" just before the AddForce
// call. The vector is at the game's [esp+40h..48h] and its pointer at the game's [esp].
static float g_explosionForce[3];
__declspec(naked) static void Stub_ExplosionForce()
{
	__asm
	{
		fstp dword ptr [esp+48h]	// the replaced store, +4 for our return address
		fld dword ptr [esp+44h]
		fmul g_ticksPer60th
		fstp g_explosionForce
		fld dword ptr [esp+48h]
		fmul g_ticksPer60th
		fstp g_explosionForce+4
		fld dword ptr [esp+4Ch]
		fmul g_ticksPer60th
		fstp g_explosionForce+8
		mov dword ptr [esp+4], offset g_explosionForce
		mov ecx, esi
		ret
	}
}

// Debris from broken objects: replaces "fstp [esp+3Ch]; call [eax+78h]" (7 bytes). The vector
// is at the game's [esp+38h..40h], its pointer at the game's [esp].
static float g_breakForce[3];
__declspec(naked) static void Stub_BreakPieceForce()
{
	__asm
	{
		fstp dword ptr [esp+40h]	// the replaced store, +4 for our return address
		fld dword ptr [esp+3Ch]
		fmul g_ticksPer60th
		fstp g_breakForce
		fld dword ptr [esp+40h]
		fmul g_ticksPer60th
		fstp g_breakForce+4
		fld dword ptr [esp+44h]
		fmul g_ticksPer60th
		fstp g_breakForce+8
		mov dword ptr [esp+4], offset g_breakForce
		jmp dword ptr [eax+78h]
	}
}

// Props freeze after N physics steps below a speed threshold; keep N in 60 Hz steps.
typedef void (__cdecl *SetFreezeTreshold_t)(void*, float, float, int);
static SetFreezeTreshold_t g_setFreezeTreshold;

static void __cdecl Hook_SetFreezeTreshold(void* body, float speed2, float omega2, int frames)
{
	g_setFreezeTreshold(body, speed2, omega2, ScaleTicks(frames));
}

//////////////////////////////////////////////////////////////////////////

static bool ExpectFloatNear(DWORD addr, float want, const char* what)
{
	float have = *(const float*)addr;
	float tol = (want < 0 ? -want : want) * 1e-5f;
	if(have >= want - tol && have <= want + tol) return true;
	Log("  mismatch at %08X (%s): %g", addr, what, have);
	return false;
}

static float ScaleFor(Scale s)
{
	float k = 1.0f / g_ticksPer60th;
	return s == OneTick ? g_ticksPer60th : s == PerTick ? k : k * k;
}

static bool VerifyTables()
{
	bool ok = true;
	for(const FloatOperand& o : g_floatOperands)
	{
		u8 op[2] = { o.op0, o.op1 };
		ok &= Expect(o.insn, op, 2, o.what) && ExpectDword(o.insn + 2, o.constAddr, o.what)
			&& ExpectFloatNear(o.constAddr, o.orig, o.what);
	}
	for(const TickImmediate& t : g_tickImmediates)
	{
		int have = t.size == 4 ? *(const int*)(t.insn + t.codeLen) : *(const char*)(t.insn + t.codeLen);
		bool same = Expect(t.insn, t.code, t.codeLen, t.what);
		if(same && have != t.orig) { Log("  mismatch at %08X (%s): %d", t.insn, t.what, have); same = false; }
		ok &= same;
	}
	for(const FloatImmediate& f : g_floatImmediates)
		ok &= Expect(f.insn, &f.op, 1, f.what) && ExpectFloatNear(f.insn + 1, f.orig, f.what);
	return ok;
}

static void ApplyTables()
{
	int n = 0;
	for(const FloatOperand& o : g_floatOperands)
	{
		float* slot = &g_scaledFloats[n++];
		*slot = *(const float*)o.constAddr * ScaleFor(o.scale);
		WriteDword(o.insn + 2, (DWORD)slot);
	}
	for(const TickImmediate& t : g_tickImmediates)
	{
		int v = ScaleTicks(t.orig);
		if(t.size == 4) WriteDword(t.insn + t.codeLen, (DWORD)v);
		else { char b = (char)(v > 127 ? 127 : v); Write(t.insn + t.codeLen, &b, 1); }
	}
	for(const FloatImmediate& f : g_floatImmediates)
	{
		float v = f.orig * ScaleFor(f.scale);
		Write(f.insn + 1, &v, 4);
	}
}

bool VerifyHighFramerate()
{
	bool ok = VerifyTables();
	static const u8 stepSize[] = { 0xDD, 0x41, 0x08, 0xD8, 0x0D, 0xE4, 0x2D, 0x67, 0x00, 0xC3 };
	static const u8 jmp = 0xE9;
	ok &= Expect(H::TimerGetStepSize, stepSize, sizeof(stepSize), "cLogicTimer::GetStepSize");
	ok &= Expect(H::GameGetStepSize + 3, &jmp, 1, "cGame::GetStepSize tail jump")
		&& ExpectDword(H::GameGetStepSize + 4, H::TimerGetStepSize - (H::GameGetStepSize + 8), "cGame::GetStepSize target");

	ok &= ExpectCall(H::ScriptLoadCall, H::LoadCharBuffer, "script load call");

	static const u8 slope[] = { 0xDC, 0x1D };
	ok &= Expect(H::SlopeCompare, slope, 2, "slope compare") && ExpectDword(H::SlopeCompare + 2, H::SlopeConst, "slope constant");
	const double slopeOrig = -0.15;
	ok &= Expect(H::SlopeConst, &slopeOrig, sizeof(slopeOrig), "slope constant value");
	static const u8 probe[] = { 0xD9, 0xFA, 0xD9, 0x5C, 0x24, 0x24 };
	ok &= Expect(H::StepProbe, probe, sizeof(probe), "step probe");
	static const u8 doorPid[] = { 0x6A, 0x14, 0x8B, 0xCE, 0x89, 0x5E, 0x40 };
	ok &= Expect(H::DoorPidSize, doorPid, sizeof(doorPid), "door PID size") &&
		ExpectCall(H::DoorPidSize + 7, H::SetPidIntegralSize, "door SetPidIntegralSize");

	HMODULE newton = GetModuleHandleA("Newton.dll");
	FARPROC setFreeze = newton ? GetProcAddress(newton, "NewtonBodySetFreezeTreshold") : NULL;
	ok &= setFreeze && ExpectDword(H::IatSetFreezeTreshold, (DWORD)setFreeze, "Newton freeze import");

	static const u8 jumpRead[] = { 0x8B, 0x97, 0x80, 0x00, 0x00, 0x00 };
	ok &= Expect(H::JumpForceRead, jumpRead, sizeof(jumpRead), "jump force read");

	for(DWORD c : H::OneTickCharForceCalls) ok &= ExpectCall(c, H::CharAddForce, "one-tick character force");
	static const u8 torqueCall[] = { 0xFF, 0x92, 0x80, 0x00, 0x00, 0x00 };
	for(DWORD c : H::OneTickTorqueCalls) ok &= Expect(c, torqueCall, sizeof(torqueCall), "one-tick torque");
	static const u8 explosion[] = { 0x8B, 0xCE, 0xD9, 0x5C, 0x24, 0x44, 0xFF, 0x52, 0x78 };
	ok &= Expect(H::ExplosionForce, explosion, sizeof(explosion), "explosion force");
	static const u8 breakPiece[] = { 0xD9, 0x5C, 0x24, 0x3C, 0xFF, 0x50, 0x78 };
	ok &= Expect(H::BreakPieceForce, breakPiece, sizeof(breakPiece), "debris force");
	return ok;
}

void ApplyHighFramerate(int rate)
{
	g_rate = rate;
	g_ticksPer60th = rate / 60.0f;
	g_tickMs = 1000.0 / rate;

	BuildScriptEdits();
	RedirectCall(H::ScriptLoadCall, &Call_LoadScript);

	g_slopeThreshold = -0.15 * 60.0 / rate;
	WriteDword(H::SlopeCompare + 2, (DWORD)&g_slopeThreshold);

	u8 call[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
	*(DWORD*)(call + 1) = (DWORD)&Stub_StepProbe - (H::StepProbe + 5);
	Write(H::StepProbe, call, sizeof(call));

	u8 doorPid = (u8)ScaleTicks(20);
	Write(H::DoorPidSize + 1, &doorPid, 1);

	g_setFreezeTreshold = *(SetFreezeTreshold_t*)H::IatSetFreezeTreshold;
	WriteDword(H::IatSetFreezeTreshold, (DWORD)&Hook_SetFreezeTreshold);

	*(DWORD*)(call + 1) = (DWORD)&Stub_JumpForce - (H::JumpForceRead + 5);
	Write(H::JumpForceRead, call, sizeof(call));

	for(DWORD c : H::OneTickCharForceCalls) RedirectCall(c, &Call_CharForceOneTick);
	for(DWORD c : H::OneTickTorqueCalls)
	{
		*(DWORD*)(call + 1) = (DWORD)&Stub_TorqueOneTick - (c + 5);
		Write(c, call, sizeof(call));
	}
	*(DWORD*)(call + 1) = (DWORD)&Stub_ExplosionForce - (H::ExplosionForce + 5);
	Write(H::ExplosionForce, call, sizeof(call));
	u8 call7[7] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
	*(DWORD*)(call7 + 1) = (DWORD)&Stub_BreakPieceForce - (H::BreakPieceForce + 5);
	Write(H::BreakPieceForce, call7, sizeof(call7));

	ApplyTables();

	// Last: switch the timer itself.
	u8 jmp[5] = { 0xE9 };
	*(DWORD*)(jmp + 1) = (DWORD)&Hook_GetStepSize - (H::TimerGetStepSize + 5);
	Write(H::TimerGetStepSize, jmp, sizeof(jmp));
	Log("  game logic runs at %d Hz", rate);
}
