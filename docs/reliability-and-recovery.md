# Reliability and recovery

## Saves

The memory cards, the history of previous saves and restoring one are in `saves.md`. From the player's side: **F1 → Game → Previous saves** lists them, and **Restore and restart** queues one, saves the settings and starts the game again.

## Controllers

The game has four controller ports. Each is filled from one player's bindings (`mkdd/src/input_map.hpp`), applied to that player's controller and, where the player has keys bound, the keyboard and mouse. The game's side of it (the SDK's PAD library) is in `input.md`.

**Which controller plays which port.** Every controller SDL lists is opened. A port's controller is, by its setting (`[player1]` to `[player4]`, `controller`):

- *Automatic* (the default): the controllers nobody named take the automatic ports in the order each first had a button pressed. A controller nobody touches takes no port.
- *A named controller:* that device, whenever it is connected. It is excluded from every automatic port, and no two players can name the same one (the settings file is refused if they do).
- *None:* no controller.

A controller's identity is its GUID with its serial number where it has one, otherwise its GUID and name, and a count among identical ones. Two identical pads without serial numbers can't be told apart once both have been unplugged: they are then taken in the order they come back, and **Reset automatic player order** in the menu starts the order again.

**Unplugging.** A controller that goes keeps its place: the other players do not move up. The game sees that port's controller go, as on the console, and come back when the same controller does. While another window has the focus nothing is held and rumble stops.

**Which ports are connected.** The first always, as a console with a controller in its first port: the game can always be started and played from the keyboard. Another port is connected while its controller is, or when its player has a key or the mouse bound (the keyboard then plays there too: two players can share it), or when a script plays it.

**The defaults** (`default_controls` in `mkdd/src/settings.cpp`; README, "Controls"). A pad's face buttons go by position, not by letter: the GameCube's A is the large button under the thumb (the pad's bottom button, A on an Xbox pad), B is below and to its left (the left button, X), X to its right (the right button, B), Y above (the top button, Y). Z is either shoulder button, L and R the triggers, Start is Start, the D-pad, the left stick (the control stick) and the right stick (the C-stick) are themselves. The keyboard, for the first player: arrows for the stick, X Z C S for A B X Y, D for Z, A and Space for L and R, Enter for Start, T G F H for the D-pad, I K J L for the C-stick.

**Analogue.** A stick or trigger bound to the stick's directions, the C-stick's or L and R is analogue; a key or a button there presses all the way, and a stick's direction or a trigger bound to a button presses it past a threshold, with the release five points below the press so a noisy axis can't chatter. The sticks have one round dead zone (`stick_deadzone`); past it the game is given a stick that starts where the SDK's own dead zone ends (15 of its units) and reaches 100 at the edge, so there is one dead zone, the player's, not two. A trigger runs from the SDK's dead zone (30) to 220, and L and R click (their digital press) where `trigger_threshold` says; a key clicks at once.

**The menu.** While it is open every port reads as its controller at rest and still connected, and rumble is off. After it closes nothing reaches the game until every control is let go, so the press that closed it isn't played.

**Rumble** is per player, with one strength for all. SDL's rumble is asked for a quarter of a second at a time and renewed while it is on: a program that stops, stops the rumble.

**Scripts.** `MKDD_INPUT` (and `MKDD_INPUT2` to `MKDD_INPUT4`) play a port in runs without a player: `Start@12,A@14.5,Stick:-100:0@20+3,R@20+3` (a button, a stick position or a trigger value, from a time in seconds of the game's own frames, for 0.2 s or the seconds after `+`).

## Updates and checks

The launchers check the disc with `qilin-disc` before building (the game's id and revision, and the SHA-256 of its `main.dol`), take `main.dol` from it, and remember a changed disc only after a successful build. The Windows kit's build stamp includes hashes of the kit archive, the recompiler, the disc reader, the plan and the flags, so an update using the same version number still rebuilds. Executable copies are staged; replacing an existing Windows executable keeps its previous copy in `Double Dash.exe.previous`.

Ordinary builds compile and link `mkdd_runtime_check` with empty stand-ins for the generated game functions. It is a build check, not a playable game. CI checks all runtime sources on each platform and runs the graphical tests on Linux under Xvfb and Mesa, where `QILIN_REQUIRE_GRAPHICS=1` turns absent graphics into a failure and lets the test that opens a window run. Elsewhere that test runs only with `QILIN_WINDOW_TESTS=1`, and machines without a Vulkan 1.3 device skip the offscreen ones. Audio tests use SDL's dummy device with real callbacks while producers change rates and volume. These checks need no disc.

## Reports

**F9** captures the next four pictures shown (or, with the menu open, the paused one) as PNG at the window's size, with the log so far, the settings and what the program knows of the machine, into one tar in `captures/` in the data folder; the place is shown on screen and logged. The log alone is `mkdd.log` there (`mkdd.old.log`: the run before).
