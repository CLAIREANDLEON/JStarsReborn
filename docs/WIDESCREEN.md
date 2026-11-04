# Widescreen / ultrawide (Hor+) and borderless fullscreen

Runtime switches (the launcher's Settings page sets them): `--fullscreen` and `--widescreen`. Both are off by default.

## Fullscreen
`--fullscreen` opens the game window borderless over the whole monitor it starts on. Alt+Enter or F11 switches between
fullscreen and the window in either mode. The window size setting is used for windowed mode.

## How the widescreen patch works
The game builds every perspective projection in one routine, `0x3ec310` (CUSA01593 v01.02), with the field of view in degrees,
a horizontal scale, the aspect as height/width, and the near and far planes:

    m00 = cot(fov / 2) / scale        m11 = cot(fov / 2) / (h / w)

The field of view (60 degrees for the normal cameras) is therefore **horizontal**: on a wider screen the unchanged game would
either be pillarboxed (what the presenter did before) or, if simply stretched, show everything too wide.

With `--widescreen` the runtime puts a small cave in front of the routine. For 16:9 projections (h/w = 0.5625; shadow maps use
1.0 and 4:3 views 0.75, both are left alone) it multiplies the horizontal scale by a factor *k* that the presenter publishes for
every frame:

    k = (window width / window height) / (frame width / frame height), at least 1, at most 4

The vertical view is unchanged and the horizontal view grows (Hor+): at 3440x1440 the horizontal field of view goes from 60 to
about 75.6 degrees. The presenter then stretches the game's 16:9 frame over the whole window, which restores correct proportions.
When the window is not wider than 16:9, *k* is 1 and the picture is the original one, letterboxed as before.

The change is applied in memory only, after every original byte (the routine's prologue, the empty cave area at `0x730140`, the
segment layout, no overlapping relocation) has been checked; on any other build nothing is changed and the log says so.

`tools/re/widescreen_check.py <prepared/boot-linked.bin>` runs the game's own routine natively (x86-64 Linux, gcc) with and
without the cave bytes taken from `runtime/jstars_loader.c` and checks the matrices.

## Known limitations
- The 2D HUD, menus and movies are drawn into the same 16:9 frame, so they are stretched horizontally. Keeping them at 16:9
  needs the HUD draws identified and placed separately (as the Bloodborne PC port's HUD layout tool does); not done yet.
- The game still renders 1920x1080; on a 3440x1440 monitor the frame is scaled up. A higher internal resolution and FSR
  upscaling are separate work.
- Objects at the far left and right edges may appear late if the game culls them with its own 60-degree view. Please report it.
