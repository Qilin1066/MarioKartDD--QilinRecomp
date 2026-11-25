# Mario Kart: Double Dash!! decompilation: the PAL retail build's symbols and splits

From [doldecomp/mkdd](https://github.com/doldecomp/mkdd) at commit `0c3baef1ba96867f4ebbcdde02f7a43624224a85`, a work-in-progress decompilation. Licence: **CC0 1.0** (`LICENSE`).

| File | What |
|---|---|
| `Release_eu/symbols.yml` | Names the decompilers placed by hand in the PAL retail build (`GM4P01`): address and name, no sizes (4,417 in the text) |
| `Release_eu/dol_slices.yml` | Which address ranges of the PAL build belong to which source file (318 translation units have a text range) |

This repository supports the US build (`GM4E01`), whose names come from the debug build's linker map on the disc (`tools/analysis/port_symbols.py`). The PAL lists are a second opinion from people who read the code: the port checks its names against `symbols.yml` (a name in both must keep its distance to the PAL address within a translation unit), and uses the order of the translation units in `dol_slices.yml` where its own evidence cannot place an object. `docs/code-analysis.md` has the results.

The files are unchanged copies of the decomp's `config/Release_eu/symbols.yml` and `dol_slices.yml`; update them by copying those again and noting the new commit here. The decomp's lists for the debug build (`config/MarioClub_us`) are not needed: the disc's own map says the same.
