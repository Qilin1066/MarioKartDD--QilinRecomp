Mario Kart: Double Dash!! QilinRecomp for Windows (64-bit)
==========================================================

Mario Kart: Double Dash!! (Nintendo, GameCube, 2003), recompiled to run
natively: the game's own program translated into C++ and compiled for your
PC, drawn with Vulkan.

IN DEVELOPMENT: the game does not run yet. See the project's README for
what works.

No game data is included. You need your own copy of the game: the USA
release, disc GM4E01, revision 0. A disc image works as it is (.iso, .gcm
or .ciso), and so does a folder extracted from one (the folder holding
"sys" and "files"). Compressed images don't: convert an .rvz or .gcz to
ISO with Dolphin first (right-click the game, Convert File).


Playing
-------
1. Unpack this folder anywhere (it needs about 2 GB once built).
2. Double-click "Play Double Dash.bat".
   - The first time, it asks for your disc (the image, or sys\main.dol of
     an extracted disc), then builds the game from it: a few minutes (up
     to 15 on a slower PC). Your game files never leave your PC.
   - After that, it starts the game straight away. Double Dash.exe in this
     folder can be started directly too, with the disc as its argument.
   - To use a different disc: Play Double Dash.bat -ChooseDisc

If Windows says "Windows protected your PC": More info, then Run anyway.
(The download isn't signed.) Or before unpacking, right-click the zip,
Properties, tick Unblock, OK.

Needs: Windows 10 or 11, 64-bit, and a graphics driver with Vulkan 1.3
(any recent NVIDIA, AMD or Intel driver).


Controls
--------
Four GameCube controllers. Controllers take the ports in the order each
first has a button pressed; the keyboard plays the first.

  GameCube controller   Xbox controller      Keyboard (player 1)
  A                     A (bottom)           X
  B                     X (left)             Z
  X                     B (right)            C
  Y                     Y (top)              S
  Z                     RB or LB             D
  L, R                  LT, RT               A, Space
  Start                 Start                Enter
  Control stick         left stick           arrows
  C-stick               right stick          I K J L
  D-pad                 D-pad                T G F H

Back + Start together, or F1 or Esc: the settings menu (the game pauses):
controls for each player (set by pressing them), the picture (size, render
resolution, SMAA, FSR 1, a CRT look), full screen, sound.
F3: frame times. F9: capture frames for a bug report. Alt+Enter or F11:
full screen.

Settings, saves and captures are kept in %LOCALAPPDATA%\qilin\mkdd.


What's in here
--------------
Play Double Dash.bat  builds (once) and starts the game
kit\qilin-disc.exe  reads your disc: says what it is, takes main.dol from it
kit\qilin.exe       the recompiler: turns the disc's main.dol into C++
kit\lib, include    the prebuilt rest of the game: its runtime, the renderer,
                    sound, the window and controls (SDL3), the menu (Dear ImGui)
kit\plan.tsv        which parts of the game are recompiled, and which run natively
kit\toolchain       LLVM-MinGW, a C++ compiler for Windows (Clang)
kit\ninja.exe       Ninja, which drives the compiler
work\               made when building: the recompiled code (from your disc)
LICENSES\           the licences of everything included

Source, changes and issues: https://github.com/Qilin1066/MarioKartDD--QilinRecomp
