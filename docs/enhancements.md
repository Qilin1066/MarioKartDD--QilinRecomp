# Enhancements: frame rate, picture shape and the rest

Written 2026-11-25 from the code alone: the game doesn't run in this port yet, so nothing here has been seen working. Every address is in the supported build (GM4E01 rev 0, `main.dol` SHA-256 `e96b8578…`). Names are the debug map's (`debugInfoS.MAP` on the disc) as ported to this build, and doldecomp/mkdd's (CC0) for classes and fields; each function relied on below was read in `qilin insns` output and matches what its name says, and the text says what settled it where that isn't the function's own body. `tools/analysis/enhance_scan.py` answers the questions asked of the listing (callers, references to a data address, float constants and who loads them, a singleton's field); the commands are at the end.

Two findings change the task as it was set:

- **The game has no 30 Hz mode for split screen.** Its logic takes one fixed step per pass of the main loop, the loop waits for one retrace per pass in every mode, and nothing in it looks at the number of players. The only frame-rate switch in the program is a *draw* skip (draw on every second pass, logic on every pass) that LAN play turns on for its fullest configurations. On this port a three- or four-player race therefore runs and draws at 60 with no work at all. (The console may well have missed retraces there; if it did, the game ran slow, because nothing compensates.)
- **There is no time step anywhere.** High frame rates can't come from feeding the game real time as Ghoulies does; they have to come from drawing more pictures than the game computes.

## A. Frame timing

### The main loop

`System::run` (`801D0B5C`, 0x110 bytes, called from `main` `801CC6A8`) is one flat loop with `beginFrame`, `endRender` and `endFrame` inlined into it; the loop's head is `801D0B74`. One pass, in order:

| Address | Call | What it does |
|---|---|---|
| `801D0B74` | `CardMgr::probe`, `DVDGetDriveStatus` and a switch to `ErrorViewApp::call` | housekeeping |
| `801D0BE0` | `GameAudio::Main::frameWork` (`80114878`) if active | the audio's per-pass work |
| `801D0C04` | `JFWDisplay::beginRender` (`8007A984`, through the vtable of `System::mspDisplay`, `[803CBD70]`) | **waits for the retrace**, copies the last picture out, clears the EFB |
| `801D0C0C` | `NetGameMgr::adjustFrame` (`801D8A64`) | LAN: holds the pass until every console's pads for it have arrived |
| `801D0C10` | `Clock::move` (`801CBF4C`) | a 300 Hz wall clock from `OSGetTick` |
| `801D0C1C`, `801D0C24` | `PadMgr::framework`, `NetGameMgr::framework` | pads read here |
| `801D0C28` | `AppMgr::draw` (`801D1B10`) | draws the state the *previous* pass computed |
| `801D0C3C` | `JFWDisplay::endRender` (`8007AE78`) | ends GX work for the picture |
| `801D0C40` | `AppMgr::calc` (`801D1B68`) | **the logic step** |
| `801D0C54` | `JFWDisplay::endFrame` (`8007B1D8`) | waits for the GPU to finish (`JFWDrawDoneAlarm`, `8007BAD0`) |
| `801D0C64` | `MoviePlayer::drawDone` if a movie plays | |

Draw comes before calc: a pass draws what the pass before computed.

**The wait.** `beginRender` calls `waitForTick(mTickRate, mFrameRate)` (`8007B3C4`) with the display's fields `+0x20` and `+0x1C` (`8007AA38`). With a tick rate it sleeps on `OSGetTime`; with none (the case here) it reads retrace counts from `JUTVideo`'s message queue (`[803CAD40] + 0x38`, posted by `JUTVideo::postRetraceProc`, `80023DD8`, from the VI retrace callback) until one reaches `nextCount` (`[803CAE24]`), then sets `nextCount` to that count plus the frame rate (0 counts as 1). The frame rate is set to 1 and the tick rate to 0 where the display is made (`8007A904`, `8007A908`) and **neither is written again**: `enhance_scan.py field 28 803CBD70` and `field 32 803CBD70` find no store through either pointer to the display (`System::mspDisplay` `[803CBD70]`, `JFWDisplay::sManager` `[803CAE08]`), and doldecomp's `setTickRate` inline has no user. So: one pass per retrace, 59.94 a second, in every mode; a pass that overruns a retrace simply starts late, and the game is that much slower.

