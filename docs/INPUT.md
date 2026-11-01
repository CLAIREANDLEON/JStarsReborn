# Input (0.4.0)

The game imports `scePadInit`, `scePadOpen`, `scePadClose`, `scePadReadState`, `scePadSetVibration` and `sceMouseInit`
(`jstars_input.c`). One pad (handle 1) exists. `scePadReadState` builds the console pad state from the first source that
applies, in this order, and the log names the source whenever it changes:

1. **Scripted test input** (`--pad-script file`) — automated test input, never a controller. Every transition is logged as
   `pad: SCRIPTED TEST INPUT buttons -> ...`. Script format (seconds count from the first `scePadOpen`):
   `at <s> press CROSS[+BTN] [for ms]`, `at <s> stick left|right x y [for ms]`, `every <ms> from <s> to <s> press BTN [for ms]`.
2. **XInput controller** (first connected pad). Button/stick/trigger mapping is the usual one (A = CROSS, B = CIRCLE,
   X = SQUARE, Y = TRIANGLE, Start = OPTIONS, Back = TOUCHPAD, bumpers L1/R1, triggers L2/R2, stick clicks L3/R3, D-pad).
   Rumble goes to the XInput motors.
3. **Keyboard**, only while the game window has focus (`--no-keyboard` disables it).

| Key | Console input | Key | Console input |
|---|---|---|---|
| Enter, Space | CROSS | Arrow keys | D-pad |
| Backspace | CIRCLE | W A S D | left stick |
| F | SQUARE | I J K L, or numpad 8 / 4 / 5 / 6 | right stick |
| R | TRIANGLE | Q / E | L1 / R1 |
| P or Esc | OPTIONS | Left Shift / Left Ctrl | L2 / R2 |
| T | TOUCHPAD | V / B | L3 / R3 |

`--pad-none` disables every source (the game then shows its own "connect a controller" screen).

## What has and has not been verified
* Scripted input: verified on Windows — the title screen accepts it and the game's save-data prompt answers it (runs
  20261101-045720, -050054, -050445).
* XInput controller (0.4.0, launcher sessions on the owner's PC): in session 20261101-144715 a controller was picked up
  while the game was running (`pad: input source is now XInput controller` at 217 s) and about 700 live button transitions
  were logged; the game's own flows answered them (title left, save data created at 220 s, menus, a Free Battle played to
  its result screen). Session 20261101-150628 started with the controller already connected. **The owner's verdict on how
  it plays is pending**; this project does not claim it.
* Keyboard (0.4.0): presses reach the runtime (`pad: live input buttons -> CROSS` with `input source is now keyboard`), and
  short taps are latched so a tap between two game frames is still seen. In the two attempts at the title screen the game
  did not leave the title; controller presses in non-accepting title phases were ignored the same way (session
  20261101-150628, 52-57 s), so this is not shown to be a keyboard problem, but **keyboard play has not been seen to work
  end to end**. When an XInput controller is connected, it is the only source and the keyboard is ignored.
* Live input reaches the game only while its window is in the foreground; on focus loss the pad reports a neutral state
  (logged as `pad: game window lost focus: input held neutral`), and on focus gain earlier key presses are discarded.
* Motion sensors, touchpad coordinates and orientation are neutral (the game does not import them).
