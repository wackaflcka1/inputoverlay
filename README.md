# 255 Input Overlay

Shows your mouse trail, APM counter and keys on stream. One small Windows exe, no installer, no internet connections: it only reads your mouse and keys to draw them on screen and saves nothing except its own settings file.

Download the ready-made exe from **Releases**, or build it yourself below.

## Build it yourself

It is plain C against the Windows API (Direct3D 11 for drawing). No other libraries.

**On Windows (MSYS2):**
1. Install [MSYS2](https://www.msys2.org).
2. Open the "MSYS2 UCRT64" terminal and run `pacman -S mingw-w64-ucrt-x86_64-gcc`.
3. In that terminal, `cd` into this folder and run:
   ```
   mkdir -p out
   windres src/app.rc -O coff -o out/app.res.o
   gcc -O2 -std=c11 -municode -mwindows -static -s -o "out/Input Overlay.exe" src/main.c src/render.c src/demo.c src/apm.c out/app.res.o -luser32 -lgdi32 -lshell32 -lcomctl32 -lcomdlg32 -ldwmapi -lwinmm -ld3d11 -ldxgi -ldxguid
   ```
   (`build.bat` runs the same commands if gcc is on your PATH.)

**On Linux or macOS (cross-compile with zig):**
```
pip install ziglang==0.13.0
sh build.sh
```
The exe ends up in `out/`. Your own build will not be byte-for-byte identical to the release exe, since compilers differ slightly; that is normal.

## User guide

```
255 INPUT OVERLAY  v1.9.7
=======================

Shows your mouse and keys on stream:
  - a trail that follows your mouse, and marks where you click
  - an APM counter (actions per minute: clicks plus key presses)
  - an on-screen keyboard and mouse that light up as you press them
  - optional glow, colours, fonts and sizes for all of it

One file, no installer. Windows 10 or 11, 64-bit.


GETTING STARTED
---------------
1. Put the "Input Overlay" folder anywhere and double-click
   "Input Overlay.exe" in it.
2. If Windows says "Windows protected your PC", click "More info", then
   "Run anyway". (The program is not code-signed.)
3. Settings opens every time you start the program, in front, on the screen
   it was last on. The program also sits in the tray, near the clock (it may
   be under the ^ arrow). Starting the program again while it is already
   running brings the existing Settings window to the front.
     Click the tray icon        open Settings
     Right-click the tray icon  show/hide, switch mode, move things, reset
                                APM, exit
4. Show / hide: Ctrl+Shift+F10 (change it in Settings > General).
5. To close the program completely, click "Exit program" at the bottom of
   Settings, or the X at the top right (or press Alt+F4 there, or
   right-click the tray icon > Exit program). Settings stay open, and
   changes save automatically. The minimise button hides the Settings
   window; click the tray icon to show it again. The gold "Done" button at
   the bottom right locks placements if you were moving things (the same as
   "Done moving"); it does not close Settings. Esc does nothing in Settings.

Updating from an older version? Your settings are in input-overlay.ini next
to the old exe. Copy that file into the "Input Overlay" folder to keep them.
The exe is now called "Input Overlay.exe", so in Streamlabs / OBS open your
overlay source once and pick the window again: it now shows as
"[Input Overlay.exe]: 255 Input Overlay ...".


SETTINGS
--------
Every option has a short line under it saying what it does. Changes show in
the preview right away and are saved automatically.

Styles: under the preview, one click sets a whole look. You can change
anything after.
  Clean dark   dark keys on a dark plate, dark APM box. The default.
  Outline      white outlines and letters with a thin dark edge, no boxes.
  Broadcast    navy and gold, slanted letters, accent bar on the APM box.
  Light        light keys and box with dark text.
  Slanted      black leaning keys with a thin white edge and big narrow
               letters. Pressed keys turn white.

  Trail      the mouse line: colours, thickness, length, smoothing
             (Off to Very high). Smoothing makes the whole line flow
             instead of looking jagged.
  Clicks     which buttons get a mark, colours, shape, size
  APM text   the number and the label next to it (size, colour, bold,
             border, right of or under the number)
  APM box    Background (Invisible, Dark box or Custom colour), size,
             corner and position. Invisible shows only the number, with a
             thin outline. "Scale with screen size" keeps the same look on
             1080p, 1440p and 4K.
  Keys       on-screen keyboard and mouse: which keys (League, Left side,
             WASD, Full or My keys), Background (Invisible, Dark plate or
             Custom colour), key shape (Rounded or Slanted), colours, size,
             outline thickness, letter font, 3D keys, corner and position.
             Invisible shows only the keys, with nothing behind them (it
             also turns off 3D key depth; you can turn 3D keys back on, but
             that adds a solid edge). The mouse can sit
             next to the keyboard or have its own spot ("Keep mouse next
             to keyboard").
             "Edit my keys" opens a key editor: click keys on the keyboard
             to add or remove them (F-keys, arrows, Home/End and the mouse
             side buttons M4/M5 too), drag them on the layout (they snap to
             half-key steps and close up against the next key), right-click
             a key for its width or to remove it. "Start from" loads League,
             Left side, WASD or Full to change from; Undo steps back.
  Glow       soft light around the trail, clicks and number
  General    on top of game, green screen or stream window (screen to
             show, edges, background colour, window size, and for the
             stream window whether it is see-through or black on your
             screen), hide when idle, redraws per second, shortcut, mouse
             position check

To move the APM box, the keyboard or the mouse, click "Move on screen" and
drag them.
Click "Done moving", or "Done" at the bottom of Settings, when finished.
"Done" only locks placements; Settings stays open.

If the trail does not sit exactly on your mouse, turn on General > "Check
position". It shows a crosshair and some numbers at the mouse, also in the
green screen and stream window, so you can see it on stream; a screenshot
of it shows what is going on. The "Move" and "Stretch" sliders under it can
correct a fixed offset.


SHOWING IT ON STREAM (OBS or Streamlabs)
----------------------------------------
There are two ways. Try the stream window first; if it shows black or
nothing in your streaming app, use the green screen.

Stream window (see-through, best quality)
  The program draws into its own window with real see-through parts, so the
  edges are exactly as smooth as in the preview. No filters needed.
  1. Settings > General > Show as: "Stream window". Leave Window size on
     "Same as screen".
  2. Streamlabs: + (Add Source) > "Game Capture" > Add.
     OBS: + > "Game Capture".
  3. In the source settings:
       Mode                 Capture specific window
       Window               [Input Overlay.exe]: 255 Input Overlay
                            (stream window)
       Allow Transparency   ticked (this is the important part)
  4. Put this source above your game source and stretch it over the whole
     canvas so it lines up with the game.
  The stream window can sit behind the game, but do not minimise it.
  With two screens: Settings > General > "Screen to show" picks the screen
  to show. "Game screen" (the default) finds the screen League is on by
  itself. Screen 1 and Screen 2 are listed with their size.
  "Stream window on my screen" (See-through by default) tries to leave your
  desktop visible around the drawing. The window has its own taskbar button
  and title "255 Input Overlay (stream window)", so you can find it and pick
  it in Streamlabs or OBS. Clicking that taskbar button shows a dashed
  outline of the window for a moment.
  While Settings is open, "Show where the stream window is" (on by default)
  draws that same dashed outline and a small corner label on your screen.
  The outline is only for you: it is not captured on stream. It hides when
  you minimise Settings. If a capture app needs a solid window, or
  see-through is not available, pick Black. Then the window looks black on
  your screen where the capture is see-through; that is normal.
  If the source stays black: check that Allow Transparency is ticked, try
  Black, or switch to the green screen below.

Green screen
  1. Settings > General > Show as: "Green screen". Leave Edges on "Smooth",
     Background on "Green" and Window size on "Same as screen".
  2. Add a Window Capture source and pick
     "[Input Overlay.exe]: 255 Input Overlay (green screen)".
     In OBS set Capture Method to "Windows 10 (1903 and up)".
  3. Right-click the source > Filters > + > "Chroma Key" (not Color Key):
       Key Color Type               Green
       Similarity                   400
       Smoothness                   80
       Key Color Spill Reduction    100  (lower it if bright colours look
                                          washed out, for example cyan)
       Opacity 100, Contrast 0, Brightness 0, Gamma 0
  4. Stretch the source over the whole canvas so it lines up with the game.
     "Same as screen" makes the window as big as your screen (usually the
     same as your canvas), so OBS does not have to enlarge it. Enlarging is
     what makes edges look blocky.
  The green screen window can sit behind the game, but do not minimise it.
  With two screens, leave General > "Screen to show" on "Game screen" (it
  finds the screen League is on), or pick the screen by its size. Leave
  Window size on "Same as screen": the window then always matches that
  screen exactly, also after you change screens, so the trail lines up
  with your mouse.
  To check that your streaming app shows the whole window, turn on General >
  "Corner marks". Four corner marks and a cross in the middle appear. On
  stream they must sit exactly in the corners and the middle of the source.
  If one is cut off or sits inside, right-click the source > Transform >
  "Reset Transform", then "Fit to screen". Turn the marks off when done.
  Avoid green in your own colours (the hint turns orange if a colour matches).

  The window has no frame or title bar, so the capture is exactly the
  drawing (no white edge). Drag anywhere inside it to move it.

Showing the League client (lobby) on stream
  If your stream shows the League client from its own Window Capture
  stretched over the canvas, the client and the screen do not line up. Then:
  1. Settings > General > Screen to show: "Follow League client".
  2. Streamlabs: + (Add Source) > "Window Capture" > pick
     "[LeagueClientUx.exe]: League of Legends". Right-click it >
     Transform > "Stretch to screen".
  3. Your green screen (or stream window) source goes above it, also
     right-click > Transform > "Stretch to screen".
  While the client is open, only the client window is mapped onto the whole
  green window, so the trail lines up with the stretched client. Outside the
  client no trail is drawn. When a game starts, it shows the game's whole
  screen again. "Check position" shows which one is used.

  Edges "Hard" is the old look for the "Color Key" filter: every pixel is
  either fully the background colour or fully opaque.
  Background "Magenta" is for setups that use green themselves.

  Cleanest of all: set Keys and APM box Background to Dark plate / Dark box
  or Custom colour, set the green screen Background to "Black" and use the
  Window Capture with no key filter.
  Add the window twice and crop one copy to the keys and the other to the
  APM box (Alt + drag the edges), so the black does not cover the game.

On top of game
  Draws over your screen so you can see it while playing. Use a Display
  Capture source to show it on stream (Game Capture will not include it).


FULLSCREEN, BORDERLESS AND SPEED
--------------------------------
- League's "Full Screen" mode gives the whole screen to the game, so Windows
  can hide anything drawn on top of it, including this overlay. For "On top
  of game", set League to "Borderless". It looks the same and the overlay
  stays on top.
- The green screen and the stream window are captured separately, so they
  work with League in any mode, Full Screen included.
- The program only redraws when something changes (the mouse moves, a key
  or button is pressed, the APM changes). When nothing happens it sleeps.
- General > "Redraws per second" sets the most it will ever redraw. 60 matches
  stream video and uses the least CPU. Pick 120, 144 or "Screen" if you want
  the overlay on your own high refresh rate screen to be just as smooth.
- If you change resolution or monitor, it adjusts by itself within a second.


SETTINGS FILE
-------------
Saved as input-overlay.ini in the same folder as the exe. You can edit it by
hand while the program runs; it reloads by itself.


SAFE TO USE
-----------
- Doesn't touch the game. It never opens, reads or changes League or any
  other program. "Game screen" and "Follow League client" only ask Windows
  where the League window is on screen, by its name.
- Doesn't send anything online. There is no internet code at all.
- Key presses are only counted and shown live on the keys display. Which keys
  you press is never saved or sent anywhere.
- It uses the same normal Windows input that other stream overlays use.
  Nobody outside Riot can promise what Vanguard allows, but the program
  only reads normal Windows input and never touches the game.


BUILDING IT YOURSELF
--------------------
The full source is in the source folder. Run build.sh with zig 0.13
(pip install ziglang==0.13.0), or build the files in source/src with any
mingw-w64 gcc.
```

## License

MIT, see [LICENSE](LICENSE).