**The draw skip.** `beginRender` ends by counting passes: `[display+0x3C]` is incremented and compared with `[display+0x38]` (`8007ADA4`-`8007ADDC`); the byte `[display+0x40]` (doldecomp: `mIsWaiting`; it means "this pass draws") is set when the count reaches the interval, and the count restarts. Everything that draws tests the byte: `RaceApp::draw` (`801CD8E8`) calls `RaceMgr::drawRace` only when it's set (`801CD908`), and `beginRender`/`endRender`/`endFrame` skip their EFB and XFB work without it. The interval is written in four places and nowhere else (`enhance_scan.py field 56 803CBD70`):

- 1 at construction (`8007A948`), in `System::reset` (`801D0854`) and in `ErrorViewApp::call` (`801D23E0`);
- 2 in `RaceApp::calc` (`801CD9A4`), as the race starts (state 1 → 2, once `NetGameMgr` reaches state 13), **if the byte `[NetGameMgr+0x12EE]` is set**; back to 1 when the race app ends (`801CDB88`).

That byte is written only by `LANEntry::calc` (`801E5DDC` sets it, `801E5DEC` clears it): set when `[LANEntry+4] == 4` and `[LANEntry+8]` is 7 or more, or is 6 with the byte `[LANEntry+0x15]` not 3 (`801E5DA8`-`801E5DD0`). `LANEntry.cpp` isn't decompiled, so the fields have no names; by their use they are the LAN game's size (consoles, screens or karts). In those configurations the logic still runs every pass (`RaceApp::calc` doesn't test the draw byte) and the picture is drawn every second one: 30 pictures a second over 60 logic steps.

**What decides the rate in three- and four-player split screen: nothing.** `RaceMgr::calcRace` (`80189AF8`), `frameWork` (`80189BB0`), `updateRace` (`80189CA0`) and `drawRace` (`8018922C`) are as doldecomp has them: one step and one draw per call, every camera every time. The console count changes detail instead: `TexLODControl`'s level is 1 for three or more (`RaceMgr`'s constructor), drivers get a second LOD, area lights are off for two or more (`RaceMgr::getProcLevel`). Those are the measures that let the console hold 60, and the code has no other.

### Is anything time-based?

No global step exists, and no table of rate-dependent constants either; rates are literals in the code that uses them.

