# Input: controllers, audio RAM, and the ports with nothing in them (`pad.cpp`, `ar.cpp`, `exi.cpp`)

## Controllers

The game reads controllers through the SDK's PAD library, which sits on the
serial interface (SI). Both are replaced at their API (`mkdd/src/pad.cpp`);
there is no serial hardware. The game's own layer above (JSystem's
JUTGamePad) is lifted and unchanged, as are the SDK's stick and trigger
clamps (`PADClamp`, `PADClampCircle`), which are plain arithmetic.

Each of the four ports holds a standard controller while the host has one for
it. The host's side is one structure per port (`mkdd/src/input.hpp`): whether
a controller is there, its buttons as the SDK's `PAD_BUTTON_*` bits, both
sticks as -128..127, both triggers as 0..255. Which pad or keys feed which
port, and scripted input for tests, are the host's business.

`PADRead` fills the SDK's 12-byte status per port: buttons (only the twelve a
controller has), stick, C stick, triggers, the pressure of A and B (always 0:
a standard controller has no sensors there), and an error. `PADSetAnalogMode`
is honoured: the game asks for mode 3, where both sticks and both triggers
have eight bits; the other modes drop the low four bits of the fields they
send short, as the controller does.

A port follows the SDK's states, which is how controllers come and go:

- A port starts disabled. `PADInit`, `PADReset` and `PADRecalibrate` reset the
  ports they name, one after the other, about a millisecond each (an
  estimate). While a port resets it reads as `PAD_ERR_NOT_READY` (-2).
- When its reset ends, a port with a controller is enabled. One without stays
  disabled and reads as `PAD_ERR_NO_CONTROLLER` (-1), which makes the game
  reset it again: a controller plugged in later is found on that next try.
- A controller that goes away is disabled by the read that misses it, and its
  rumble is stopped.

`PADRead` returns the ports whose controllers can rumble (every enabled one);
`PADControlMotor` passes rumble on and off to the host for an enabled port.
`SIProbe` says what a port holds: a standard controller (0x09000000) or no
response (0x08).

## The steering wheel

The SDK's Logitech wheel library is linked in, and JUTGamePad opens a wheel
on any port where `SIProbe` reports one (0x08000000). None ever does. The SI
calls under that library are replaced to say so: a reset answers "no
controller" through its callback, a read returns the same error, and the
library's sampling callback runs twice a field while a controller is being
polled, as on the console, and finds nothing open.

## Audio RAM

The 16 MB of ARAM behind the DSP hold the game's second file store (JKRAram,
lifted) and its sound data. The AR library is replaced (`mkdd/src/ar.cpp`):
`ARInit`, `ARAlloc` (a stack of blocks from 0x4000), `ARGetSize`,
`ARRegisterDMACallback` and `ARStartDMA`. ARQ, the queue of transfers above
it, is plain code and stays lifted.

A transfer is a copy between main memory and the ARAM block, made when it
starts; its end is reported by the registered callback, run as an interrupt
after the time the hardware takes: 32 bytes in 246 cycles of the 486 MHz
processor (about 63 MB a second, the rate Dolphin's authors measured), so a
4 KiB piece of ARQ's takes 65 microseconds. Addresses and lengths are in
32-byte units, as the hardware's registers hold them. The block itself
belongs to the audio layer, whose DSP reads samples from it.

## The expansion ports

The game offers LAN play through the Broadband Adapter in serial port 1, and
looks for it once, at start-up (`EXIGetType`, which stays lifted, over
`EXIGetID`, which is replaced in `mkdd/src/exi.cpp`). The port answers as a
bare connector, so the game finds no adapter and never starts its network
code. A memory card slot answers with its card's size, or not at all.

Nothing else reaches the expansion interface: the memory cards are replaced
above it (see [saves.md](saves.md)), and the clock chip and the debug serial
port are the OS layer's. A call that did reach it would touch the bus's
registers, which nothing answers; the runtime stops there and names the
register.

## Runs without a player (development)

For following and driving the game in `--headless` and `--offscreen` runs (`later.cpp`, `pilot.cpp`); all hexadecimal
addresses are the supported build's.

- `MKDD_DEVICES_LOG=1`: every memory card call with its result, controller resets and rumble, wheel resets asked,
  ARAM transfers in sum.
- `MKDD_WATCH=<address>[><offset>...][:1|2|4],...`: logs each value when it changes, with the game time. Each `>`
  follows the pointer read so far. Useful ones: `803CBDA0` AppMgr's application (4 logo, 12 movie, 10 the menus'
  SequenceApp, 8 a race), `803C9CE0` SequenceApp's scene (0 title, 1 menu, 3 course select, 7 ghost data),
  `803CBDB0>C>15C` the title's state (1 PRESS START, 3 card check, 4 its menu), `803CBDB0>C>20E0` the menu's
  (1 players, 4 VS/co-op, 7 mode, 13 characters), `803CBDDC` CardAgent's state (0 idle, 4 a YES/NO prompt).
- `MKDD_PILOT_STEPS=<steps>`: plays port 1 by those values instead of the clock, since menus come up seconds
  earlier or later from run to run and a race ends when it ends (`MKDD_INPUT` is then ignored for port 1; ports 2
  to 4 keep their scripts). `value=n:presses;...`, taken in order; `pilot.cpp` has the whole syntax.
- `MKDD_PILOT=<file>` steers port 1's kart along a path of positions, backing out when stuck;
  `MKDD_PILOT_RECORD=<file>:1` writes such a path from one of the game's own drivers in a Grand Prix.

With these a run goes from boot through the title and menus into a Grand Prix (one player, two in one kart, two
or four on a split screen) and through a whole time trial to its saved ghost.

## Tests

`mkdd_pad_test`: the reset protocol with controllers coming and going, the
status bytes in each analogue mode, rumble, and the wheel calls.
`mkdd_ar_test`: the allocator, transfers both ways and when their callback
comes, and the expansion ports' answers.
