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
| Culling, clipping | back 825, none 188, front 26. `GXSetClipMode` off for some particles: counted headless, 0.1% of the primitives of 225 s of the attract mode (title and demo races) and 0.26% of a four-player race's, all quads | culling done. Clip mode done where the GPU has depth clamping (`Gpu::depth_clamp`): XF `0x1005` bit 0 makes `PipelineKey::no_clip`, a pipeline that clamps depth instead of cutting at the near and far planes (`clip_off` test). Without it such draws are cut as before, and the log says so once |
| Line width, point size | J2D, JPA, `stParticleDraw` | done (wide lines and large points where the GPU has them) |
| Texture offsets for lines and points (`GXEnableTexOffsets`) | JPA and `stParticleDraw` enable them | **not done, and not needed so far**: counted headless over 225 s of the attract mode and a four-player race, the game drew no lines and no points at all (208 million primitives, every one a triangle, strip, fan or quad), so nothing reaches the offsets. Were one drawn, a wide line's texture coordinate wouldn't spread across its width. |
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
| Colour and alpha update masks, destination alpha | shadows draw into alpha alone; ghosts use `GXSetDstAlpha(1, a)` then copy the alpha; the EFB clear writes a constant alpha | done (write masks are pipeline state; `clear_by_z_texture` checks the constant alpha through an A8 copy). A constant alpha beside blending by source alpha: below (`constant_alpha` test, both ways) |
| Dither | on in play (RGBA6) | done (off in true colour) |
| Fog | perspective linear 633 materials, perspective exponential 16 (`patapata_course`), none 389; no orthographic kinds, no range adjustment | done |

