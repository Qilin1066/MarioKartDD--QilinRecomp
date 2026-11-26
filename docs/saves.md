# Saves: the memory cards (`card.cpp`, `save_history.cpp`)

The game saves to a GameCube memory card through the SDK's CARD library. That
library is replaced at its API (`mkdd/src/card.cpp`); nothing below it (the
card's unlock sequence, its flash commands, the EXI bus) exists here.

## The cards

A card is a folder in the game's data folder (`host/paths.hpp`):

- `card-a/`: slot A. Made at the first start, so slot A always holds a card.
- `card-b/`: slot B. Empty unless this folder exists; make it to put a second
  card in, delete it to take the card out (the game is told, as when a card is
  pulled).

Each is a **251-block card**: 16 megabits in 8 KiB sectors, five of them the
card's own, at most 127 files. The game's card code compares the sector size
with 8 KiB, which every size of Nintendo's own cards has; its file takes 3
blocks and each ghost 5, so the smallest card (59 blocks) would hold the save
and eleven ghosts, and this one the save and forty-nine.

## The files

Each file on the card is one host file in the standard `.gci` form: the
64-byte directory entry as the card stores it, then the file's blocks.

| offset | bytes | field |
|---|---|---|
| 0x00 | 4 + 2 | game code, maker code (`GM4E`, `01`) |
| 0x06 | 1 | 0xFF |
| 0x07 | 1 | banner format |
| 0x08 | 32 | file name, zero-padded |
| 0x28 | 4 | time of the last change, seconds from 2000 |
| 0x2C | 4 | where the banner and icons start in the data |
| 0x30 | 2 + 2 | icon formats, icon speeds |
| 0x34 | 1 + 1 | permissions (4: public), copy count |
| 0x36 | 2 + 2 | first block, number of blocks |
| 0x3A | 2 | 0xFFFF |
| 0x3C | 4 | where the comment starts in the data |

All big-endian. So a save here is already what an emulator's card manager
imports and exports: copy `card-a/GM4E01-MarioKart Double Dash!!.gci` out to
use it elsewhere, or drop a `.gci` from elsewhere into the folder (it is on
the card from the next time the game mounts it). The host file's name is the
game code, the maker code and the card file's name, with bytes a host file
name can't hold written `%XX`; it is only a label, and the entry inside says
what the file is. A `.gci` whose size doesn't match its entry is left alone
and logged; so is one that would not fit on the card.

The game's files (USA disc, read from its card code and messages):

- `MarioKart Double Dash!!`: 3 blocks (0x6000 bytes), created at the first
  save. The game asks for "1 file and 3 empty blocks".
- ghosts: 5 blocks each (0xA000 bytes), one file per ghost. A ghost's name is its 20-byte record (course,
  time, characters, kart, initials) in a uuencode-like form: `5`, then 28 characters from `!` to `` ` ``, then
  `aa`: for example ``5"QA!*%&"11!!!O)TG#`+'5>*2)M!aa``, on the host
  ``GM4E01-5%22QA!%2A%25&%2211!!!O)TG#`+'5%3E%2A2)M!aa.gci`` (41,024 bytes).

What the game does with the card, as seen in scripted runs:

- At every start it mounts the card, checks it, asks the free blocks and looks for its file; nothing is shown.
- At the title, Start does the same. Without the file it asks "Would you like to create data for MARIO KART:
  DOUBLE DASH!! on the Memory Card in Slot A?" (YES selected); A creates and writes it ("Data has been created.").
  With the file it reads it and goes on.
- After a time trial with a new record it writes the file ("Saving game data."); the save history takes the copy
  from before.
- Time Trials lists the ghosts by asking the status of all 127 entries. SAVE GHOST after a run creates the
  ghost's file (new files have no earlier copy to keep).
- A probe (`CARDProbeEx`) comes every frame from the game's card thread; a card busy with anything but mounting
  still answers it, as the SDK's does.
- The game's own file is only ever looked for in slot A. Ghosts are listed from both slots: LOAD GHOST DATA
  shows each card with its ghosts and the room left (48 more on a card holding the save and one ghost, 49 on one
  with a ghost alone), and a ghost on card B loads like one on A.
- A loaded ghost replays exactly: its kart's positions matched those of the run that recorded it, in another
  process, to the tenth of a unit over all 11,530 frames of a race.

When things go wrong, the game's own messages appear:

- The host refuses a write (a full disk, seen by accident): the call gives IOERROR, the save on the card stays as
  it was, and the game says "An error may have occurred during saving."
- The card in slot A is gone (its folder deleted) when the game wants it: "No Memory Card found in Slot A.
  Continue without saving game data?", then "Insert a Memory Card into Slot A and press the A Button." Making the
  folder again puts the card back.
- Card B pulled while its ghosts are listed: the game returns to the slot screen with slot B empty, without a
  message.

Other games' files in the folder count against the free blocks and are
visible as the SDK shows them: readable by number if they are public, never
writable.

## What a call returns, and when

The results are the SDK's (`CARD_RESULT_*`): READY 0, BUSY -1, NOCARD -3,
NOFILE -4, IOERROR -5, EXIST -7, NOENT -8 (no free entry), INSSPACE -9,
NOPERM -10, LIMIT -11 (past the file's end), NAMETOOLONG -12, FATAL_ERROR
-128 (a bad argument). A host error (the disk full, the folder read-only) is
IOERROR, with the reason in the log; the game shows its own message for it.

The card takes time, and so do the calls (figures chosen as plausible for a
card's flash memory, not measured on one): mounting 300 ms, reading 1 ms plus
5 ms per sector, writing 150 ms per sector written and per system sector a
change rewrites (creating or deleting a file two, a write or a status change
one, formatting five). The game uses the synchronous calls from a thread of
its own: that thread sleeps for the time. The asynchronous forms return at
once and call back as an interrupt; the card answers BUSY meanwhile.

## Safe writes

Every change to a file writes the whole new file beside the old one
(`<name>.gci.tmp`), flushes it to the disk, and swaps it in with one rename.
A crash or power cut at any point leaves the old save whole; the leftover
copy is deleted, with a line in the log, the next time the card is mounted.

## Save history

Before the first change to a file after it is opened, and before a delete or
a format, the file as it was is copied to
`save-backups/card-a/<file>.gci.history/<time>.bak`: the five most recent
per file. A save the game writes in several calls is one entry. If the copy
can't be made the write fails (IOERROR) and the save stays as it was.

**F1 → Game → Previous saves** lists them with the card, the file and the
time; **Restore and restart** writes `restore-save.txt`, and the next start
puts that copy back before the game runs, keeping the one it replaces in the
history too. Only files inside `card-a/` and `card-b/` can be restored over.
A backup is itself a `.gci` under another name.

## Tests

`mkdd_card_test` runs the replacements over synthetic guest memory and a
temporary folder: results and free blocks, the status's banner and icon
offsets, permissions, the time each call takes, the order asynchronous calls
finish in, a write cut short, a write whose backup fails, delete, restore,
format, a card pulled out. `mkdd_save_history_test` covers the history alone.
