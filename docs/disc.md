# The disc: read where it lies

GameCube discs are not encrypted (Wii discs are), so nothing has to be extracted with Dolphin first: the launcher, the build and the running game all take the player's disc image as it is. `disc/` is the small library that does it (`qilin_disc`, C++23, no dependencies, runtime code: no `std::format`) and `qilin-disc` the tool over it.

| Opens | Recognised by | Notes |
|---|---|---|
| Plain image, `.iso` / `.gcm` | the magic at `0x1C` | whole (1,459,978,240 bytes) or cut short after its last file |
| CISO, `.ciso` | `CISO` at 0 | absent blocks read as zero |
| Extracted folder | a directory with `sys/` | Dolphin's layout: `sys/boot.bin`, `bi2.bin`, `apploader.img`, `main.dol`, `fst.bin`, `files/...` |

The format is taken from the contents, never from the file's name.

## The formats, as facts

**The disc.** All fields big-endian.

| Offset | Size | What |
|---|---|---|
| `0x0000` | `0x440` | `boot.bin`: game id (4 bytes: console, two of game code, region), maker (2), disc number (`0x06`, 0 for the first), revision (`0x07`), audio streaming flag and buffer size (`0x08`, `0x09`), the magic `0xC2339F3D` at `0x1C`, the title at `0x20` (NUL-terminated, up to `0x3E0` bytes), the DOL's offset at `0x420`, the FST's offset at `0x424`, its size at `0x428`, the largest FST of a multi-disc game at `0x42C` |
| `0x0440` | `0x2000` | `bi2.bin` |
| `0x2440` | from its header | the apploader: `0x20` bytes of header (the build date as text at 0, the entry point at `0x10`, the code's size at `0x14`, the trailer's at `0x18`), then code and trailer. `apploader.img` is `0x20` + both sizes |
| header `0x420` | from its header | `main.dol`. Its length is not stored: it ends where its last section does (the largest file offset + size over its 7 text and 11 data slots) |
| header `0x424` | header `0x428` | the FST |
| per FST | | the files |

A Wii disc has `0x5D1C9EA3` at `0x18` instead, and is refused by name.

**The FST** is a table of 12-byte entries followed by the names. Entry: one byte of type (0 file, 1 directory), three of the name's offset within the names; then for a file its disc offset and its length, for a directory the index of its parent and the index one past its last entry (at any depth). Entry 0 is the root: its "one past" is the number of entries, so the names start at 12 times that. The game's own DVD code walks this table in memory, which is why the library hands it over as raw bytes as well as decoded.

`parse_fst` rejects what a walk would run off: a count the table cannot hold, a type other than 0 or 1, a name outside the names or without its terminator, an empty name, a directory that names another parent than the one it lies in, one that ends at or before itself (a walk would never leave) or after its parent, and nesting deeper than 64.

**CISO.** A `0x8000`-byte header: `CISO`, the block size (32 bits, **little-endian**: the only such field anywhere here), then `0x7FF8` bytes, one per block: 1 present, 0 absent. The present blocks follow the header in order, each whole. Disc offset *o* is in block *o / size*; if present it is at `0x8000 + (number of present blocks before it) × size + o mod size`. Refused: a file shorter than the header, a block size of 0 or above 1 GB, a map byte other than 0 or 1, a file that does not hold every block its map lists.

**The folder.** Each `sys/` file must be the length the header gives it (`boot.bin` 1,088 bytes, `bi2.bin` 8,192, `apploader.img` as its own header says, `main.dol` as its sections say, `fst.bin` as `boot.bin` says) and each file under `files/` the length the FST gives it; all are checked when the folder is opened. A read by offset finds the file covering it: `boot.bin` at 0, `bi2.bin` at `0x440`, the apploader at `0x2440`, the DOL and the FST where `boot.bin` puts them, the files where the FST does. The FST is the committed `sys/fst.bin`, offsets as they are in it. Two things follow:

- Bytes that no file or system area covers read as zero. On an image they are whatever the disc holds there (see below), but nothing can ask for them through the FST.
- Files that overlap on the disc are refused (a folder cannot say which one a read there means), as are FST names that cannot be file names on every system (outside printable ASCII, any of `/ \ : * ? " < > |`, `.`, `..`, ending in a space or full stop). Such a disc is read from its image.

A folder's files are opened when first read and kept open, at most 48 at once (the least recently read is closed first): a disc can hold more files than a process may have open.