**A constant alpha with blending by source alpha** (2026-11-26). GX writes `GXSetDstAlpha`'s constant as the pixel's alpha while the blend's factors still take the TEV's alpha. The base wrote the TEV's alpha there on every GPU. Now:
- with dual-source blending (`Gpu::dual_src_blend`, desktop GPUs): the shader's second output (`ocol1`, declared at location 1 and moved to location 0, index 1 as the modules are made: `with_second_output`) carries the TEV's alpha, the factors become `SRC1_ALPHA`/`ONE_MINUS_SRC1_ALPHA`, and the first output's alpha is the constant;
- without (the Pi 4's v3dv; `MKDD_NO_DUAL_SOURCE=1` forces it): the EFB's depth gets a stencil (D32S8, else D24S8), the draw is made as before marking every pixel it writes with its own stencil value (the marks run 1-255 a frame and the stencil is cleared when they run out), then one triangle over its scissor writes the constant alpha, rounded as the shader rounds it, where the stencil holds that mark. A pixel dropped by the depth or alpha test isn't marked, so this is exact, with two exceptions logged once: a draw with Z before texturing (its dropped pixels pass the early tests and would be marked; none seen), and within one draw, a later triangle blending by destination alpha over an earlier one sees the TEV's alpha, not the constant (no such blend seen).

  It shows in this game: course materials draw with constant alpha `E7` (the boost pads on Luigi Circuit, blended by source alpha), and a later screen-wide pass darkens by a copy of the EFB's alpha (`MKDD_PROBE` on the capture: draw 153, blending a dark colour by a texture's alpha; the shadows' pass, by its look). With the TEV's alpha written instead, the pads came out darkened (the one-view capture, 3,130 pixels at 1x; four views unchanged); both paths now give the same picture, in which they aren't.

### EFB copies
| Copy | Format | Status |
|---|---|---|
| `BlurScreen` scene and blur | RGB565, half scale, the first clearing | done |
| `GhostScreen` scene (RGB565, clearing) and ghost (A8) | | done |
| `ShimmerScreen` colour (RGB565, half) and depth (**Z16**, half, sampled as IA8) | | done: Z16 holds the depth's middle byte as intensity and the high byte as alpha (`depth_copies` test); the base had the high byte as intensity |
| `ShadowScreen` | A8, half scale, from the EFB's alpha | done (`clear_by_z_texture` test) |
| `MaskScreen` (20 of 30×32), `GeoVisionSupervisor` (200×120, the course's screens) | RGB565 | done |
| `ZCaptureNode` | **Z24X8, 2×2, read by the CPU** (`getZValue`: is a player's mark or the sun hidden?) | done, on time: below (`depth_copies`, `z_on_time` tests) |
| Any other copy | | stays a GPU image found by its address; **guest memory isn't written**. The game reads no other copy on the CPU (searched: only `JUTTexture::load` binds them). |
| The copy filter | set all frame | colour copies filtered; depth copies **not** (as Dolphin; a blend of depth bytes means nothing) |
| To the XFB | filter, clamp, gamma 1, clear | done |
| The XFB written by the CPU | `JUTDirectPrint`, `JUTException` (debug text, the crash screen) | done: our copies to a frame buffer don't write guest memory, so its memory keeps what it held at the first copy unless the CPU writes it. Its hash is taken then (and at the first copy after a look); once copies to it stop for a tenth of a second the memory is hashed again, and if it changed, VI's frame buffer in memory is shown (`Renderer::cpu_wrote_xfb`, main.cpp, `cpu_frame_buffer` test). Normal drawing hashes nothing. The text then shows over what the memory held (the game's first clear), not over the last copy's picture, which isn't in memory. |

**The Z the CPU reads, on time** (2026-11-26). `RaceDrawer::drawSceneFirstStage` copies 2×2 pixels of depth for each of its 11 nodes a view (`ZCaptureNode::capture`, then `GXPixModeSync`), and `Race2D::calcPlayerMark` and `TFlareLine::calc` read the first pixel in the next pass's calculation (`ZCaptureNode::getZValue`, `8029582C`: bytes 1, 32 and 33 of the tile), which `System::run` makes straight after `JFWDisplay::endRender`: the console's GPU has made the copy by then. Here:
- the renderer, at the last of a run of such copies, ends the command buffer there and submits it signalling the next value of a timeline semaphore, and records the rest of the frame in another (`submit_cpu_copies`);
- `getZValue` is wrapped (the plan, `mtxtag.cpp`): it catches the GPU thread up with what the game has sent (`sync_decoder`, gpu.cpp: the frame the copies are in has ended at endRender's draw done), then waits until every copy to that node's buffer handed to the renderer is in memory (`await_cpu_copy`, texture.cpp: waiting on the semaphore itself and writing the tile, so the drawing thread needn't come back to it);
- the recorder counts a frame's copies for the CPU as it queues it, so a read waits only for copies on their way (none when nothing draws: `--headless`).

  Measured on the captured race frames (`mkdd_render_test --zwait-dff`, GTX 1080 Ti): the copies are in memory 0.27 ms after the renderer starts on a four-view frame (44 copies), against 2.3 ms for its recording and 4.2 ms for the GPU's end of the frame; one view (11 copies) 0.28 ms. What the game's read waits is that, plus the drawing thread's start on the frame. Not answered on time: with the early frame end off (`MKDD_EARLY_END=0`, a `MKDD_DFF` capture) the frame ends only at the next pass's copy, after the read, which then gets what memory held, as before. The pictures are unchanged (`--picture-dff` of both captures at 1x and 3x, byte for byte).

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

### Real race frames (2026-11-26)

Captured with `MKDD_DFF` on Luigi Circuit (`frame_rate = original`, the devices agent's scripts), then `mkdd_render_test --bench-dff <file.dff>` (the same as `mkdd --bench-record`, without the game). A four-view frame is 30,000 primitives of 4.6 vertices (28,000 of them from 1,950 display lists), 137,000 vertices, 843 draws after joining and as many states, 1.3 MB of textures hashed; one view, 9,200 primitives, 41,600 vertices, 314 draws. Commands: 36,000 BP writes, 30,000 NOPs (the lists' padding), 23,000 CP writes and 9,000 XF loads a four-view frame. Decode and record, best median of alternated runs (the machine shared with other builds):

| | 4 views, MSVC | 1 view, MSVC | 4 views, Clang | 1 view, Clang |
|---|---|---|---|---|
| before | 5.49 ms | 1.76 ms | 3.91 ms | 1.32 ms |
| now | **3.34 ms** | **1.16 ms** | **3.35 ms** | **1.16 ms** |

(Clang: Zig's, `-O2 -mcpu=x86_64_v3`, the recorder's files alone. In the game, offscreen, the four-view recording went from 6.1-6.5 ms a frame to 4.3-4.5, and the game's wait for the GPU thread from 3.2 ms a retrace to 1.2.) What changed:
- **Vertices by layout**: nine in ten primitives come in a handful of layouts, every attribute by a 16-bit index (position f32×3; RGBA8 colour or s16 normal; s16 texture coordinates). Each has a decoder compiled for it (`decode_fused`: one pass a vertex, constant offsets and formats, every byte written once); the rest keep `decode_columns`. `MKDD_CHECK_DECODE=1` compares either with the per-vertex reader, and passes on both captured frames.
- **Big-endian reads are a load and a byte swap** (`std::byteswap`): MSVC didn't recognise one in shifts and ors, and these run for every attribute. Most of MSVC's gain; Clang already did it.
- Runs of NOPs are skipped at once; the scissor search allocates nothing.
`--render-dff` pictures of both frames, at 1x and 3x, are identical byte for byte before and after.

Where a four-view frame's time goes now (MSVC; `MKDD_PROFILE=1 mkdd_render_test --bench-dff`, a sampler that reads the program database: build that target with `/Zi` and link with `/DEBUG`): the command loop about 20% (130,000 commands, mostly branch costs), the fused decoders 25% (mostly waiting on the vertex arrays' memory), per-primitive recording 13% (vertex and index vectors, strips to lists), states 10% (843 copies of 5 KB, `build_bp`, the shape hash), texture hashing 3%.

**Later the same day** (the same captures and method; best medians of alternated runs on a busy machine):

| | 4 views, MSVC | 1 view, MSVC | 4 views, Clang | 1 view, Clang |
|---|---|---|---|---|
| before | 3.41 ms | 1.17 ms | 3.13 ms | 1.09 ms |
| now | **2.82 ms** | **0.99 ms** | **3.03 ms** | **1.08 ms** |

- **States as shared blocks** (`gx::States`): each state is twenty 256-byte blocks, sharing every block whose bytes are the state before's; the recorder builds into one state of its own, copying only the position-matrix words and BP registers written since the last, and the renderer writes the blocks in order into its buffer (`resolve_all`), so the shaders' layout is unchanged. A four-view frame's states are 796 KB, one view's 296 KB (were 4.2 and 1.6 MB), read once more by the renderer instead of 4.2 MB.
- **A fused decoder for position and texture-matrix-1 indices** with position, s16 normal and s16 texture coordinate: an eighth of a race frame's primitives went through `decode_columns`.
- **The command loop** is a switch on the opcode's top five bits, and a CP write remakes only what reads it (array 12's base, the indexed loads' matrices, changes 600 times a frame and remade the vertex format each time). The recorder's switches are read once, the shape id is compared with the draw before's first, s16 normals are scaled by a multiply (MSVC kept the division).
- Tried and left out, slower here: prefetching vertex-array elements two or four vertices ahead inside a primitive (primitives are 4.6 vertices; +3-7%), holding the arrays' registers in the decoder (+4%: the copy a primitive costs more than the reloads), prefetching the next display list the FIFO calls (+2%). On this CPU the frame's data stays in the L3; a smaller cache may differ.
- **The hand-over to the GPU thread** (`mkdd_render_test --bench-pipe <file.dff>`: the frame's FIFO stored to the pipe a word at a time, the GPU thread decoding): the guest's side 2.92 ms, bound by the decoder's 2.97 ms behind the 16 KB queue; all decoded 3.01 ms after the first store. Under 0.1 ms is the hand-over's: left as it is.
- `mkdd_render_test --render-dff <file.dff> <out.ppm> [loops]` is `mkdd --render-dff` without a game build, for checking recorder changes against pictures.

**The race's 60-byte vertex** (2026-11-26, done): `gx::TinyVertex` (position, normal, two colours, two texture coordinates, the matrix-index words; nothing packed) is a third layout beside `SmallVertex` (four coordinates: the menus' models have three) and the full one; the recorder takes the smallest that holds the format (`PipelineKey::small_vertices` is `kLayoutTiny`, `kLayoutSmall` or `kLayoutFull`), the fused decoders have a version for each, and the renderer feeds the missing coordinates from zeros as before. Both captures are all tiny: 136,727 vertices a four-view frame, 8.0 MB where they were 10.2 (2.2 MB less written by the decoder and copied to the GPU's buffer), one view 2.4 MB (was 3.1). `--picture-dff` of both at 1x and 3x identical, `MKDD_CHECK_DECODE=1` passes on both.

What a frame's vertices need, from the captures: no layout has more than two texture coordinates or one colour, and no texture matrix index past 1. An exact compact layout (no packing: the 44-byte one changed pixels because it packed normals) would be 60 bytes with two texture coordinates instead of four (2.2 MB less written and uploaded a four-view frame), 48 if colour 1 and the matrix-index words 1-2 came from the draw instead of the vertex; both need the renderer's vertex input and a third layout (the menus have three coordinates), so they're the renderer's to take.

**Display lists called again** (`work/rec2/dlspans.py`, `cmdstats.py`): a four-view frame calls 1,953 lists (301 distinct, 1.2 MB); 76% of those bytes, and 105,000 of the 137,000 vertices, come from a list already called in the frame with the same bytes (one view: 35%, 16,000 of 41,000); no memory the GPU read changed within either captured frame. The vertices depend on the list's bytes, the CP registers, the last normal and every array element read. The game's stores aren't watched (lifted code writes memory directly), so only a comparison proves those unchanged: the list bytes (0.9 MB) and the arrays' spans (2.3 MB; 1.0 MB of distinct elements) against copies made at the first call, every frame. Against decoding 105,000 vertices (0.6-0.8 ms here, and 8 MB of vertices written and uploaded) that might pay on the Pi in four-player, but not clearly on a desktop. **Not done**: what a validated cache would have to hold is written here for whoever takes it on.

**The Pi 4, estimated** (not measured: no Pi here, and no hardware counters on this PC). Compute: a Cortex-A72 at 1.5 GHz against this Zen 3 at about 4.7 GHz with roughly twice the instructions a cycle is about six times slower a core, so 2.8 ms of a four-view frame becomes about 17 ms and one view's 1.0 ms about 6 ms. Memory: a four-view frame's recording reads 1.5 MB of commands and 1.3 MB of distinct array elements (within 3.1 MB of spans, read again for each view), and writes 10.4 MB of vertices, about 1 MB of indices and 0.8 MB of states; the renderer reads those and writes 15.5 MB to the GPU's buffers. At the 3-4 GB/s a Pi 4 streams, with each write also read for ownership, the recording's traffic alone is 6-8 ms, overlapping the compute. So **four views: 15-20 ms of the GPU thread a frame, at or over the 16.7 ms there is; one view: 6-7 ms**. The states' blocks took about 10 MB a four-view frame off that traffic (2-3 ms by the same reckoning). Next for the Pi, by size: the vertex bytes (the exact 60-byte layout; a validated cache of repeated lists), then the decoders' instructions.

**The write-gather pipe on the game's thread** (2026-11-26, tried and left out). A word store to the pipe costs 3.1-3.4 ns (`mkdd_render_test --pipe bench`: the inline check, the call to `write_slow`, the hook, the gathering, an eighth of a burst's hand-over). Gathering in the store itself (the burst in the runtime, `cpu.hpp`, filled inline by `write8/16/32`, a call only when it's full) took it to 1.5 ns, but made the game 11 MB bigger (52.9 to 64.1 MB: 130,000 stores in the lifted code each carry it) and changed nothing measurable: two side-by-side rounds (core's method, title, demo race and four-player race) differed by -0.3 to +0.3 ms a retrace either way, the noise. Gathering at the top of `write_slow` instead (no growth) saved nothing: the call is the cost, not the hook behind it. At about 100,000 stores a frame, the most there is to win is 0.2 ms a frame. `mkdd_pipe_test` checks that stores of every size reach the FIFO as their bytes.

About 19 ns a vertex and 45 ns a primitive remain on that CPU (from the same frame as 50-vertex strips: 0.77 ms). Not done, in the order I'd take them on the Pi 4: the vertex is 76 bytes where this game's needs 60 exactly (above), written once by the decoder and copied once more to the GPU's buffer (10 MB a 4-view frame, each way). (The state snapshots that followed are done: shared blocks, above.)

## Pictures between frames

The game steps and draws 59.94 times a second with no time step to scale (`docs/enhancements.md`, A), so a higher frame rate is the renderer drawing the newer of two recorded frames again with its matrices part of the way back towards the older one's. The guest's only part is two leaf functions replaced natively.

- **Tags** (`mtxtag.cpp`): `J3DFifoLoadPosMtxImm` (`80067A64`) and `GXLoadPosMtxImm` (`800C20D8`) write the originals' bytes (`tools/check/mtxtag_check.py --dol … --test …/mkdd_render_test` runs the DOL's own instructions and compares: 424 bytes from 8 calls, the same) and say who loads (the J3D shape packet at `[803ACFD0]`; or the caller's address with bit 1 set) and the view matrix at `803ACF90`. The note travels beside the stream: `gpu.cpp` keeps a ring of (offset in the bytes sent to the decoder, tag, view), and the decoder, reaching an XF load at that offset, gives it to the recorder with the load. Indexed loads (`0x20`) are tagged by their source address with bit 0 set and take the view last given. Beside the tag goes a *context*: `Race2D::drawPlayerMark` (`8014573C`), `drawAlarm` (`80146CF0`) and `drawBomb` (`80147B1C`) are wrapped in the plan, and their native wrappers (`mtxtag.cpp`) mark every load made while they run as following the 3D scene (`gx::kContextFollowsWorld`, `Draw::context`, `gx::draw_context`), which their J2D pictures' tags can't say. `mtxtag::follows_world` answers `hud::set_follows_world` from it.
- **Records** (`gx.hpp`): `Frame::loads` (every load of a position matrix into slots 0-9: tag, slot, view, the matrix), `Frame::views` (each distinct view matrix once, with the first perspective projection drawn under it), `Frame::state_info` (beside each state: the load each slot holds and the view). `Draw::tag` and `gx::draw_tag(const gx::Draw&)` give a draw's tag.
- **Pairing** (`interp.cpp`, once a frame): the n-th load by one tag into one slot pairs with the n-th in the frame before, if both frames have as many (else those stay unpaired: a particle system whose count changed, say). Views pair by order, if there are as many.
- **Blending**: a loaded matrix is the view times the model's own. The view is blended as a rotation (slerp) and an eye position; the model's as rotation, scale and translation apart; a matrix with shear, entry by entry. Normal matrices follow by the inverse transpose of the change, lights move with the view, a view's projection is blended where it changed. Unpaired loads keep their place in the world under the blended view.
- **Not blended** (drawn as the newer frame has them): orthographic drawing (all 2D); untagged matrices; a view that **cuts** (a turn over 60° in a tick, or the eye moving more than a hundredth of the far distance and more than six times its last step); a model that **jumps** (more than a fiftieth of the far distance in a tick: it is drawn at its new place); every view when the number of views changed.
- **Texture matrices** (XF rows 30-59, GX_TEXMTX0-9: scrolling water, conveyor arrows, env maps) are blended too, entry by entry, without tags of their own: a state's matrices pair with those of the older frame's state that drew the same thing, found through the state's first paired position matrix (the older frame's first state that load is in). J3D loads a material's texture matrices just before its shape's position matrices, so a shape that pairs brings its material's with it. A matrix whose translation moves by more than a quarter of the texture in a tick has wrapped (a scroll kept in 0..1) or jumped, and is drawn as the newer frame has it. `Blender::Counts::textures`, `texture_snaps` and `MKDD_INTERP_LOG` count them; the `between_textures` test checks both. Texture matrices loaded into position-matrix slots, and those of flat drawing, are left as they are.
- **Drawing** (`Renderer::render_held`, `render_between(phase)`): the frame is drawn once as ever, which makes its texture copies and its own picture; that drawing remembers each draw's pipeline and texture set. A picture between replays the packets into a second EFB with the blended states: draws as they were, texture copies not made again (their clears are), the last copy into one of three pictures. It starts from the clear the frame before ended with, so a frame whose predecessor didn't end by clearing colour and depth, or that changes pixel format in mid-frame, has no pictures between (the caller shows the frame). So the blur's feedback, the shadows' and the shimmer's copies are made once a tick from the tick's own state, and a picture between samples the newer frame's; the Z the CPU reads back likewise.
- **A wrong foretold copy** (`docs/runtime.md`, the early frame end: at a movie's start or end the copy filter or the EFB's format changes after `endRender`): between frames the foretold copy is the one made and the real one is left out, so the held frame's picture had the old filter for a tick. Now the recorder posts the real copy with the frame it belongs to (`real_copy`, `Frame::serial`), and the renderer, at its next look (each pass, `take_real_copy`, and `render_between`), takes its source, size, filter, gamma and format for that frame's copy and draws the frame's own picture again with it (at phase 1, the frame as drawn, from the clear the frame before ended with); the pictures between after that use it too. Nothing waits for it: until it comes the foretold copy's picture stands, as before; a frame not drawn yet takes it at once. What it cleared is left as foretold (the EFB was cleared so, and the next frame starts from that). `between_real_copy` test.
- **Phases**: 1 is the frame itself, pixel for pixel; 0, with everything paired, the frame before; both tested.
- **Pacing** (`interp.hpp`): `set_frame_rate(rate, display_hz)`; original changes nothing (frames shown as they come, nothing held); display and unlimited hold the newest frame for one tick while pictures from the one before towards it are shown, their phase from `interp::phase(arrived_ns, present_ns)` (even steps k/n where the refresh is n times the game's rate). `main.cpp`'s loop has to call these; that part isn't in my files (the report has the lines).

Measured (`--bench players 300 5 pictures`, the race-shaped frame with tagged matrices and a perspective projection; same machine and caveats as above):

| | renderer CPU a frame | a picture between: CPU | GPU |
|---|---|---|---|
| 1 view (68 draws, 39,400 vertices) | 0.55 ms median (0.41 fastest) | 0.15 ms median (0.10 fastest) | 0.70 ms (the frame itself: 0.69) |
| 4 views (272 draws, 157,600 vertices) | 1.8 ms median (1.4 fastest) | 0.49 ms median (0.29 fastest) | 1.5 ms (the frame itself: 1.5) |

The path without pictures between is as it was: the commit before and this one on the same scene, run back to back under the same load (the machine was at 100%), gave medians of 2.07-2.13 ms against 2.09-2.14 for decoding and recording one view and 0.55-0.57 against 0.46-0.49 for the renderer on the CPU; four views 8.0-8.4 against 6.5-8.4 and 1.5-1.6 against 1.2-1.5.

### Checked on the attract mode's demos (2026-11-26)

Offscreen, `frame_rate = display` with `MKDD_PRESENT_HZ` 120 and 240, through the opening movie, the title and the demo races with one, two and four views; `MKDD_INTERP_LOG=<file>` writes what follows every 600 frames, and `MKDD_DUMP_BURST` gave the pictures looked at.

- **Pairing.** A race frame loads 250-600 position matrices with one view and 1,100-1,450 with four. Before each particle and simple model was told apart, 55-70% paired: particles (`JPADrawRotBillboard` and its kin, `stParticleDraw`) pair by count alone and lose all partners whenever one is born or dies, and every `ExModel::simpleDraw` object lost its partner on one frame in ten, when any object came into or left the view. `mtxtag.cpp` now takes the particle's, the list link's or the model's address from the register its caller keeps it in (`Load::id`; eight call sites, each checked in the function's code): **97-99% pair**, the rest being particles in their first frame. J3D shape packets always paired (skinned drivers and karts too: their matrices arrive through the tagged loader; no indexed loads were seen failing). Nothing was untagged.
- **Scale.** The view's far distance is 200,000 in a race (5,000 in menus). A racing camera moves at most 93 units a tick (1/2000 of far) and turns under 10 degrees; the demos' cuts move it 10,000-30,000 or turn it over 45 degrees. Models move mostly under 1/2000 of far, tailing off by 1/250. The rules were 1/100 (cut) and 1/50 (jump) of far and a 60 degree turn, chosen blind; they are now **1/256, 1/128 and 20 degrees**. The far distance is taken from the projection that reaches farthest under a view: the first one drawn was a close-up's in the opening scenes and made every particle a "jump".
- **Frames without pictures between**: the THP movies (the pixel format changes in mid-frame, every frame), and one frame at each change of scene. Flat scenes (title, menus) now ask for none: there is nothing to move.
- **Looked at** (phases 2/4 and 4/4 of the same two frames, one, two and four views): karts and their skinned drivers whole, dust and item boxes in step with the road, the split screen's seams and the 2D overlay as in the frame. Not judged, because stills can't show it: texture-matrix animation and the copies sampled from the newer frame stepping at 60 against 120 Hz motion, and wheels.
- **Cost in the game** (GTX 1080 Ti, the machine shared): a picture between costs the drawing thread 0.6-0.9 ms with one view (340-420 draws), 0.9-1.0 ms with two (420-550) and 1.0-1.4 ms with four (620-920), and the GPU 1.9-2.1, 1.7-1.9 and 2.2-2.4 ms; the frame itself 2.3-4.2 ms of the thread and 1.7-3.3 ms of the GPU. At 240 Hz that is three pictures a tick: about 4 ms of the thread and 7 ms of the GPU in 16.7.

### To check live (pictures between frames)

1. Pairing in a played race (the demos are checked above): items thrown and held, Lakitu, the award scene.
2. The thresholds on what the demos don't have: Lakitu's rescue, cannons, replays; a false cut shows as one tick without smoothing, a missed one as a smear. A particle's address is reused by the pool: one that dies and one born at the same address in the same tick pair wrongly unless they are far enough apart to count as a jump.
3. Wheels: slerp takes the short way round, so a wheel turning more than half a revolution a tick appears to turn backwards between frames (as it would on film).
4. Skinned models (`driver.bmd`: several slots a packet; indexed loads tagged by address): limbs between frames, and whether the matrices at one address really are one bone from frame to frame.
5. Geometry the CPU places (particle stripes, water, tyre marks: the matrix is the bare view): steps at the tick rate under a smooth camera; look for judder against the course.
6. Texture matrices (scrolls, env maps: now blended; that a race's water and arrows pair, and the wrap threshold) and the effects sampled from the newer frame's copies (shadows a tick ahead of their karts, the shimmer, the blur): visible or not at 120 and 240 Hz.
7. Split screen: that each player's camera is a distinct view in the same order every frame; two cameras with the same matrix in one frame would count as one view and stop blending.
8. Frames that can't be blended at all (no clear at the end of the frame before): none expected in this game; the count should stay 0 outside movies.
9. Latency: one tick more than without, by design; the early frame end at `JFWDisplay::endRender` gives a tick back in every mode (done: `runtime.md`, "Response time").
10. The tag ring (16,384 loads between the guest and the decoder): a load beyond it goes untagged; count them in a busy 4-player race.

## A picture wider than drawn for

The aspect path (enhance.cpp, hud.cpp) widens the game's cameras and stretches the 608x448 frame across the screen. Two things here serve it:

- **Columns** (`Renderer::request_widening`, called from `main.cpp` with `enhance::widening()`): the EFB gets enough whole columns for each of the console's that the stretch isn't a blur: scale x widening, rounded up (16:9 and 21:9: 2 columns at 1x; 3 and 4 at 2x; 32:9: 3 at 1x). Viewports, scissors, clears, copies and the Z read-back place the console's pixels in them; the dither's grid and the fog's range adjustment go by the column scale (a second scale in the push constants); lines' widths and points' sizes stay the line scale's. The `widened` test draws 16:9 and 21:9 frames at 1x and 2x and checks each of those.
- **A survey** (`MKDD_SURVEY=<file>`): every 600 frames, where the perspective draws' vertices fall across the half width in eighths from the middle, and how many draws lie wholly beyond each eighth; and every 20th frame each view's eye and direction. Over the single-view demo race (frames 3,000-5,400, 2026-11-26), the same game frames at four shapes:

| | vertices in each eighth, middle to edge, % | draws wholly in the outer quarter | draws wholly beyond the old 4:3 edge |
|---|---|---|---|
| 4:3 | 33 15 11 11 8 8 8 6 | 7.7% | - |
| 16:9 | 39 18 12 9 6 6 5 5 | 6.7% | 6.7% (beyond 3/4) |
| 21:9 | 33 13 13 12 8 7 7 6 | 9.2% | 14.7% (beyond 5/8; the edge is at 0.57) |
| 32:9 | 37 18 13 8 7 6 5 5 | 7.4% | 24.4% (beyond 3/8) |

  The bands beside the old picture are as full as a 4:3 picture's own edges, at every shape: nothing in this race is still culled against 4:3. (One ten-second stretch alone, the race's start, read thinner at 21:9; over the race it doesn't.) Split screen and the other courses weren't surveyed.

### Two things seen in widened pictures (2026-11-26)

- **Faint bands across the lower part of player 1's view in a two-player countdown at 16:9 and 21:9**: ours. Captured (`work/render2/p2c.dff`, `MKDD_DFF=...@803CBDA0=8+6.5`: 6.5 s after the race scene starts, the countdown at "2"), they are five flat quads, 226 by 6 pixels at y 185-265, alpha fading from 75 to 0 towards the middle: the countdown number's speed lines, trailing it in from the left. The number is drawn in perspective, so it stays where it is on a widened picture; the lines are flat, their centre in the left third, so the HUD's placing (`hud.cpp`) pulled them towards the left edge and left them across the views as stray bands. A flat part mostly at a side that reaches the view's middle (within 5% of the width) is now squeezed about the middle instead, which keeps its inner end where it was drawn (`enhance_test`'s anchors). `mkdd_render_test --picture-dff <file> <out> [loops] [scale] [widening]` draws a capture with the HUD placed as in the game.
- **A pale rectangle behind Lakitu's lap board at 3x** (`work/os/review/keep/babypark-scale3-lakitu-rectangle.png`): not reproduced. The countdown capture above has Lakitu with his lights and shows no rectangle at 1x or 3x; the lap board is up only for a moment after the line is crossed, and no trigger for that moment was at hand (a lap counter's address would do: `MKDD_DFF=<file>@<address>=<lap>+<seconds>`). Next: capture it so, and compare its picture at 1x and 3x.

## Tools

- A GLSL compiler isn't on the build machine and CI installs `glslang-tools`. For the shader change here, glslang 15.1.0 was built from source in scratch (`work\gpu\bin\glslangValidator.exe`), and `tools/build_shaders.py` used as it is. Every shader's SPIR-V bytes changed with the compiler's version; only `copy.frag` changed in source.
- `mkdd_render_test` links `gpu.cpp` and feeds it bytes (`decode_commands`), so scenarios can be written as the game writes them: BP, XF and CP loads, display lists in guest memory, raw indexed loads. The few OS functions `gpu.cpp` calls are stubbed in the test.
- `tools/check/picture_scenes.json` holds one scene, the first seconds with no input; the rest need the running game to script. `picture_check.py` itself is as seeded (its `--save` copies a `nand/` folder, which this game won't have).

## To check once the game runs

1. The boot: `GXInit`'s register writes all land in mapped ranges; the first `GXDrawDone`; the logo. If `GXAbortFrame` is ever reached, the log says "the FIFO is reset".
2. The first frame's picture against Dolphin (`picture_check.py`): the EFB clear by Z texture, 608×448, the interlaced filter. A GameCube `.dff` has not been through Dolphin's player yet.
3. Indirect textures (a green shell, the menus' title lines), then the screen effects in a race: shadows (EFB alpha, subtract blending), ghosts, blur, shimmer (Z16 through swap table 1), the masks.
4. The 2×2 depth read-back: player marks and the lens flare should hide behind scenery in the same frame as on the console; the log names any read not answered within 250 ms.
5. Particles with clipping off, and `stParticleDraw`'s lines (texture offsets).
6. The real cost of a frame (`MKDD_DFF` a race frame, then `--bench-record`), and how many draws remain after joining: a J3D shape changes matrices between packets, not within one.
7. Pipelines: `pipeline_seed.inc` is empty; make it from a play-through's `pipelines.keys` (`tools/make_pipeline_seed.py`) so a first run compiles nothing while playing.
