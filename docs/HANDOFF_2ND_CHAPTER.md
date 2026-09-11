# Trails in the Sky 2nd Chapter engineering handoff

This document is the entry point for a **separate** 2nd Chapter validation task. It freezes what is known about the 1st Chapter implementation; it does not claim 2nd Chapter support.

## Known-good 1st baseline

| Item | Frozen value |
|---|---|
| OptiScaler branch | `public/trails1st-engineering-handoff-20260911` |
| OptiScaler immutable ref | `trails1st-engineering-baseline-20260911` |
| OptiScaler implementation source parent | `c7f548b255427af9bd2b155f3026f651422c4527` |
| OptiScaler upstream base | `4f17a05da871a583b9313ee74d33acfcf074a03d` |
| RenoDX companion branch/ref | `public/trails1st-engineering-handoff-20260911` / `trails1st-engineering-baseline-20260911` |
| RenoDX implementation source parent | `e04df8fafe7e2aff0c025dfd792712b0ffa29799` |
| RenoDX upstream base | `1e7366dde741c551de218ea4b5c7190bcd6ac595` |
| 1st Chapter build | `sora_1st.exe` 1.0.5.0, SHA-256 `9A8F3BE355D81C2D1E68494C51578F9F6A3C3EF8DBA9F445FCEE60CFE71A74AF` |
| Validated system | Windows 11 10.0.26200, RTX 5070 Ti, 4K/240 Hz HDR; final pipeline driver 616.56, latest diagnostic A/B driver 616.92 |
| Build | `msbuild /m /p:Configuration=Release OptiScaler.sln`; RenoDX configured tree: `cmake --build build.vs --config Release --target falcomengine` |

The immutable ref includes documentation only beyond the implementation-source parents above. NVIDIA runtime DLLs, game files, private logs and captures are not distributed.

Both implementation parents were rebuilt successfully during the freeze. OptiScaler produced a 25,646,080-byte DLL with SHA-256 `9A496BB8F9E7E6450673444475C2FDB92570FC97A5BE71A903A843EF72BD7A7E`; RenoDX produced a 1,792,000-byte add-on with SHA-256 `16978C6245DCD333B10DC4670E72E9CFFAAF3AC7DA78547033549167223E935B`. The latter exactly matches the deployed provider. The rebuilt OptiScaler has a different binary hash from the deployed DLL despite the same source parent; treat source refs and build inputs—not byte-for-byte reproducibility of linker metadata—as canonical.