### Not read, and why

| Format | Recognised by | Why not |
|---|---|---|
| GCZ (`.gcz`) | `01 C0 0B B1` at 0 | zlib-compressed blocks: it needs an inflate, so either a dependency (the project keeps to SDL3, Vulkan headers, volk, Dear ImGui) or our own decoder. The container is simple (a header with the block size and count, a table of 64-bit block offsets whose top bit marks a block stored uncompressed, Adler-32 sums); the inflate is the cost. Dolphin converts it to ISO in one step |
| RVZ, WIA (`.rvz`, `.wia`) | `RVZ\x01`, `WIA\x01` | two header levels with SHA-1 sums, tables of "raw data" and group entries, and per-group compression by any of bzip2, LZMA, LZMA2 or (RVZ) Zstandard: three or four decompressors. RVZ also stores padding as the seed of the disc's junk generator, which must be re-run to give the bytes back. Weeks of work and several dependencies for what Dolphin converts in a minute |
| NKit (`.nkit.iso`, `.nkit.gcz`) | `NKIT` at `0x200` | not the disc: junk removed, files moved and the FST rewritten to match. It runs in Dolphin, but offsets are not the original's, so nothing checked against a real disc (hashes, timing) holds. Refused; convert it back with NKit, or use the original dump |
| WBFS | `WBFS` at 0 | holds Wii discs |

Each is refused with a message that says which it is and what to do.

## Mario Kart: Double Dash!! (USA), as `qilin-disc` reports it

