# Graphics: what this game asks of the GPU

The design is the base's and `docs/runtime.md` explains it ("The GPU's front end", "The renderer", "Comparing a frame with Dolphin"): the SDK's GX and the game's J3D are lifted with the game and write the real GPU's command stream; `gpu.cpp` keeps the registers and decodes it, `recorder.cpp` turns it into draws, `renderer.cpp` draws them with Vulkan 1.3. This file holds what differs for a GameCube and for Mario Kart: Double Dash!!, which GX features the game uses and how far each is done, and what to check once the game runs.

Everything here was read from the retail code and the disc's data (2026-11-25); the game had not run yet. Function names are the debug map's (and doldecomp/mkdd's for classes), addresses the retail DOL's.

## The registers GX touches

The GameCube's and the Wii's GPU sit at the same addresses, so the base's map held: CP `0xCC000000` (16-bit), PE `0xCC001000` (16-bit), PI `0xCC003000` (32-bit), the write-gather pipe `0xCC008000`. The SDK keeps their bases in small data (`__piReg`, `__cpReg`, `__peReg`, `__memReg` at r13 −25288, −25284, −25280, −25276) and these are all the accesses the retail GX makes:

| Register | Who | What we do |
|---|---|---|
| CP `00` status (read) | `GXCPInterruptHandler`, `GXGetGPStatus` | `0x000C`: read and command idle, no overflow, underflow or breakpoint. The decoder keeps up with the pipe, so the watermark interrupts never come. |
| CP `02` control, `04` clear, `06` (`GXInit`, `GXSetGPMetric`: the performance select) | FIFO link, read enable, interrupt enables | stored; bit 4 of control links the pipe to the decoder |
| CP `20`-`3A` FIFO base, end, watermarks, distance, write and read pointers | `GXSetGPFifo`, `__GXSaveCPUFifoAux` | stored; the distance reads 0 and the read pointer is the write pointer |
| CP `40`-`4E` | `GXReadXfRasMetric` | **0**: the XF and rasteriser counters aren't modelled. Only `JFWDisplay`'s `diagnoseGpHang` reads them, when a frame's draw-done never came. |
| PE `00`-`08` | the `GXPoke*` setters, only from `__GXInitGX` | stored. They configure EFB pokes, and nothing pokes: the retail DOL has no `GXPeekARGB`, `GXPokeARGB`, `GXPeekZ` or `GXPokeZ`, and no code builds an address in the EFB's window at `0xC8000000` (searched). The runtime maps nothing there, rightly. |
| PE `0A` control, `0E` token | `__GXPEInit`, the finish and token handlers | as the base: draw-done (BP `0x45`) and tokens raise their interrupts 50 µs after the command is decoded |
| PI `0C`, `10`, `14` CPU FIFO base, end, write pointer | `GXSetCPUFifo`, `__GXSaveCPUFifoAux` | physical addresses in MEM1 (mask `0x03FFFFE0`); bit 26 of the write pointer is the wrap flag |
| PI `18` FIFO reset | `__GXAbort`, `GXAbortFrame` (1, then 0 after 50 ticks) | writing 1 drops the commands not yet decoded (`abort_fifo`); what was drawn stays. Reached from `JFWGXAbortAlarmHandler` (a frame that never finished) and `ResetAgent::calc` (the reset button). |
| MEM `0xCC00404E`, `50` | `__GXAbort`, `GXAbortFrame`, `__GXShutdown` loop until two reads agree | **not ours** (`hw.cpp`): must read as a constant, or those loops never end |

The rest of the PI's block (interrupt cause and mask at `00`/`04`, the reset register at `24`, the console type at `2C`) is the OS's business; `gpu.cpp` still maps the whole block and stores what is written, as the base did.

What the command stream holds, and how this SDK differs from the Wii's the base met:
- **No `GXBeginDisplayList`, `GXEndDisplayList` or `GXRedirectWriteGatherPipe`** in the DOL. J3D builds its display lists in its own buffers with the GD library (`J3DDisplayListObj::beginDL`, the `J3DGD*` family), interrupts off, and calls them with `GXCallDisplayList`. Material state (TEV, textures, fog, blending) reaches the GPU only this way, never through the GX API. The pipe is never pointed at memory, though the unlinked path still works.
- **Display lists don't nest** (23 callers of `GXCallDisplayList`, none inside a list; no raw `0x40` written by code). A nested call is still fatal.
- **Indexed matrix loads `0x20` and `0x28`** are written raw by J3D (`J3DSys::loadPosMtxIndx`, `loadNrmMtxIndx`); `0x30`, `0x38` aren't used. So are CP loads of the matrix indices (`0x30`, `0x40`), XF `0x1018`, and the array bases `0xA0`-`0xA2`.
- **No token** (`GXSetDrawSync` is absent), no raw `0x44` or `0x48` from code; `GXInvalidateVtxCache` writes `0x48` and the metrics calls `0x44`, both one byte, skipped.
- **No TMEM preloads** (`GXPreLoadEntireTexture` is absent; BP `0x63` with a size stays fatal). `J3DFifoLoadTexCached` writes the TMEM image registers `0x8C`/`0x90`, which we don't need.
- `GXInvalidateTexAll` (BP `0x66`) comes once a frame, from `JFWDisplay::beginRender`: a texture's data is hashed at most once a frame, as on the Wii.
- No bounding box is read (`GXClearBoundingBox` from `__GXInitGX` only), no `GXAdjustForOverscan`.
- Memory is MEM1 alone (`gx::in_memory`): a display list, vertex array, texture, palette, copy or FIFO address outside its 24 MB is fatal. `MKDD_DFF` writes a GameCube's log (no MEM2, game id `GM4E01`; **not yet played back in Dolphin**).

## The picture

The game brings its own render modes (four `GXRenderModeObj` at `80336F58`…; the SDK's `GXNtsc480IntDf` and others are in the DOL but `System::init` replaces the default before use):

| Mode | TV | EFB | XFB | VI | Copy filter (7 taps) |
|---|---|---|---|---|---|
| normal, interlaced (boot default) | NTSC 480i, double field | 608×448 | 608×448 | origin 27,16, 666×448 | 32, 0, 32, 0, 0, 0, 0 |
| movie, interlaced | NTSC 480i | 608×448 | 448 | same | 0, 0, 21, 22, 21, 0, 0 |
| normal, progressive | 480p | 608×448 | 448 | same | 2, 2, 17, 22, 17, 2, 2 |
| movie, progressive | 480p | 608×448 | 448 | same | 0, 0, 21, 22, 21, 0, 0 |

- `System::changeProgressive` switches after the logo if the cable allows it (`VIGetDTVStatus`) and progressive mode is saved or B is held; `changeMovieRenderMode` and `changeNormalRenderMode` switch around THP movies.
- **No anti-aliased mode**: every table has `aa` 0, so `JFWDisplay::beginRender`'s `GXSetPixelFmt(RGB565_Z16)` branch is never taken. If the format ever changes to or from RGB565 the recorder says so once (the picture is then kept as it was, not reinterpreted).
- The EFB is **RGBA6_Z24 with dither** in play (`JFWDisplay` is created with alpha on); THP movies set RGB8_Z24. The base's reinterpretation of the picture at each change applies.
- The Y scale is 1 (448 to 448). Gamma is 1.0, the copy clamps top and bottom, the copy to the XFB always clears (colour 0,0,0,0, Z `0xFFFFFF`).
- So the picture handed to VI is **608×448 for a 4:3 screen**, shown 666 wide of the 720 by VI's own scaling. In our files nothing assumes a size or a shape: the XFB image is whatever the copy made. Found elsewhere, for their owners: `main.cpp`'s `--render-dff` reads back into a fixed 1280×720 and `MKDD_DUMP` writes 1280×720 (16:9); `video.*` and the window's letterboxing are the Wii's.
- The interlaced mode's filter gives each line half of the line above it. It is set for the whole frame, so texture copies made in mid-frame (the blur, the shimmer's colour) are filtered by it too, as on the console; copies of depth are not (below).

## What the game uses

From the disc: 387 distinct models (all `J3D2bmd3`, no BDL), 1,039 materials, 2,105 textures, 442 particle emitters (`JPAC2-10`), 116 J2D screens. From the code: every caller of the GX API outside the SDK. Status: **done** (implemented, and tested where a test is named), **approx.** (how), **not done** (what happens).

### Geometry
| Feature | Use | Status |
|---|---|---|
| Vertex formats | models: every attribute by 16-bit index, matrix indices direct; positions f32 (two models s16), normals s16 (fraction 14 or 15) or f32, colours RGBA8, texture coordinates s16 (fractions 8-15) or f32; never more than two texture coordinates, one colour, no NBT. Code: direct s16/u16/s8/u8/f32 in formats 0-2 and 7 | done (the decoder is checked against a per-vertex reference with `MKDD_CHECK_DECODE=1`) |
| Primitives | models: triangle strips only, **5.1 vertices each on average** (153,679 strips, 785,719 vertices), one fan model; code: quads, strips, fans, lines (`stParticleDraw::drawParentLine`, J2D), points only if a particle asks (none on the disc does) | done; strips of one state are joined into one draw (`joined_primitives` test) |
| Display lists | a model's shapes (up to 47 KB each), materials (GD), JPA's and `stParticleDraw`'s static lists | done, read straight from guest memory |
| Matrices | per-vertex position and texture-matrix indices (83 skinned shapes; `driver.bmd`'s env map uses a texture matrix index), indexed loads, billboards (CPU side) | done (`indexed_matrices` test) |
| Culling, clipping | back 825, none 188, front 26. `GXSetClipMode` off for some particles | culling done. **Clip mode: not done**, XF `0x1005` is ignored and Vulkan always clips at the near and far planes |
| Line width, point size | J2D, JPA, `stParticleDraw` | done (wide lines and large points where the GPU has them) |
| Texture offsets for lines and points (`GXEnableTexOffsets`) | JPA and `stParticleDraw` enable them | **not done**: a wide line's texture coordinate doesn't spread across its width. Only textured lines would show it. |
| Viewport, scissor, scissor offset | 1-4 views through `J2DGrafContext::setPort`; `SceneMenu::draw` uses scissor offsets (0x4C, 0x2E, 0x19) | done (`split_screen` test; the offset's wrap as the base) |

### Lighting and texture coordinates
| Feature | Use | Status |
|---|---|---|
| Channels | one colour channel; lit in 353 materials: diffuse clamp, spot attenuation, light 0 (or 7), ambient from register; alpha never lit | done (the base's XF lighting) |
| Texgens | 2×4 1,166, 3×4 123 (projected), SRTG 2; sources tex0/1, normal 180, position 4, colour 2; matrices identity or texture matrix 0-2; no post matrices, no emboss | done |
| Projected textures (STQ) | 123 texgens, env maps and `Projmap`/`ViewProjmap` | done (the divide by q is per pixel) |

### Textures
| Feature | Use | Status |
|---|---|---|
| Formats | CMPR 1,130, IA4 462, I4 140, I8 116, RGB5A3 96, IA8 78, C8 43 (RGB5A3 palettes), RGB565 39, RGBA8 1; no C4, no C14X2; up to 1024 on a side; 245 not a power of two (clamped, one mirrored on its power-of-two side); one I4 of 278×557 | done (decoded on the CPU; partial tiles handled) |
| Mip levels | 766 textures with 2-7 levels; min filter linear or trilinear only | done |
| LOD bias | 195 textures, −3.9 to +3.06 (−1.5 the common one); min LOD 0, max LOD the last level | done (`lod_bias` test) |
| Edge LOD, bias clamp, anisotropy | never set | n/a (the base's settings menu can add anisotropy) |
| Wrap | clamp, repeat, mirror in every pairing | done |
| Z24X8 as a texture | `JFWDisplay::clearEfb`'s 4×4 | done (RGBA8's layout) |
| Texture cache regions | J3D's own (`J3DSys::setTexCacheRegion`) | nothing to do: we keep no TMEM cache |

### TEV and the pixel
| Feature | Use | Status |
|---|---|---|
| Stages | models 1-3, J2D 1-3, code up to 3 (shimmer) | done (ubershader and specialised pipelines, 16 stages) |
| Combiners | add 1,286, subtract 7 (alpha 11); bias ±½; scale 1, 2, 4, ½; clamp off in 91 colour and 239 alpha stages; inputs from C0-C2, konst, ONE, HALF; output always PREV. **No compare ops** in models (the shimmer compares against registers in code) | done, all ops including compares |
| Konst selects, swap tables | K0-K3 and fractions; swap tables identity but for `ShimmerScreen::levelDraw` (table 1 = A,G,B,R) | done |
| Indirect textures | 37 model materials (shells, fire bars, fountains, cups, Rainbow Road): one indirect stage, 8-bit, bias STU, matrix 0, no wrap; `ShimmerScreen` uses `GXSetTevIndWarp` with matrix 1; 9 J2D materials; no particle | done in the shader, **not tested here** (Punch-Out never used it either): first thing to compare with Dolphin |
| Alpha compare | always 812; ≥ and ≤ (128, 255) 185; others 42; no XOR/XNOR | done |
| Z compare before or after texturing | before 843, after 196 (24 alpha-tested materials still compare before) | done: a draw that compares before and whose alpha test can fail uses the same shader with early fragment tests declared (patched into the SPIR-V as the module is made), so a dropped pixel still writes its depth (`z_before_alpha_test`). The base dropped the depth with the pixel. |
| Z textures | `JFWDisplay::clearEfb` every frame: replace, 24 bits, Z always | done (`clear_by_z_texture` test) |
| Blending | none 812, alpha blend 167, additive 42, a few others; **subtract** in `ShadowManager::draw`; no logic ops | done |
| Colour and alpha update masks, destination alpha | shadows draw into alpha alone; ghosts use `GXSetDstAlpha(1, a)` then copy the alpha; the EFB clear writes a constant alpha | done (write masks are pipeline state; `clear_by_z_texture` checks the constant alpha through an A8 copy). **Approx.** where the base is: a constant alpha together with blending that reads source alpha needs dual-source blending (not on the Pi 4's driver; there the constant isn't written) |
| Dither | on in play (RGBA6) | done (off in true colour) |
| Fog | perspective linear 633 materials, perspective exponential 16 (`patapata_course`), none 389; no orthographic kinds, no range adjustment | done |

### EFB copies
| Copy | Format | Status |
|---|---|---|
| `BlurScreen` scene and blur | RGB565, half scale, the first clearing | done |
| `GhostScreen` scene (RGB565, clearing) and ghost (A8) | | done |
| `ShimmerScreen` colour (RGB565, half) and depth (**Z16**, half, sampled as IA8) | | done: Z16 holds the depth's middle byte as intensity and the high byte as alpha (`depth_copies` test); the base had the high byte as intensity |
| `ShadowScreen` | A8, half scale, from the EFB's alpha | done (`clear_by_z_texture` test) |
| `MaskScreen` (20 of 30×32), `GeoVisionSupervisor` (200×120, the course's screens) | RGB565 | done |
| `ZCaptureNode` | **Z24X8, 2×2, read by the CPU** (`getZValue`: is a player's mark or the sun hidden?) | done, late: a copy of one tile or less in RGBA8's layout is also written to guest memory when the GPU has drawn it, one or two frames after the game asked (`depth_copies` test). The console has it at once. |
| Any other copy | | stays a GPU image found by its address; **guest memory isn't written**. The game reads no other copy on the CPU (searched: only `JUTTexture::load` binds them). |
| The copy filter | set all frame | colour copies filtered; depth copies **not** (as Dolphin; a blend of depth bytes means nothing) |
| To the XFB | filter, clamp, gamma 1, clear | done |
| The XFB written by the CPU | `JUTDirectPrint`, `JUTException` (debug text, the crash screen) | **not done** in the renderer: the picture shown is the GPU's copy. A crash screen would not show. |

### Not used, so untouched
Anti-aliased EFB, EFB peeks and pokes, bounding box, tokens, TMEM preloads, C4/C14X2, emboss, NBT, logic ops, orthographic fog, fog range adjustment, field rendering (`GXSetFieldMode`/`GXSetFieldMask` only from `__GXInitGX`). The first three would stop loudly or log; the rest are implemented in the base anyway.

## Efficiency

Measured with `mkdd_render_test --bench [players] [frames] [vertices a strip]`: a frame shaped like a race from the disc's own numbers (per view a course of 20 shapes and 25,000 vertices and 48 kart and object shapes of 300, each shape its own material, matrices and display list of five-vertex strips with three 16-bit indices a vertex), through the real decoder, recorder and renderer. GTX 1080 Ti and a desktop CPU **shared with five other builds** while measuring, so medians are high and the fastest frame is the better guide.

| | draws | decode + record, ms | renderer on the CPU, ms |
|---|---|---|---|
| 1 view, as seeded | 7,880 | 8.8 median | 1.7 median |
| 1 view, primitives not joined (`MKDD_JOIN=0`), the rest as now | 7,880 | 3.5 fastest | 0.89 fastest |
| 1 view, now | 68 | **1.1 fastest** (1.9 median) | **0.22 fastest** (0.43 median) |
| 4 views, as seeded | 31,520 | 33.9 median | 5.8 median |
| 4 views, now | 272 | 7.5 median | 1.4 median |

What changed, in order of effect:
- **A primitive drawn with the state of the one before joins its draw** (an indexed triangle list in the primitives' order). The base gave each primitive a draw: state lookups, a shape hash, a viewport and texture bind, and a `vkCmdDraw` with its push constants, for five vertices. Nothing drawn changes: the same triangles in the same order under the same state (the test checks winding through culling). Off while `MKDD_DFF` captures, which numbers draws by primitive.
- **The vertex format, its plan and the vertex size are kept between CP writes** instead of being rebuilt for each primitive.
- **Recording is timed a batch of commands at a time**: two clock readings a primitive cost as much as the primitive.

About 19 ns a vertex and 45 ns a primitive remain on that CPU (from the same frame as 50-vertex strips: 0.77 ms). Not done, in the order I'd take them on the Pi 4: the vertex is 76 bytes where this game's needs 44 (two texture coordinates at most), written once by the decoder and copied once more to the GPU's buffer (12 MB a 4-view frame, each way); a draw's state snapshot copies 5 KB when a matrix changes (272 a frame now, so 1.4 MB: no longer first).

## Tools

- A GLSL compiler isn't on the build machine and CI installs `glslang-tools`. For the shader change here, glslang 15.1.0 was built from source in scratch (`work\gpu\bin\glslangValidator.exe`), and `tools/build_shaders.py` used as it is. Every shader's SPIR-V bytes changed with the compiler's version; only `copy.frag` changed in source.
- `mkdd_render_test` links `gpu.cpp` and feeds it bytes (`decode_commands`), so scenarios can be written as the game writes them: BP, XF and CP loads, display lists in guest memory, raw indexed loads. The few OS functions `gpu.cpp` calls are stubbed in the test.
- `tools/check/picture_scenes.json` holds one scene, the first seconds with no input; the rest need the running game to script. `picture_check.py` itself is as seeded (its `--save` copies a `nand/` folder, which this game won't have).

## To check once the game runs

1. The boot: `GXInit`'s register writes all land in mapped ranges; the first `GXDrawDone`; the logo. If `GXAbortFrame` is ever reached, the log says "the FIFO is reset".
2. The first frame's picture against Dolphin (`picture_check.py`): the EFB clear by Z texture, 608×448, the interlaced filter. A GameCube `.dff` has not been through Dolphin's player yet.
3. Indirect textures (a green shell, the menus' title lines), then the screen effects in a race: shadows (EFB alpha, subtract blending), ghosts, blur, shimmer (Z16 through swap table 1), the masks.
4. The 2×2 depth read-back: player marks and the lens flare should hide behind scenery, a frame or two late at worst. If the lateness shows, the copy has to be answered on the game's thread instead.
5. Particles with clipping off, and `stParticleDraw`'s lines (texture offsets).
6. The real cost of a frame (`MKDD_DFF` a race frame, then `--bench-record`), and how many draws remain after joining: a J3D shape changes matrices between packets, not within one.
7. Pipelines: `pipeline_seed.inc` is empty; make it from a play-through's `pipelines.keys` (`tools/make_pipeline_seed.py`) so a first run compiles nothing while playing.