- **No function is given a time step.** Counted from the mangled names in the symbol list, the game's own libraries have 277 `calc` methods, 264 of them with no argument at all, and 101 `update`, 90 with none; all three `framework` and all three `exec` take none. The 13 `calc` with arguments take a pad, an area, a position, an index or an effect's own parameter (`TWipeBase::calc(int, float, TColor)`, `BombEffectObj::calc(float, float, …)`, `DarkAnmPlayer::calc(int, float)`); the four `frameWork` with arguments are audio managers given speeds and flags. None is a step length: with no global step to pass (next point), there's nothing a caller could give them.
- **No 1/60 constant is loaded anywhere.** `enhance_scan.py floats "1/60" "1/30" "1000/60" "59.94~0.01"` finds 1/60 twice in the data (`802EF3D0`, `802EF3DC`, inside the embedded system archive) with no reference from code, 1/30 loaded by three functions (`DriverModel::SetDriverOffsetPosition`, `Race2D::getGoalRankPos`, `ShoreSoundMgr::setTide`: layout and blend factors), 1000/60 nowhere, and 59.94 only in the movie player's pacing (`PlayControl`) and `JASProbe::stop`.
- **The race clock is a frame counter.** `KartChecker::incTime` (`80186D30`) increments `[checker+0x7C]` and stores `frames × 1000 / 60` as milliseconds in `[+0x84]` (`mulli 1000`, `divw` by 60 at `80186D64`). The same conversion is in `KartChecker::setLapTime` (`80186904`; it also places the line crossing within the frame by stepping along the frame's movement times 0.06, `[803CE8FC]` loaded at `801868C0`: one millisecond's share of a sixtieth of a second), `calcRabbitTime` (`80187600`) and `RaceDirector::doRunning` (`8019FBC0`). Nothing else turns frames into time.
- **Kart physics** (`KartCtrl::DynamicsStage1` `8029FD40`, `DynamicsStage2` `802A01D8`, called once each from `RaceMgr::frameWork`), **items, map objects and effects** (`ItemObjMgr::calc`, `GeographyObjManager::calc`, `JPEffectMgr::calc`, all once per step from `frameWork`/`updateRace`) advance by one implicit step.
- **Animation**: `J3DFrameCtrl::update` (`800571B4`) adds the control's own rate to its frame, per call. **Particles**: `JPAEmitterManager::calc()` (`80081A50`) has no time argument in this JPA.
- **Ghosts and replays are pad recordings**: `PadRecorder::nextFrame`/`framework` store and fetch one `KartPadData` per logic frame (`RaceApp::ctrlRecorder`, `801CDF0C`, between `calcRace` and `updateRace`), and a ghost is that recording run through the physics again. Any change to the step would break every ghost on a memory card, and every record's comparability.
- **LAN is lockstep**: `NetGameMgr::adjustFrame` loops on `VIWaitForRetrace` (`801D8C00`) until all consoles' pads for the frame are in, and, at race phase 17, sums every kart's position into `[NetGameMgr+0]` (`801D8B40`-`801D8B70`): a desynchronisation check. Two consoles must compute identical frames.
- **Real time is used** only by `Clock` (`801CBF4C`: memory-card message waits in `CardAgent`, the logo's timing in `LogoApp`), by the THP movie player and by the audio's own DSP-driven timing.

### Options

**1. Decoupled drawing with interpolation (recommended).** The logic stays exactly as it is, 59.94 steps a second off the VI retrace; the renderer shows pictures between the game's.

*Where.* Two levels were weighed.

- *In the game (J3D)*: run `AppMgr::draw` again with blended matrices in guest memory. J3D here keeps world matrices and the view matrix apart until the draw (`J3DShapeMtxConcatView::loadMtxConcatView_*`, e.g. `800774CC`, multiply the packet's base matrix by the model's draw matrix at draw time), which helps; but particles (JPA and Sato's `stParticle`), the course's own drawing (`Course::drawBase`), `ExModel::simpleDraw` (`801832F0`, which a dozen object classes use) and every 2D element keep their state elsewhere, each would need its own snapshot, and a second run of the draw path isn't free of side effects (the blur screen's feedback, shadow and Z captures). It also costs the game's draw path and our vertex decoding once per shown picture on the guest's thread.
- *In the command stream*: the recorder already turns a frame into draws that each name a `gx::State` holding the XF matrices, the projection and the viewport, with vertices decoded (`gx.hpp`). The renderer can keep two consecutive frames and draw frame N's draw list with matrices moved part of the way back towards frame N−1's, as often as the display wants, without the guest's thread taking part.

The stream level is the one to build, with identities supplied from the game's side, because pairing draws between frames by order alone breaks whenever something appears, disappears or is sorted differently (J3D sorts translucent packets by depth).

*Identity.* Every position matrix the game loads goes through one of three doors:

| Door | Address | Used by | Identity it gives |
|---|---|---|---|
| `J3DFifoLoadPosMtxImm(Mtx, id)` | `80067A64` (10 call sites) | every J3D shape drawn with a concatenated view (`J3DShapeMtxConcatView`, the billboard variants, `J3DShape::drawFast`) | the shape packet being drawn, `[j3dSys+0x40]` = `[803ACFD0]` (set by `J3DShapePacket::draw`, `80050608`), plus the slot `id` |
| `GXLoadPosMtxImm(Mtx, id)` | `800C20D8` (65 call sites) | J2D, JPA particles (one load per particle in the billboard draws), `stParticle`, `ExModel::simpleDraw`, `Course::drawBase`, the screen effects, the lens flare | the caller (LR) and the matrix's guest address |
| indexed loads (CP command 0x20) | in display lists | J3D shapes with GPU-indexed matrices (`loadMtxIndx_PNGP`, `80076D78`) | the matrix's own address in the model's buffer, which the decoder already computes |

Both functions are leaves that write opcode 0x10, `(11 << 16) | id × 4` and twelve words to the write-gather pipe (read at the addresses above; J3D passes `id` already multiplied by three). Replacing both natively (same bytes into the FIFO, plus a note to the recorder) tags every matrix with who loaded it. At the same moment the replacement reads the current view matrix, `j3dSys.mViewMtx` (`803ACF90`, 48 bytes; `RaceMgr::updateRace` and `RaceDrawer::drawSceneFirstStage` set it per camera), so a model-view matrix can be split into view and model.

*Blending.* For a pair (same identity, same camera) with model-views MV₀, MV₁ and views V₀, V₁: M = V⁻¹·MV for each; the view is blended as a rigid motion (rotation by slerp, eye position linearly), the model by rotation, scale and translation apart (a plain blend of matrix entries collapses anything that turns a large angle in a tick, and a kart's wheel turns about half a revolution per tick at speed); MVα = Vα·Mα. Normal matrices are recomputed from the result, the projection's entries blended (the field of view changes with boosts), light positions and directions carried by Vα·V₁⁻¹, texture matrices blended unless an entry jumps by more than a half (a scroll wrapping). A draw of frame N with no partner keeps its model matrix and takes the blended view (it stays put in the world and doesn't judder against it); a draw that was only in frame N−1 is dropped a tick early. Vertices come from frame N; where a paired draw's matrix is the bare view or the identity (geometry the CPU placed in world or view space: particle stripes, water, tyre marks) the same view correction applies and its own motion steps at the tick rate. Flat draws (orthographic projection: all of J2D, fades, wipes, screen effects) aren't blended.

*What breaks, and the guard for each.*

- **Camera cuts** (the pre-race fly-by `KartDemoCam`, replays, rescue and cannon views: `KartCam::DoCamera`'s mode switch, `8029F2D8`): blending across one smears the picture. A cut is a change of the camera's mode bytes (`[cam+0xE1]`, `[cam+0xE2]`) or an eye movement or turn far beyond the last tick's; for that camera's viewport the new frame is shown unblended.
- **Teleports** (Lakitu's rescue, cannons, warp pipes, an item changing hands): a paired draw whose model moves more than a few times its last step snaps.
- **Appearing and disappearing objects**: handled as above (no partner).
- **Feedback effects**: `BlurScreen` (`RaceDrawer::drawPreScene`, `801A1D0C`: capture, blend, capture) accumulates per picture drawn; in-between pictures must not feed it. The renderer keeps the texture copies an in-between picture makes apart from the real frames' (a copy that a later frame reads before overwriting is "persistent"; real frames are rendered once each for those even when not shown).
- **EFB copies within a frame** (shadows `ShadowScreen`, heat shimmer `ShimmerScreen`, `GhostScreen`, `MaskScreen`, Z captures for the lens flare `ZCaptureMgr`): replayed with the blended matrices like any draw; nothing reads them back on the CPU (`GXPeekZ`/`GXPeekARGB` aren't linked into the DOL), so the logic can't see the difference.
- **2D timers and the HUD**: step at the tick rate, as now. The race clock shows frames × 1000/60, so it is exact.
- **Movies**: THP at their own rate, flat: untouched.

*Latency.* Showing a blend of N−1 and N while N is the newest known costs up to one tick (16.7 ms) over showing N at once. The loop's order gives it back: a frame's last draw command is issued in `endRender`, but its copy to the XFB (what ends a frame for the recorder) happens in the *next* pass's `beginRender` (`8007AC64`), after `AppMgr::calc` and the wait for the retrace, a whole tick later. Ending the recorder's frame when `endRender` returns (`801D0C40` is the next instruction) and treating the following copy as already shown removes that tick, in every mode. To verify on the running game: the double-buffer exchange clears instead of copying when the last picture hasn't been displayed yet (`8007AB58`-`8007AB64` compare the drawn and displayed buffer indices), so the early end must follow the same rule.

*Cost.* The guest and the recorder work once per tick as now. The renderer draws n pictures per tick (n = refresh / 59.94) from vertex buffers it already holds, and blends a few hundred states' matrices per picture; it holds two frames (tens of megabytes). At 60 Hz output nothing changes and nothing is blended. A GameCube scene n times over is light for any desktop GPU; the Raspberry Pi 4 would stay at 60.

**2. Running the logic faster with scaled steps: not possible.** There is no step to scale. It would mean rewriting, function by function, every one of the per-frame increments above: the kart dynamics' integration and its 1/30-style coefficients, AI and rubber-banding, every item, every map object's and animation's rate, particle lifetimes, counters compared with literal frame counts (`RaceDirector`'s 30- and 1800-frame waits), the race clock's ×1000/60 and the recorded-pad model of ghosts and replays, whose recordings hold one pad state per 1/60 s. Records and ghosts would stop being comparable with the console's, and LAN play with a console or Dolphin would desynchronise at once. Running the unchanged step more often simply speeds the game up.

**3. Sixty logic frames in three- and four-player modes: already so.** See above; nothing to patch. The related real case is LAN's draw skip, which is one word: keep `[display+0x38]` at 1 (`draw_every_tick` in `enhance.cpp`, called once a pass before `beginRender`). It can't affect lockstep: only drawing tests the draw byte, and consoles exchange pads per logic frame whatever each draws.

### Recommendation and hooks

The menu's four settings, for this game:

- **original**: as the console. Logic and pictures at the VI's 59.94; LAN's draw skip kept.
- **60**: every tick drawn (LAN's skip off), pictures shown as they come. No interpolation, no added latency.
- **display**: one picture per refresh, interpolated when the refresh is above 60.
- **unlimited**: interpolated pictures as fast as they render, without vsync.

Hooks, in the order to build them:

1. `draw_every_tick()` once per pass (from the retrace wait's replacement, or wherever the os/threads side sees the game's thread enter `beginRender`), when the setting isn't original. Done in `enhance.cpp`.
2. Early frame end: the recorder closes the frame when `JFWDisplay::endRender` returns rather than at the XFB copy. Needs a way to run native code after a lifted function (see Requests).
3. Tags: native `J3DFifoLoadPosMtxImm` (`80067A64`) and `GXLoadPosMtxImm` (`800C20D8`), each writing the original's bytes to the FIFO and calling `gx::tag_matrix(slot, identity, view matrix)`; the recorder stores the tag and the view with the draw's state. The decoder tags indexed loads with the source address itself.
4. Renderer: keep frames N−1 and N; `render(frame, previous, alpha)` blends as above; the presenter picks alpha from the clock (fixed k/n steps when the refresh is a multiple of the tick rate).
5. Guards: cuts, teleports, persistent copies.

Test plan, once the game runs:

- *Determinism first, without the feature*: a time-trial ghost replays to the same finish time twice; a headless run from a save with scripted pads logs each kart's position per tick (`KartCtrl::GetBodyPos`, `802B12C0`, as `adjustFrame`'s checksum uses) and two runs match bit for bit.
- *With interpolation on* (any refresh): the same per-tick log is identical to the run without it. This must hold by construction (the guest isn't touched); it's the regression test for the tag replacements, which are the only guest-visible part: the FIFO bytes of a frame, captured with the replacements and with the lifted originals, must be identical.
- *LAN skip off*: a LAN race's position checksum sequence equals the same race's with the skip on.
- *Pictures*: at alpha = 1 an interpolated picture equals the plain frame pixel for pixel; at a camera cut no picture mixes two shots (compare against both neighbours); frame-time graph at 120 and 144 Hz is flat; the blur and shimmer effects look the same at 60 and 240.
- *Latency*: with the early frame end, a pad edge to the first changed picture, in ticks, before and after.

## B. Picture shape

### Where the shape comes from

The frame is **608×448**, not 640×480: `SystemData`'s tables (`.rodata`, `80336EF8` on) hold the 3D viewport {0, 0, 608, 448} (`80336EF8`), the scissor (`80336F08`), the award scene's scissor {0, 34, 608, 380} (`80336F18`), the 2D orthographic box {0, 0, 608, 448} (`80336F28`) and a sub-screen {437, 312, 149, 112} (`80336F38` floats, `80336F48` integers). The four render modes follow (`80336F58`, `80336F94`, `80336FD0`, `8033700C`: 608 wide, 448 high, shown across 666 of the VI's 720 columns from x 27, y 16), so the console shows the picture at 1.35:1 inside the 4:3 frame, with a border all round.

`__sinit_systemData_cpp` (`801D656C`) derives the rest at start-up:

| Address | What | Value |
|---|---|---|
| `[803CBE08]` | `scAspect` = 608/448 | 1.357 |
| `[803CBE0C]` | `scAspectDiv2` = 608/223 | 2.726 |
| `[803CBE10]` | `scAspectSub` = 149/112 | 1.330 (never read: `enhance_scan.py refs 803CBE08-803CBE14`) |
| `80352A28` | two-player viewports {0, 0, 608, 223}, {0, 225, 608, 223}; their scissors at `80352A48` | |
| `80352A68` | four-player viewports, 303×223 each at x 0/305, y 0/225; their scissors at `80352AA8` | |

The two aspect floats are read only by `System::getPerspAspect` (`801D0F50`), `getPerspAspectDiv2` (`801D0F58`) and `getPerspAspectDiv4` (`801D0F60`, which returns `scAspect`: a quarter has the whole's shape). Their callers are two functions:

- **`KartCam::DoPort`** (`8029E0A8`; ten calls, one per kind of port: start demo, award, one screen, sub-screen, halves, quarters) stores the value in the camera at `+0x168` (e.g. `8029E194`) together with the viewport and scissor. It runs when a race is set up or reset (`KartCtrl::DynamicsInit` `8029FB0C`, `DynamicsReset` `8029FCFC`), not per frame.
- **`TFlareLine::resetFlares`/`calc`** (`8026EE04` on, `8026F084` on), every frame, as the ratio of the mode's aspect to the full screen's.

From the camera the value reaches everything else:

- **Projection**: `KartCtrl::DrawKartInit` (`802A02D8`) and `SetGrafPort` (`802A045C`) give it to a `GrafPort`, whose `setPort` (`802B3350`) calls `KartMtxPerspective(mtx, fovy, aspect, near, far, mirror)` (`801A77E8`: half-height = near·tan(fovy/2), half-width = that × aspect, then `C_MTXFrustum`) and `GXSetProjection`. It is the race's only projection.
- **Culling**: `KartCam::CheckClipper` (`802B3200`, called from `DoCamera` every tick at `8029F384`) compares the camera's aspect, far plane and field of view with the copies at `+0x16C`…`+0x178` and, when any differs, refills the `J3DUClipper` at `cam+8` (aspect to clipper `+0x50`) and calls `calcViewFrustum` (`8000A260`), which builds the four side planes from fovy and aspect. `J3DUClipper::clip` (`8000A3C8`) is the only frustum test in the game: called by `Course::calc` (`8017B710`, the course's sectors) and `ExModel::clipBySphere` (`80183934`), whose 21 callers are `KartCtrl::DoLod` (15: karts, drivers, their parts), `GeographyObjManager`'s three `viewCalc`s (every map object), `ItemObjMgr::viewCalc` and `StringObj::drawSimpleModel`.
- **Screen positions for 2D that follows 3D**: `SiUtil::getScreenPos` (`802671C8`) rebuilds the camera's projection with its aspect; `Race2D` (player marks `80149B40`, the alarm `80146EFC`, particles, bomb marks) and `TWipeCircle::calcMain` place 2D by it.

Other projections in the program, none of them in a race: `CharacterSelect3D::draw` (`80167774`, `80167B54`; the aspect is its float argument: `SceneMenu::draw` passes a width over a height from its parameters at `801597F4`, `SecretScene::draw` the float `[803CC584]` set by its static constructor), `J2DPerspGraph::setPort` (`80027730`) and `MVGrafPort::setPort` (`801B1D74`) with their own viewport's shape, `MdlViewer::draw` (debug). `C_MTXLightPerspective` (`JPEffectMgr::draw` `801FDBB0`, `draw_thunder`, `SecretScene::draw`) projects textures, not the scene.

No other constant encodes the shape: `enhance_scan.py floats "608/448" "608/223" "4/3" 608 448 640 480 304 224 303 223` finds 4/3 only in the audio (`JASTrack::updateTempo`), 608 and 448 as floats only in the tables above, and 640/480 in JSystem defaults that the game overrides (`J2DScreen`'s constructor, `JUTConsole`, `SysDebug`) and in `EffectScreenMgr`'s constructor (`803D056C`, its own orthographic box for full-screen quads). Scattered 304, 224, 240 and 320 in `Race2D`, `Result2D`, `Goal2D` and the menus are 2D layout positions.

### Hor+ at any shape

Let *w* be how many times wider the picture is shown than the game drew it for (16:9 on a frame shown at 4:3: 4/3; 21:9: 1.75; 32:9: 2.67).

- **Do**: return `scAspect × w` and `scAspectDiv2 × w` from the three getters (native replacements, `enhance.cpp`); for a change during a race, multiply each live camera's `+0x168` by the change (`KartCtrl` `[803CC588]`, cameras at `+0x200 + 4i`, count from `[[RaceMgr 803CB7E8]+0x38]+0x20`). Projection, clipper, course and object culling, LOD selection, 2D that follows karts and the flare then agree, because they all read the camera. The vertical field of view is untouched: Hor+.
- **Don't change the frame.** The EFB stays 608×448 and the game's viewports, scissors, EFB copies and half-size effect textures (`BlurScreen`, `ShimmerScreen`, `GhostScreen`, `MaskScreen`, `ShadowScreen`, `ZCaptureScreen`, all sized from `JUTVideo`'s 608×448) stay as they are; the display stretches the finished frame by *w*, as a widescreen TV does, and the renderer's horizontal resolution multiple grows by *w* so pixels stay square. This is what makes the change small: nothing that works in frame coordinates has to know.
- **Split screen** needs nothing more. Halves are 608×223 (2.73:1 already; ×4/3 = 3.63:1), quarters 303×223. A side-by-side two-player layout would be possible by rewriting the tables at `80352A28`/`80352A48` (they're in `.data`, filled at start-up), but the two-player HUD is laid out for stacked halves, so it isn't proposed.
- **Outside races** (logo, menus, movies, LAN screens): *w* = 1, the frame shown at its own shape between black bars. The switch falls on the black frames between apps (`AppMgr::setNextApp` stalls drawing for a frame, and scenes fade out and in). "In a race" is `RaceMgr`'s manager pointer `[803CB7E8]` being set, which also covers the award ceremony and the staff roll, both of which draw through the race's cameras.

What the widening distorts, since flat drawing is stretched with the frame:

- **The HUD and other flat elements** are *w* times too wide. Fixed in the renderer, as in Ghoulies: a draw with an orthographic projection that covers less than nine tenths of its viewport's width is squeezed by 1/*w* about a point: the nearer side edge of its frame of reference when its centre is in the left or right third (**edges**), the middle of the 4:3 area (**middle**), or not at all (**stretched**). The frame of reference is the draw's scissor box when that is narrower than the screen (`Race2D::draw`, `80145CB0`, sets scissors itself; per-player HUDs in quarters must keep to their quarter), else the screen. Full-width flat draws (fades `JUTFader`, wipes, the blur, mask and shimmer quads, letterbox bars) keep the stretch, correctly.
- **2D that follows 3D** (`Race2D::drawPlayerMark` `8014573C`, `drawAlarm` `80146CF0`, the bomb and particle marks, the lens flare's sprites `TFlareLine::draw`) is already in the right place in the stretched frame and must be squeezed about its *own* centre, never moved to an edge. These come through `GXLoadPosMtxImm`, so the same tag that serves interpolation (the caller's address) tells the renderer which rule applies.
- **Circles in screen space**: `TWipeCircle` (the iris wipe) becomes an ellipse; `TFlareLine` corrects by a ratio of two getters, which the widening cancels out of, so the flare's line is laid out as at 4:3 and stretched. Cosmetic; the wipe can be given the same own-centre squeeze if it bothers anyone.
- **J2D layouts** (`.blo` screens under `J2DScreen::draw`, `80030F4C`) hold absolute pane positions in the 608×448 box; panes have no anchors, and the game moves many of them from code (`Race2D`'s `getStartLapTimePos` and the like). Anchoring per draw in the renderer avoids touching any of that, and keeps text lines whole if the Ghoulies rule (pieces on one line move together) is carried over.

At the GPU level alone (scaling projection loads in XF) the picture would widen but the CPU would still cull to the 4:3 frustum: course sectors, karts and objects would pop in at the sides, and the HUD's kart-following marks would sit in the wrong places. Guest values are the right level for the 3D; the renderer is the right level for the flat squeeze and the stretch. Both, each for its half.

Test plan: at *w* = 1 the frames are bit-identical with and without the replacements (FIFO capture); at 16:9, 21:9 and 32:9 a circle in the world (an item box's shadow, a wheel side-on) is round; driving past scenery with the camera turned shows nothing appearing inside the picture's edge (the clipper follows); a player mark stays over its kart at the screen's side; the per-tick position log is identical at every *w* (culling must not reach the logic: to confirm, since `DoLod` chooses models by it, and an animation skipped for a culled object would be a logic difference visible in that log).

## C. Other enhancements (no work now)

**The game's LOD and draw distance.** Detail is cut by console count (`TexLODControl` level 1 for three or more consoles, the drivers' second LOD level, area lights off for two or more or LAN: `RaceMgr`'s constructor and `getProcLevel`) and per object by `KartCtrl::DoLod` (`802B0E40`) and the map objects' `viewCalc` clip tests. A "full detail in split screen" option would force the one-console choices; they are constructor-time decisions, so it's a few compares to override, each to be checked for memory use (the heaps are sized for the console).

**30 Hz animation or particles.** None found: animation controls and emitters step once per logic tick in every mode. Only THP movies are below 60.

**Texture filtering and LOD bias.** The game sharpens or softens by its own bias (`ExModel::setLODBias` `80182384`, `TexLODControl::getLODBias` `8019111C`, a course flag for geography). Anisotropic filtering is already a renderer option in the base; with it on, the game's positive biases (which blur distant ground to hide shimmer on the console) could be dropped to zero in the sampler.

**The interlace filter.** The normal render mode (`80336F58`) has the vertical filter {32, 0, 32, 0, 0, 0, 0}: each output line is half the line above and half itself, the deflicker that makes the picture soft; the progressive one (`80336FD0`) has {2, 2, 17, 22, 17, 2, 2}, a mild blur; the movie modes (`80336F94`, `8033700C`) none. `JFWDisplay` passes them to `GXSetCopyFilter` (`8007AC0C`, `8007B040`, `8007B130`) and they reach the renderer in `gx::Copy::filter`. A "sharp" option ignores the filter for copies to the XFB.

**LAN without a Broadband Adapter.** The DOL carries the SDK's whole IP stack (`IP*.c`: ARP, DHCP, UDP, TCP, PPP) over `eth.c`'s BBA driver, and `NetGameMgr`/`BBAMgr` use its socket calls (`SOSocket` `800D82E8`, `SOBind`, `SOSendTo` `800D9ACC`, `SORecvFrom` `800D986C`) and an SSDP-style search. Two levels would work: replace the driver's ten entry points (`ETHInit` `800E282C`, `ETHSendAsync`, `ETHSetRecvCallback`, `ETHGetMACAddr`, …) and carry Ethernet frames between instances over host UDP, which keeps the guest stack and should interoperate with Dolphin's BBA modes; or replace the socket layer with host sockets. The first is smaller and exact. Lockstep at 60 with `adjustFrame`'s wait means the usual LAN-only latency limits.

**The progressive-scan prompt.** `LogoApp::calc` asks `VIGetDTVStatus` (`801CFBE0`); only if it's non-zero does it look at `OSGetProgressiveMode` and the B button and show `PrintProgressive`'s question. Reporting no component cable skips the prompt for good, leaves the game in its interlaced mode, and the only visible difference, the soft filter, is the renderer's to ignore.

**Load times.** `JKRDvdRipper::loadToMainRAM` calls `VIWaitForRetrace` between chunks (four sites) and `JKRDvdAramRipper` likewise, so loads are paced by the display, and `AppMgr` stalls a frame per app change while ARAM loads finish. With instant disc reads the retrace waits are most of what remains; shortening them while a load is in flight (never during a race or a LAN session) would cut loading screens. To measure first.

## The interface this needs

Proposed text for `mkdd/src/enhance.hpp` (the host's file), beyond its `aspect_supported()` and `frame_rate_supported()`, both true:

```cpp
namespace mkdd::enhance {
// The picture is shown `times` as wide as the game drew it for (1 to 3): the game's cameras
// widen to match. `running`: the game has started (then from its thread, between passes).
void set_widening(float times, bool running);
float widening();
// Undo LAN play's draw-on-every-second-pass; once a pass, before JFWDisplay::beginRender.
void draw_every_tick();
// A race (or the award scene, or the staff roll) is on: the widened picture applies.
bool in_race();  // [803CB7E8] != 0; not in enhance.cpp yet
// System::getPerspAspect, getPerspAspectDiv2, getPerspAspectDiv4.
std::span<const qilin::rt::Provided> replacements();
}
```

Requests to other roles:

- **core** (`data/<id>/plan.tsv`): mark `801D0F50`, `801D0F58`, `801D0F60` replaced (then add `mkdd/src/enhance.cpp` to `mkdd_runtime_sources` and `rt::provide(enhance::replacements())` in `main.cpp`); later `80067A64` and `800C20D8` for the tags. A way to run native code before and after a *lifted* function (a wrap entry in the plan: the body lifted under another name, the address dispatched to the native wrapper) would serve the early frame end at `JFWDisplay::endRender` and any future tag, without rewriting guest functions natively.
- **host/devices** (`gpu.cpp`, `recorder.cpp`, `renderer.cpp`, `gx.hpp`): a tag and a view matrix per position-matrix load carried into `gx::State`; the flat-draw squeeze; two-frame rendering with alpha; the frame's end movable to `endRender`.
- **os/devices**: `VIGetDTVStatus` returning 0 (no prompt).

## Reproducing the analysis

```
qilin insns <disc>/sys/main.dol -o listing.txt
S="python tools/analysis/enhance_scan.py --dol <disc>/sys/main.dol --insns listing.txt --symbols <work>/symbols.txt"
$S fn run__6SystemFv waitForTick__FUlUs draw__7RaceAppFv            # the loop, the wait, the draw byte
$S field 56 803CBD70 ; $S field 28 803CBD70 ; $S field 32 803CBD70   # who writes the display's interval, rate, tick rate
$S refs 803CBE08-803CBE14                                           # the aspect floats' readers and writers
$S callers getPerspAspect__6SystemFv C_MTXPerspective KartMtxPerspective__FPA4_fffffb calcViewFrustum__11J3DUClipperFv
$S callers GXLoadPosMtxImm J3DFifoLoadPosMtxImm__FPA4_fUl
$S floats "1/60" "1/30" "1000/60" "59.94~0.01" "608/448" "4/3" 608 448 640 480
$S words 80336EF8 24
```

## Not determined statically

- Whether the console really held 60 in three- and four-player races (the code only says it doesn't choose 30).
- What `[LANEntry+4]`, `[+8]` and `[+0x15]` count.
- Which J3D matrix path each model uses (concatenated view, indexed, or `ExModel::simpleDraw`), and so how many draws each tag kind covers; whether any model uses CPU skinning (`J3DSkinDeform::deform`, `800791B8`), which would need vertex blending.
- Whether culling results feed the logic (an object's `calc` skipped when off screen), which decides if widening is invisible to ghosts and LAN: the per-tick position log across *w* answers it.
- How `Race2D` scissors per-player HUDs, hence the squeeze's frame of reference in quarters.
- The camera-cut signals' exact fields beyond the mode bytes.
