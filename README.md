# Penumbra: Black Plague ultrawide + high framerate fix

Makes the Steam version of Penumbra: Black Plague (build 459397) look right on widescreen and
ultrawide displays such as 3440x1440, and run its game logic at your monitor's refresh rate.
It is a port of the [Penumbra: Overture unofficial remaster](https://github.com/Nik0OneShot/penumbra-overture-unofficial-remaster),
so it fixes the same things in the same way.

## What it fixes

The 3D view already handles wide screens: it keeps a 70° vertical FOV and shows more at the
sides. Everything 2D, though, is laid out on a fixed 800x600 canvas and stretched to fill the
screen, about 1.8× too wide at 21:9. With the fix installed:

- Menus, HUD, text, inventory, notebook, keypads, terminals and loading screens keep their 4:3
  shape, centred, with black bars at the sides.
- Full-screen effects (fades, flashes, damage, the sanity noise, letterbox bars) still cover
  the whole screen.
- The in-world cursor (grabbing, using items) still lines up with what you point at.
- Horizontal mouse speed matches vertical again. At 21:9 it was about 1.8× slower, so it
  will feel faster horizontally than before.
- Options > Graphics offers modern resolutions, including your desktop resolution. Without
  the fix the list stops at 1920x1200.
- Things at the far left and right of a wide view no longer turn black (stencil-shadow method
  chosen from a box that only covered a 4:3 view, rooms behind doorways just off screen).
  On very wide screens (about 2.6:1 and up, e.g. 32:9), fog culling no longer hides everything.
- **Environmental Audio crash (a bug in the original game):** with Environmental Audio
  turned on, the game could crash at start-up or on exit, depending on leftover memory
  contents. Its effect slots were created before threading was switched on, so they never
  got the lock the sound thread then uses. They now always get one.
- **High framerate:** the game's logic (player, camera, physics, scripts) normally updates
  60 times a second, and frames are only drawn after an update, so it never looks smoother
  than 60 FPS. The fix runs the logic at your monitor's refresh rate (or the `LogicRate` you
  set), so motion is genuinely smooth at 120/144/165+ Hz.

## High framerate

Running the logic faster isn't enough on its own. Some code was written for exactly 60 updates
a second: it counts updates, or applies a push for a single update. The fix compensates each of
these so the game plays as it does at 60 Hz:

- **Player:** jump take-off; dragging doors, drawers, levers and valves (and how soon a dragged
  door brakes after you stop moving the mouse); push/pull kicks and shoves; the screen-edge
  turn while dragging; the ground grace period; the "standing still" check for hiding in
  shadow; camera and hand smoothing; slope sliding.
- **Combat and objects:** knockback from enemy hits and explosions; explosion pushes on
  objects; the spider's leap; the hammer/pickaxe flick threshold; spin on hit and thrown
  objects and flares; debris from broken objects and doors.
- **Enemies:** path following; stair stepping; mesh smoothing; how the worm's body trails its
  head; how long footstep sounds stay audible to them.
- **Physics:** how long props take to fall asleep after coming to rest; swing-door braking;
  the controllers on valves, wheels, levers, drawers and doors (their resistance and the
  pull that drives them open or shut remember the last fraction of a second, which was
  counted in updates); turning a held object.
- **Level scripts:** the cell's electrical cord snapping (level 1), the flare check in the mess
  hall, the living head in the infirmary, the robot arm in the chemical lab, the typewriter text
  on the terminals in levels 18 and 19, and the one-off pushes on the tutorial shelf, the worm's
  door, the ice bridge and the lab barricade. These are adjusted as the scripts load; the files
  on disk are untouched.
- **Machine room gate:** the rod you jam into the gate's lock only breaks it when turned to
  its limit, but the padlock's collision box stops it a few degrees short. At 60 Hz contacts
  are soft enough for the rod to sink in and reach the limit; at higher rates they are
  stiffer, so the lock now breaks once the rod is most of the way there.

The rate is set by `LogicRate` in `PenumbraUltrawide.ini`: `0` matches your monitor (default),
and `60` gives the original behaviour. With "Limit FPS" on (the default), the game draws one
frame per update, so the framerate equals the logic rate.

If anything feels off, set `LogicRate=60` to compare.

## Requirements

- Windows, and the **Steam** version of Penumbra: Black Plague. Other versions (GOG, retail
  discs) have different code, so the fix detects them and does nothing.
- Requiem (`Requiem.exe`, in the same folder) is a different program; the fix detects it and
  leaves it alone.
- No game files are included or modified. The fix only adds a DLL next to the game.

## Install

Download this repository and extract it. `dist\` holds the prebuilt DLL. Close the game, then
from the extracted folder run:

```bash
powershell -ExecutionPolicy Bypass -File install.ps1
```

The script finds the game through your Steam libraries (pass `-GameDir` to point it
somewhere else). It renames the game's `SDL_image.dll` to `SDL_image_orig.dll` and puts the
fix in its place. Then set the resolution in Options > Graphics, or edit
`Documents\Penumbra\Black Plague\settings.cfg` (`<Screen Width="3440" Height="1440" .../>`).

Each launch writes `PenumbraUltrawide.log` next to `Penumbra.exe`, saying whether the patches
applied. If the game crashes, the log also records where (module, registers and call stack),
which is worth including in a bug report. `PenumbraUltrawide.ini` there turns each part on or off.

## Uninstall

```bash
powershell -ExecutionPolicy Bypass -File uninstall.ps1
```

Steam's "Verify integrity of game files" also puts the original DLL back, which removes the
fix. Run `install.ps1` again afterwards if you want it back.

## How it works

`Penumbra.exe` is wrapped in Steam DRM, so its code is encrypted on disk and can't be patched
there. Instead the fix ships as a stand-in `SDL_image.dll` that forwards everything to the real
one, and patches the decrypted code in memory at the game's first `SDL_Init` call (hooked
through the import table in `DllMain`), before any game object is built.

Black Plague shares its engine and much of its game code with Overture, so every patch from the
Overture fix has a Black Plague counterpart; the addresses differ and some fields moved (the
crosshair state, for one). Black Plague's own additions (the Infected, the terminal minigame,
the new level scripts) were audited for 60 Hz assumptions as well. Before patching, the DLL
checks the exact bytes at every patch site; on any other build it logs the mismatch and leaves
the game untouched (and keeps 60 Hz if only a high-framerate site differs).

Engine reference: Frictional's open-source releases of the previous game and the engine
([PenumbraOverture](https://github.com/FrictionalGames/PenumbraOverture),
[HPL1Engine](https://github.com/FrictionalGames/HPL1Engine)). Black Plague's own game code was
never released, so its sites were found in a memory snapshot of the running game.

## Building and testing

- `src\build.bat` builds `dist\SDL_image.dll` with the Visual Studio 2022 Build Tools (x86).
- `tests\run_test.bat <dump> <work dir> [game dir]` maps a memory snapshot of the running game
  at its real address, lets the DLL patch it, and drives every hook with fake objects
  (projection, overlay stretching, cursor mapping, mouse scaling, resolution list, logic rate,
  script rewrites, tick compensations, culling). The game dir (default `game\redist`) supplies
  the real `SDL_image.dll`, its dependencies and the level scripts. The snapshot holds the
  game's code, so it isn't kept in this repo. Make one with
  `python -I tests\dumpmem.py bp_mem.bin` (read-only) while the game sits at the main menu.

## Credits

Not affiliated with Frictional Games. Penumbra: Black Plague and its files belong to their owners.