`GM4E01`, maker `01`, disc 0, revision 0, title "Mario Kart Double Dash!"; apploader of 2003/04/17, 119,332 bytes; `main.dol` 3,572,640 bytes at `0x1F700`, SHA-256 `e96b8578451b9157e2b68fe5e918ebb572940c3ea54d6c8c7d45c24382bf12ae`; FST 4,883 bytes at `0x387B00` (its maximum the same): 182 entries, 166 files, 15 directories; 487,292,675 bytes of files. The files lie at the end of the disc, from `0x39FA00FC` to exactly `0x57058000` (the disc's size), every offset a multiple of 4.

The owner's CISO has 2 MB blocks, 236 of them present (494,961,216 bytes). Between the end of the FST and the first file, the image holds 2,183,537 non-zero bytes that belong to no file; a folder reads them as zero. Everything else outside the files and system areas is zero.

## The interface

`#include "qilin/disc/disc.hpp"`, link `qilin_disc`; namespace `qilin::disc`. The header is the reference; in short:

```cpp
auto disc = qilin::disc::Disc::open(path);        // Result<Disc> = std::expected<Disc, std::string>
if (!disc) fatal(disc.error());                   // the message names the file and what is wrong

disc->header();      // game_id "GM4E01", maker, disc_number, revision, title, dol_offset, fst_offset, fst_size, ...
disc->boot_bin();    // spans kept in memory: boot.bin, bi2_bin(), apploader(), fst()
disc->dol_range();   // {offset, size}; dol() reads it
disc->entries();     // the FST decoded: directory, name, path, parent, offset, size, end
disc->find("/Course/Luigi.arc");                  // const Entry*, ASCII case ignored, null if absent
disc->read(offset, std::span<uint8_t>(buffer));   // Result<void>: the disc's bytes from offset
disc->read_file(*entry, offset_in_file, span);    // the same within one file
```

- Nothing throws, nothing asserts on input. Every failure is a `std::string` naming the file.
- A `Disc` is immutable once opened; `read` is positional (`pread`, or `ReadFile` with the offset in an `OVERLAPPED`) and safe from any number of threads at once. The image is never loaded: only `boot.bin`, `bi2.bin`, the apploader and the FST are kept (130 KB for this game).
- `read` accepts any range within `size()` (a GameCube disc's 1,459,978,240 bytes, or the image's length if that is more). Past a cut-short image's last byte, in a CISO's absent blocks and in a folder's gaps it gives zeros; a range running past `size()` is an error, as is any failure of the host's read (a file removed or changed in length under an open folder included). Nothing is returned short.
- `open` insists that the system areas, the FST and every file lie within the image (for a plain image its length, so a truncated download is refused; for a CISO what its map can address). Overlaps and the DOL's addresses are `verify`'s business, not `open`'s.
- `extract(disc, folder)`, `write_dol(disc, file)` and `verify(disc)` are what the tool's commands call.

## How `mkdd/src/dvd.cpp` should use it

One `Disc` for the process, opened at start-up from the path the launcher passes (image or folder: the same code), fatal with its message if it does not open.

- **The FST in guest memory.** Copy `fst()` to where the apploader would have put it and set the low-memory pointers as the apploader does. The game's own `DVDConvertPathToEntrynum`, `DVDFastOpen` and directory functions then work from guest memory, lifted, with no replacement. `boot_bin()`'s first `0x20` bytes are the disc id at `0x80000000`; `bi2_bin()` is what the SDK reads its debug and country fields from.
- **Reads by offset.** Replace the lowest level, where the SDK tells the drive "read *length* bytes at *offset* into this buffer" (the `DVDLowRead` level), with `disc->read(offset, guest_buffer)`. That one call serves files, the FST re-read and anything else the game asks for; no per-file bookkeeping or path mapping is needed. If instead the higher level stays replaced as in the Wii code, `find()` and `read_file()` serve it; note the GameCube's FST stores byte offsets, not the Wii's offsets / 4.
- **Timing is `dvd.cpp`'s.** The library reads as fast as the host does and blocks while it does. When the read completes for the game (the drive's seek and transfer time, the interrupt and callback) is the caller's model; the library says nothing about it. Reads may come from the guest's DVD thread and others at once.
- **Errors.** A failed `read` is a host failure (the image vanished, a folder's file changed): `rt::fatal` with the message. Do not hand the game zeros.

## How the launcher and the build should use the tool

```
qilin-disc info    <image|folder> [--json]   id, title, revision, DOL size and SHA-256, FST and file counts
qilin-disc list    <image|folder>            offset, size, path of every entry (directories end in '/')
qilin-disc extract <image|folder> <folder>   sys/ and files/, as Dolphin's "Extract Entire Disc" writes them
qilin-disc dol     <image|folder> <file>     just main.dol
qilin-disc verify  <image|folder>            structure checks
```

Exit status 0; 1 for an error or a failed check, with the message on stderr; 2 for a wrong command line. Paths may hold spaces and any Unicode (on Windows the wide command line is used).

1. `qilin-disc info <image> --json`: require `"id": "GM4E01"`, `"revision": 0` and `"dol_sha256": "e96b8578451b9157e2b68fe5e918ebb572940c3ea54d6c8c7d45c24382bf12ae"` before building anything, and say which of the three is wrong otherwise (another region or revision is the likely case). The keys: `format`, `id`, `maker`, `disc`, `revision`, `title`, `apploader_date`, `apploader_size`, `dol_offset`, `dol_size`, `dol_sha256`, `fst_offset`, `fst_size`, `fst_entries`, `files`, `directories`, `file_bytes`.
2. `qilin-disc dol <image> <build folder>/main.dol`, and configure with `-DQILIN_GAME_DOL=` that file. The DOL is game data: it goes in the build tree, never the source tree.
3. Run the game with the image's path. No `extract` is needed to play; it is there for people who want the files, and to check the library against Dolphin.

`verify` checks: every area and file within a GameCube disc's size; none overlapping another (system areas, DOL, FST, files); the FST no larger than the header's maximum; no two entries with the same path (case ignored, as the console compares); the DOL's sections outside its header, not overlapping in the file, loading within `0x80000000`–`0x81800000`, the entry point inside a text section; and that the recompiler's own DOL loader accepts it.

## Tests

`ctest -R qilin_disc_tests` (no game data): one synthetic disc (header, apploader, a small DOL, nested directories, an empty directory, a zero-length file, files that touch, a file of zeros, 80 small files in a row, gaps) written as a plain image, a CISO with absent blocks and a folder, in a temporary folder whose name has a space and non-ASCII letters. All three must give the same header, FST bytes, entries, path lookups and bytes for reads by offset (within files, across files that touch, through gaps and absent blocks, 80 files in one read, up to and across the disc's end, from four threads at once), and extract to identical folders. Then 56 malformed images, CISOs and folders, each to be refused with a message holding the file's name and the reason, and ten discs that open but that `verify` must object to.

Checked by hand against the real disc (2026-11-25): `info` on the owner's CISO and on the folder extracted from it earlier by other means agree in every field (above); `extract` of the CISO (13 s) is byte-identical to that folder, all 171 files compared by SHA-256; `verify` passes on both.
