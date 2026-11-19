# HaloScale

Resolution independent pickup halos for Manhunt (PC, Steam).

## The problem

A pickup halo is three things drawn on top of each other: the model, a corona
sprite, and a wire sphere. The wire sphere is generated at runtime and handed to
RenderWare as a `rwPRIMTYPELINELIST`.

D3D8 rasterises line primitives exactly one pixel wide no matter how large the
backbuffer is. On a 640x448 PS2 framebuffer that is the thickness the effect was
authored for. At 1920x1080 it is still one pixel, on a framebuffer 2.4x taller, so
the wireframe covers less than half the relative screen area it should. The sphere
is also blended additively at alpha 120, so the loss of coverage costs brightness
as well as thickness, and the dark blue weapon-class spheres are the first ones to
become almost invisible.

The radius is *not* the problem - it is world space (`size * 0.42`) and scales
correctly at any resolution. Only the wire fails to scale.

PCSX2 does not show the issue because upscaling scales line rasterisation along
with the 640x448 geometry, which keeps the ratio intact.

## The fix

The plugin hooks the single `RwIm3DTransform` call inside the wire sphere draw and
replaces the line list with camera facing quads whose width is constant in *pixels*:

	unitsPerPixel = (2 * camera->viewWindow.y) / screenHeight
	halfWidth     = 0.5 * lineWidth * unitsPerPixel * viewDepth

Width is computed per line endpoint, so it stays correct under perspective. Reading
`viewWindow` live from the camera means it follows the widescreen fix automatically -
MHWSF is Hor+ and only widens `viewWindow.x`, leaving the vertical mapping intact.

Below roughly PS2 resolution the plugin leaves the original line list alone, so
640x480 renders exactly as it always did.

## Soft glow (`bGlow`, on by default)

With the wire scaling alone the PC matches the PS2 on measurements - at matched
sphere size it lands within a few percent on both wire coverage and peak channel
brightness. It still doesn't quite *look* the same, because an upscaling emulator
filters the PS2's lines into a soft falloff while the PC draws them hard-edged.
Equal light, distributed differently, and a soft line reads as brighter than a
sharp one carrying the same energy.

`bGlow` draws each wire twice: the sharp quad, plus a wider dimmer copy underneath
(`fGlowWidth`, `fGlowAlpha`). Additive blending sums them, reproducing that falloff
rather than faking it with brute thickness.

It also fixes a problem the scaling can't. Additive blending pays each colour by
its luminance contribution, so the classes divide sharply:

| class | colour | luminance |
|---|---|---|
| painkillers | white | 1.00 |
| lures | yellow | 0.93 |
| quest items | cyan | ~0.78 |
| green class | green | 0.72 |
| ammo | orange | ~0.68 |
| red class | red | 0.21 |
| blue class | blue | 0.07 |

Blue and red are one and two orders below the rest, so at identical alpha they read
far weaker no matter how thick the wire is. The glow lifts both without needing
per-colour special cases.

`fBlueAlphaScale` exists as a targeted alternative - extra alpha on blue-dominant
spheres only, leaving every other class at PS2 parity. It is off (1.0) by default.
The detection requires blue to exceed both red and green by 20, which was verified
against all seven real classes: cyan measures (40,67,57) and fails it, white is
neutral and fails it, and only blue at b-r = b-g = 33 qualifies.

## The halo palette

Recorded by running the plugin with `bLog=1` and logging every colour that reached
the renderer, then locating each one in the executable. Nine exist, eight appear in
normal play:

| RGB | class | source |
|---|---|---|
| (80, 0, 0) | red weapon | weapon class table at `0x6728E4`, index 1 |
| (0, 0, 80) | blue weapon | class table, index 2 |
| (0, 80, 0) | green weapon | class table, index 3 |
| (80, 80, 0) | yellow lure | class table, index 4 |
| (20, 70, 50) | teal quest item | class table, index 5 |
| (60, 30, 0) | orange ammo | entity types `0x66`-`0x6B` |
| (80, 80, 80) | white painkillers | branch before the switch |
| (20, 80, 120) | cyan save tape | separate function at `0x4D27DD` |
| (20, 70, 30) | dark green - never observed | script command, `0x435CD5` |

The ninth is placed by a level script rather than by entity type. It is created at
`0x435950`, reached only through a script dispatcher, and differs from every other
halo in two ways: the position takes a +0.3 offset on Y so it floats above a point
rather than centring on an object, and the draw sits behind a virtual call that
must return zero. Two script opcodes reach it, 232 and 233; 233 appears in none of
the 24 shipped level scripts, and 232 could not be confirmed either way without
decoding the `.mls` bytecode properly.

Note how low the values are - nothing exceeds 120. The spheres are blended
additively, so these are light contributions rather than surface colours, which is
also why blue (7% of perceived luminance) and red (21%) read so much weaker than
yellow (93%) at identical alpha.

## Compatibility

Sits alongside `MHP.asi` rather than replacing it. MHP's `bFixHalos` is what makes
the sphere visible in the first place (it clears the corona texture before the
sphere is drawn); this plugin only changes how thick the wires are. The one call
site it patches, `0x5FBF50`, is not touched by MHP or by the widescreen fix.

Requires the same unpacked `manhunt.exe` that MHP targets. The plugin verifies the
image base and the instruction at the patch site before touching anything, and
silently does nothing if either does not match.

## Building

Run `build.cmd`. No RenderWare or DirectX SDK is needed - the handful of engine
structures and functions it uses are declared directly, with offsets verified
against the game binary.

## Installing

This repo is source only, with no prebuilt binary. Build it first (see Building).

1. You need an ASI loader in the game folder (for example Ultimate ASI Loader). If
   other `.asi` plugins already work for you, you have one.
2. Requires [MHP](https://github.com/Fire-Head/MHP) by Fire_Head with
   `bFixHalos=1` (MHP's default).
3. Copy the built `bin\HaloScale.asi` and `bin\HaloScale.ini` into the game's
   `scripts\` folder, next to `MHP.asi`.

It patches the game in memory only. `manhunt.exe` is never modified. To uninstall,
delete the two files.