For a known-good playable starting point use 2X, explicit RGBA UI mode 2, no frame cap, V-Sync disabled and `AllowedFrameAhead=0`. Modes 7–12 are diagnostics, not normal configuration. See [engineering status](ENGINEERING_STATUS_1ST_CHAPTER.md#known-good-runtime-profile).

## Current subsystem matrix

`PASS` means the stated configuration was exercised successfully, not merely initialized.

| Subsystem | Status | Evidence boundary |
|---|---|---|
| FG initialization | PASS | DX11 temporal resources reach D3D12 FG and dispatch succeeds |
| DLSS FG | PASS | Engine-aware DLSSG path, HDR10 presenter and real/generated present ratio verified |
| DLSS MFG 2X | PASS | 2.001x measured; current recommended mode |
| MFG 3X | PASS | 3X ratio and gameplay spot checks passed |
| MFG 4X | PARTIAL | 4X ratio works; fast moving marker intermediate frames remain imperfect |
| Frame pacing | PARTIAL | V-Sync off materially improved uniformity; no final hardware pacing characterization |
| Responsiveness/latency | PARTIAL | `AllowedFrameAhead=0` fixes observed cross-frame UI alignment and was preferred subjectively; no click-to-photon measurement |
| HUD separation | PASS | Final/HUD-less/UI resource invariants and frame epochs verified |
| HUD recomposition | PASS | Exact tagged surfaces satisfy the composition equation within R10 quantization tolerance |
| Static HUD | PASS | Menus, fixed HUD and transitions passed visual regression |
| Moving white marker | PARTIAL | Mode 2 strongly improves 2X; fast 4X intermediate-frame edge contamination remains |
| Billboard family | FAIL | Not generalized; black billboard variants are not handled by the white-marker identification |
| RenoDX coexistence | PASS | Combined injection, Home overlay and provider exports validated |
| HDR | PASS | RenoDX remains FP16 internally; matching Final/HUD-less HDR10 PQ/BT.2020 at FG boundary; UI alpha FP16 |
| Falcom Engine+ | PASS | Combined operation verified; its GPU cost must be treated as a distinct performance domain |
| RenoDX + FE+ + MFG | PARTIAL | Functional and visually tested; long soak and broad lifecycle matrix incomplete |
| Stability | PARTIAL | No persistent project-attributable device error established; long soak not completed and test machine had unrelated instability |
| Alt-tab | PARTIAL | Used repeatedly during testing, but no dedicated lifecycle matrix |
| Resolution/display changes | NOT TESTED | Dedicated resolution/fullscreen/HDR-toggle recreation regression still required |

## Architecture overview

The engine’s native DX11 DLSS-SR integration supplies temporal color, depth, motion vectors, jitter and frame continuity. OptiScaler preserves/imports those inputs, creates the D3D12 FG/presenter path, imports title-specific HUD resources from RenoDX, tags Streamline and schedules presentation.

Key OptiScaler files:

- `OptiScaler/inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp`: DX11 temporal-input lifetime and deferred synchronization.
- `OptiScaler/with_dx12/dx11_with_dx12.cpp` and `.h`: D3D11-on-D3D12 bridge/device path.
- `OptiScaler/with_dx12/dx11_with_dx12_sc.cpp` and `.h`: D3D12 swapchain/presenter, RenoDX provider import, frame-boundary and diagnostics bridge.
- `OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp`: DLSSG options, UI tagging and diagnostic modes.
- `OptiScaler/hooks/FG_Hooks.cpp`, `Reflex_Hooks.cpp`: FG lifecycle and low-latency hooks.
- `OptiScaler/Config.cpp`, `Config.h`: reproducibility switches.

Key RenoDX companion files:

- `src/games/falcomengine/addon.cpp`: resource slots, C9 boundary, marker suppression/replay, publication ABI and passive diagnostics.
- `src/games/falcomengine/sora/final_0xE20E1A41.ps_5_0.hlsl` and `finalsdr_0x14DAB5E7.ps_5_0.hlsl`: fused visible Final + HDR10 HUD-less + FP16 UI export.
- `src/games/falcomengine/sora/sora_fg_ui.ps_5_0.hlsl`: fallback UI export.
- `src/games/falcomengine/sora/sora_fg_marker_snapshot_*.hlsl`: controlled marker replay/sweep fixtures.

The active HDR chain is FP16 linear scene/RenoDX work, final tone/gamut mapping and PQ encode, matching `R10G10B10A2_UNORM` Final/HUD-less in `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`, plus `R16G16B16A16_FLOAT` premultiplied UI color/alpha. Shared NT handles cross the DX11/DX12 boundary. The provider has four slots and is consumer-gated so it is dormant with FG off.

## 1st-specific identifiers — do not copy blindly

| Identifier | 1st Chapter meaning |
|---|---|
| `0xE20E1A41` | HDR final/fused export shader |
| `0x14DAB5E7` | SDR-source final/fused export shader |
| `0xC9FA40B7` | output-resolution pre-HUD scene/HUD-less boundary |
| `0x499D48C2` | white NPC/shop marker draw used by the deferred-replay prototype |
| `RenoDX_GetSoraFGResourcesV1` | game-specific provider export; current structure is versioned despite the historical function suffix |
| `RenoDX_NotifySoraFGFrameBoundaryV1` | game-specific production/lifetime gate |

Shader hashes, resource identities, executable layout, instance-buffer shape, pass order and scene fixtures are observations for 1st Chapter only. The 192-byte marker payload and its first 64-byte transform region must be rediscovered in 2nd.

## Candidate engine-level mechanisms

These are transfer candidates, not universal facts:

- DX11 temporal-resource lifetime through Present and D3D12 FG/presenter architecture.
- A versioned producer/consumer HUD-less + premultiplied UI ABI.
- Fused MRT export preserving the visible RenoDX output while avoiding full-resolution copies.
- Same-application-frame token/resource alignment with `AllowedFrameAhead=0`.
- Late overlay suppression/replay when a draw is proven to belong outside HUD-less scene color.
- HDR10 boundary matching while keeping RenoDX/UI intermediates FP16.
- The empirical fast-overlay failure signature: clean endpoints but malformed generated/intermediate phases.

## Mandatory 2nd validation order

1. Check out both frozen refs and test this implementation against 2nd before adopting newer upstream.
2. Record initial launch, resource discovery and feature matrix without changing identifiers.
3. Preserve that initial result as its own commit/evidence note.
4. Only then inspect current OptiScaler upstream.
5. Adopt upstream changes individually only when controlled evidence shows a benefit.
6. Reproduce a static and fast-moving marker/billboard test in 2nd.
7. Apply the 1st deferred-replay mechanism only if capture proves the same render boundary and causal signature.
8. Regression-test 1st after every meaningful shared change, with Falcom Engine+ state explicitly fixed and recorded.

Recommended first action in the new task:

```powershell
git clone --recurse-submodules https://github.com/circerio/OptiScaler.git
git -C OptiScaler checkout trails1st-engineering-baseline-20260911
git clone --recurse-submodules https://github.com/circerio/renodx.git
git -C renodx checkout trails1st-engineering-baseline-20260911
```

Then inventory the 2nd executable, renderer API, native temporal integration and shader/resource identities before deployment.

## Known traps

- High FPS captured while Falcom Engine+ was not actually active is not comparable with the FE+ active domain.
- The historical ~57 FPS domain was reproduced by FE+ active GPU cost; it was not evidence of persistent OptiScaler/RenoDX pollution.
- A static-HUD PASS does not imply moving billboard correctness.
- Successful DLSSG initialization or a correct multiplier does not prove generated-frame morphology.
- A non-zero UI mask alone is insufficient; Final/HUD-less/UI equation, formats and frame epoch must agree.
- Desktop Present order is not a reliable real/generated classifier under asynchronous presentation.
- A 1st Chapter shader hash/resource pointer/instance index must not be assumed valid or stable in 2nd.
- The old “artifact begins at about 25 px/application-frame” wording is superseded: the controlled sweep supports continuous severity growth, not a hard onset threshold.
- RTSS/MSI Afterburner caused severe capture/runtime interference during testing; keep overlays/monitoring state explicit.

## What remains unresolved

The white-marker repair is not a universal billboard fix. At 4X, a frozen marker moving 2/10/25/32/40 px per application frame had clean application endpoints but increasingly visible thin, irregular edge/background contamination at generated phases. Strict same-snapshot recomposition OFF/ON testing produced the artifact in both cases, so UI recomposition is not necessary. Bias-current-color hinting was also insufficient. Black billboard variants remain unhandled.

The highest-information unexecuted diagnostic is a generated-surface capture/readback immediately after DLSSG and before the common HDR/presenter boundary. It should determine whether the residual is already in DLSSG output or introduced later. Do not start correction work until that boundary is isolated.

See [1st Chapter engineering status](ENGINEERING_STATUS_1ST_CHAPTER.md), [billboard evidence](evidence/BILLBOARD_MARKER_FORENSICS.md), [performance A/B/A evidence](evidence/PERFORMANCE_FALCOM_ENGINE_PLUS_ABA.md), and [machine-readable status](implementation_status.json).
