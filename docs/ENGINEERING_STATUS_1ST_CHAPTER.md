# Trails in the Sky 1st Chapter engineering status

This is the authoritative status of the frozen 2026-09-11 source baseline. Statements are labelled **verified**, **inference**, or **untested** where the distinction matters.

## DX11 Frame Generation / DLSS MFG

**Problem.** The game is DX11 and has native DLSS Super Resolution inputs but no native FG integration; Streamline DLSSG presentation is D3D12-oriented.

**Current solution.** OptiScaler retains the engine temporal inputs through Present, imports them into its D3D12 path, drives DLSSG/MFG and presents through a D3D12 swapchain. A dedicated interop queue and deferred input sync avoid premature serialization. RenoDX supplies game-specific HUD-less and UI resources through shared NT handles. This is engine-aware FG, not screen-only optical flow.

**Verified.** 2X, 3X and 4X actual present ratios; non-null depth/MV; DLSSG dispatch; 4K HDR10 presentation; gameplay, menus and combat spot checks. Paired FE+-active measurements were 56.24 source FPS off, 50.15 at 2X, 46.89 at 3X and 43.48 at 4X. Multiplier-dependent cost scales with inference/presenter back-pressure. The title-specific optimized resource producer adds about 0.19 ms/source frame versus dormant.

**Limitations.** 4X is not a visual PASS for fast markers; long soak, resolution/fullscreen/HDR recreation and RTX 40 validation remain untested. The long hidden Present is observed presenter back-pressure; attempts to skip it reduced output throughput and were rejected.

## HUD separation and recomposition

**Original failure.** With final UI embedded in scene color or with a representation-mismatched HUD-less surface, FG split, ghosted or malformed HUD elements. A generic upscaler-output HUD-less texture was not compatible with the final RenoDX HDR10 boundary.

**Current solution.** RenoDX writes the pre-HUD FP16 scene into a per-slot MRT at the output-resolution `0xC9FA40B7` boundary. The final shaders write visible Final, matching HDR10 HUD-less and FP16 premultiplied UI color/alpha in one fused pass. OptiScaler imports and tags the exact current-frame resources. The older copy/two-draw route remains a compatibility fallback.

For the white marker draw `0x499D48C2`, experimental mode 2 suppresses its original contribution before C9 and replays the captured draw immediately after `FinishSoraHudlessSceneMRT`, via `ReplaySoraFGDeferredMarkerPackets`. Replay restores shaders, input layout, buffers, constants, SRVs, samplers, blend/raster/depth state, viewport/scissor and scene-depth sampling while keeping it out of HUD-less RGB.

**Verified invariants.** Final contains marker; HUD-less contains background without marker; RGBA16F UI contains marker. Final/HUD-less are both 3840x2160 R10 PQ/BT.2020; UI is RGBA16F. The measured composition equation has channel p99 error 0.0005174 and no pixel over two R10 codes. With `AllowedFrameAhead=0`, 18,850/18,850 token groups had matching Final/HUD-less/UI application epochs and zero marker replay mismatch.

**Mechanism versus identification.** The slot ABI, fused export, replay state packet and frame alignment are candidate engine-level mechanisms. The shader hashes, C9 identity and white-marker classification are 1st-specific.

## Moving markers and billboards

Status: **PARTIAL for white marker at 2X; FAIL as a generalized billboard solution.**

The original symptom was jitter, doubled/ghosted images, breakup and deformation on NPC/shop icons while fixed HUD remained correct. Passive forensics on 1,058 4X application frames found no discontinuity in atlas, UV, appearance bytes, alpha scale, tint, depth-fade constants, resource identity, instance correspondence or projected motion. The first 64 transform bytes changed smoothly; bytes 64–191 were invariant. Real endpoints were clean while intermediate/generated-like frames lost bright area and gained edge complexity.

Controlled frozen-snapshot sweep at 4X:

| Speed (px/application frame) | Representative adjusted ring20 result |
|---:|---|
| 2 | half-ish, 1 px nearest-endpoint distance: median 0% |
| 10 | quarter 0.520%; half 1.008% |
| 25 | quarter 1.740%; half 10.085% |
| 32 | quarter 2.981%; half 4.131% |
| 40 | quarter 3.961%; half 28.269% |

These individual samples do not establish a hard 25 px threshold. Across the sweep, contamination is present in generated phases and broadly grows with displacement from the nearest clean endpoint. Morphology is the same thin irregular edge/background contamination seen on the original white marker.

Strict 40 px same-process/same-F10 A/B with BiasCurrentColorHint off:

| Recomposition | Endpoint | Quarter | Half |
|---|---:|---:|---:|
| OFF | 0% | 9.011% | 22.690% |
| ON | 0% | 8.295% | 18.673% |

Both paths show the same phase-dependent class. Therefore UI recomposition changes magnitude but is not a necessary cause. Mode 11 proved BiasCurrentColorHint is not sufficient. Static UI, linear synthetic UI, frozen transform and low-speed frozen-appearance controls were clean; explicit UI Alpha versus RGBA and recomposition OFF/ON did not explain the original defect.

The minimum supported claim is a fast-overlay interpolation failure occurring in generated/intermediate phases, not an application-endpoint discontinuity. It may be in common DLSSG synthesis or the later shared HDR/presenter chain. No single MV, depth or alpha mechanism is proven. The next diagnostic is post-DLSSG/pre-HDR-presenter generated-surface capture; no correction follows from current evidence yet.

Black billboards are unhandled and the white-marker hash-based repair does not apply to them.

## RenoDX coexistence and HDR

**Original conflict.** The earlier scRGB presentation arrangement was incompatible with the working DLSSG presenter path and produced overexposed/invalid output; overlay routing also prevented the ReShade Home UI from appearing on the visible presenter.

**Current solution.** RenoDX retains internal `R16G16B16A16_FLOAT` linear HDR. Only the final FG/presentation boundary uses PQ/BT.2020 `R10G10B10A2_UNORM`; Final and HUD-less match. UI remains premultiplied `R16G16B16A16_FLOAT`, preserving alpha precision. The visible D3D12 presenter receives the ReShade overlay. Load uses OptiScaler as the `dxgi.dll` proxy, with ReShade/add-ons loaded through the configured plugin path; do not add a second competing proxy.

**Verified.** No overexposure in the current path; HDR color/highlights and UI transparency passed manual checks; Home overlay opens; exact tagged resource formats and color-space marker were logged. The implementation does not lower RenoDX’s internal HDR pipeline to 10-bit.

**Remaining edge cases.** HDR toggle, output-mode recreation and non-HDR fallback lack a dedicated matrix. Ordinary SDR screenshots are not colorimetric HDR evidence.

## Latency, responsiveness and pacing

- `AllowedFrameAhead=0` is required for the current marker/frame-alignment baseline. It removed the observed one-application-frame resource mismatch and was subjectively preferred. It may reduce queue depth; no hardware latency number is claimed.
- `DeferFGInputSyncToPresent=true` and `DedicatedInteropQueue=true` preserve overlap and avoid an earlier overly serialized DX11/DX12 path.
- `Dx11ResourcesValidUntilPresent=true` keeps engine inputs valid for direct backend use instead of forcing unnecessary clones.
- `DisableHUDFix=true` disables generic tracking because the title-specific RenoDX provider owns HUD resources.
- V-Sync is forced off (`SyncInterval=0`) because it materially improved present uniformity in this setup.
- No frame limiter is used (`FramerateLimit=0`, `AutoFramerateLimit=false`).
- `NonBlockingHiddenPresent=false` and `SkipHiddenPresent=false` are retained: changing them moved waits and reduced output throughput.
- Game Reflex markers are disabled. No claim is made that Reflex provides free latency reduction in this custom path.

Do not compare subjective feel or FPS unless display mode, DLSS/DLAA, multiplier, FE+ state, V-Sync/cap and `AllowedFrameAhead` are fixed.

## Performance and Falcom Engine+

A clean controlled attribution compared identical 4K DLAA, ReShade + RenoDX, FG-off runs with Falcom Engine+ inactive versus active. FE+ inactive averaged 80.75 FPS / 12.384 ms; FE+ active averaged 57.59 FPS / 17.363 ms. The +4.979 ms source-frame and +4.692 ms GPU-busy deltas reproduced about 97.9% of the historical ~80 to ~57 domain. CPU increased only 0.323 ms.

Canonical conclusion: active Falcom Engine+ GPU work explains the old ~57 FPS state; persistent FG/OptiScaler/RenoDX performance pollution was not observed after removal. No production performance fix or diagnostic instrumentation from that task belongs in this baseline. Historical 78/80+ to 59 observations remain only an FE+ effective-activation transition hypothesis.

Every performance result must state **Falcom Engine+ OFF** or **Falcom Engine+ ON**. The multiplier table above belongs to the FE+-active domain.

## Current upstream status

This baseline deliberately remains based on OptiScaler `4f17a05d`. At freeze time official `origin/master` was `d36a078f`, with newer DX11 resource-management, mutex and D3D11On12 queue work. None was blindly merged. RenoDX remains based on `1e7366dd`; official `origin/main` had advanced to `3a155bad`.

The future 2nd task should first establish whether this frozen implementation works, preserve that result, and only then compare newer upstream commits individually.

## Known-good runtime profile

```ini
[Upscalers]
Dx11Upscaler=dlss

[FrameGen]
Enabled=true
FGInput=upscaler
FGOutput=dlssg
AllowedFrameAhead=0

[DLSSG]
InterpolationCount=1
OverrideInterpolationCount=auto
UseGamesReflexMarkers=false
SoraUIExperimentMode=2
SoraUIFrameDiagnostics=false

[OptiFG]
UseDx11UpscalerOutputAsHudless=false
Dx11ResourcesValidUntilPresent=true
DisableHUDFix=true

[Dx11withDx12]
DeferFGInputSyncToPresent=true
DedicatedInteropQueue=true
NonBlockingHiddenPresent=false
SkipHiddenPresent=false

[Framerate]
FramerateLimit=0
AutoFramerateLimit=false

[HDR]
ForceHDR=true
UseHDR10=true
SkipColorSpace=false

[V-Sync]
OverrideVsync=true
ForceVsync=false
SyncInterval=0

[Plugins]
LoadReshade=true
```

`InterpolationCount=1` means one generated frame (2X). Mode 2 is the explicit RGBA white-marker diagnostic/playable path. The source default `-1` does not enable this title-specific deferred marker repair. Modes 7–12 and high-frequency diagnostics are research controls only.
