# DirectX 11 engine-aware DLSS Frame Generation proof of concept

## Trails in the Sky 1st Chapter

> [!WARNING]
> This is a source-only engineering proof of concept. It is not an official OptiScaler or RenoDX release, and it is not yet suitable for a one-click end-user package. Do not use DLL injection in anti-cheat protected software.

[Traditional Chinese version](TRAILS1ST_DX11_DLSSG_POC.zh-TW.md)

## Result

This branch demonstrates that a DirectX 11 game with an existing temporal-upscaling integration can feed engine data into a DirectX 12 frame-generation backend without replacing the game renderer or falling back to screen-only optical flow.

The tested game is the PC version of **Trails in the Sky 1st Chapter**, version `1.0.5.0`. It uses DirectX 11 and has native DLSS Super Resolution, but no native Frame Generation integration.

The final tested path is:

```text
Falcom DirectX 11 renderer
  -> native DLSS Super Resolution inputs
     (color, depth, motion vectors, jitter and frame state)
  -> OptiScaler DX11/DX12 input bridge
  -> RenoDX FP16 HDR scene processing
  -> RenoDX final tone/gamut mapping and PQ encoding
  -> matching HUD-less HDR10 + FP16 premultiplied UI resources
  -> shared NT handles opened by the D3D12 bridge
  -> Streamline DLSS Frame Generation
  -> D3D12 HDR10 presenter
```

This is engine-aware Frame Generation. It is not equivalent to Lossless Scaling, driver Smooth Motion, or another final-frame optical-flow injector.

## Current source references

- OptiScaler upstream base: `4f17a05da871a583b9313ee74d33acfcf074a03d`
- OptiScaler proof-of-concept code before this report: `620f06334a74a4b9a8f10abee2a2ba45c38f58cd`
- RenoDX upstream base: `1e7366dde741c551de218ea4b5c7190bcd6ac595`
- RenoDX game provider: `b1811b9270f0fc5a55be7a0ce973452b8dde7fbb`
- RenoDX provider branch: `https://github.com/circerio/renodx/tree/public/trails1st-dx11-dlssg-provider`

The OptiScaler branch is intentionally kept as a proof-of-concept series. It still contains experimental history and game-specific coupling. It should be refactored into smaller generic changes before any upstream pull request.

## Pipeline classification

### Internal HDR processing

RenoDX keeps the HDR-critical render resources in `DXGI_FORMAT_R16G16B16A16_FLOAT`. Runtime resource-upgrade logs show the relevant full-resolution resources upgraded from `R11G11B10_FLOAT` to `R16G16B16A16_FLOAT`.

The game-specific Frame Generation additions do not replace visible RGB tone mapping, grading, bloom, TAA, lighting, or Falcom Engine+ processing. The scene and final shaders expose additional MRT outputs, while the visible `SV_Target0` operations remain equivalent. RGB is unchanged.

### Final representation

The final presentation boundary uses:

| Resource | Format | Representation |
|---|---|---|
| Visible final color | `R10G10B10A2_UNORM` | PQ, BT.2020, HDR10 |
| HUD-less color | `R10G10B10A2_UNORM` | PQ, BT.2020, HDR10 |
| UI color and alpha | `R16G16B16A16_FLOAT` | premultiplied high-precision UI |
| D3D12 swapchain | `R10G10B10A2_UNORM` | `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` |

The HUD-less and final color representations therefore match at the DLSSG boundary. UI alpha is not stored in the two-bit alpha channel of `R10G10B10A2_UNORM`.

The previous scRGB path used an FP16 `extended_srgb_linear` swapchain. The current path converts only at the final boundary because Streamline DLSSG presentation is using HDR10. This is not an internal RenoDX precision downgrade, though it is not bit-identical to the previous scRGB transport.

## Resource chain

The RenoDX provider maintains four slots. Each slot contains:

1. An FP16 HUD-less linear scene render target/SRV.
2. A shared HDR10 PQ HUD-less texture.
3. A shared RGBA16F premultiplied UI texture.
4. A local final HDR10 PQ resource retained for the compatibility fallback.

The active path fuses both exports into renderer passes that already exist. The pre-HUD FP16 scene pass writes the HUD-less slot as a second MRT, bypassing the old 4K RGBA16F `CopyResource`. The final pass writes visible Final, HDR10 HUD-less and FP16 UI simultaneously. Pixels with exactly zero UI coverage reuse the already encoded Final as HUD-less; any non-zero alpha keeps the full high-precision HUD-less encode and UI calculation. The previous copy plus two-draw producer remains available when the MRT preconditions are not met.

The provider exports a versioned structure through `RenoDX_GetSoraFGResourcesV1`. OptiScaler opens the shared handles on its D3D12 device, validates size, format and flags, and tags the matching resources for the current frame. A fifth unique handle is treated as provider resource recreation and resets the opened set.

This ABI is currently game-specific. A reusable upstream design should replace the `Sora` and `RenoDX` names with a generic versioned Frame Generation resource-provider contract.

## Runtime evidence

The latest final-pipeline validation session reported:

```text
FrameGen.FGInput: upscaler
FrameGen.FGOutput: dlssg
DLSSG.InterpolationCount: 1
HDR.UseHDR10: true
Dx11wDx12SC HDR presentation active: format 24, color space 12
RenoDX Sora FG interop active: HUD-less R10 PQ/BT.2020 + premultiplied UI RGBA16F, 3840x2160
DLSSG UI recomposition: enabled
```

`format 24` is `DXGI_FORMAT_R10G10B10A2_UNORM`; color space `12` is `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`.

That session contained 5,456 log lines over approximately 103 seconds, initialized the HDR presenter five times during startup/recreation, imported the provider successfully, and recorded no OptiScaler error line or device-removed event. The ReShade Home overlay was also rendered on the visible D3D12 presenter.

The runtime used Streamline host SDK `2.11.1`, with NVIDIA OTA production feature plugins reporting `2.12.129`.

## Validation performed

### Temporal bridge and HUD-less proof

An earlier FSR Frame Generation stage was used to isolate the temporal bridge before DLSSG/HDR integration. It verified:

- Non-null game depth and motion-vector resources reached the D3D12 FG dispatch.
- FSR FG configure and dispatch returned success.
- The generated-frame callback reported one generated frame per source frame.
- A 60 FPS source test produced approximately 120 FPS output rather than reducing the source to 30 FPS.
- Enabling the pre-UI DLSS output as HUD-less eliminated repeatable HUD splitting visible without HUD-less input.
- Movement, dialogue, menus, scene transitions and combat were exercised.

These results prove the DX11 temporal-input bridge and HUD-less behavior, but they are not presented as validation of the final DLSSG/HDR implementation.

### Final DLSSG, HDR and UI path

The final build has been runtime-verified at 3840 x 2160, 240 Hz HDR on:

- Windows 11 build `10.0.26200`
- NVIDIA GeForce RTX 5070 Ti
- NVIDIA driver `616.56`
- MSI MPG322UX OLED using RGB 10 bits per color channel
- DLSSG 2x in the latest logged session

User-visible checks passed for color, HDR highlights, HUD appearance and transitions, input feel, movement, menus and combat during iterative testing. ReShade Home was also revalidated after the final lifetime patch. DLSSG 2x, 3x and 4x actual present ratios were measured; presentation uniformity improved substantially after disabling V-Sync, but 4x remains experimental.

The final HDR/UI build has not yet passed a long-duration soak test. The host system also had unrelated instability, so no crash attribution or broad stability claim is made.

## Reproduction configuration

The final local configuration uses the following relevant settings:

```ini
[Upscalers]
Dx11Upscaler = dlss

[FrameGen]
Enabled = true
FGInput = upscaler
FGOutput = dlssg

[DLSSG]
InterpolationCount = 1
UseGamesReflexMarkers = false

[OptiFG]
UseDx11UpscalerOutputAsHudless = false
Dx11ResourcesValidUntilPresent = true
DisableHUDFix = true

[Dx11withDx12]
DeferFGInputSyncToPresent = true
DedicatedInteropQueue = true
NonBlockingHiddenPresent = false
SkipHiddenPresent = false

[Framerate]
FramerateLimit = 0
AutoFramerateLimit = false

[HDR]
ForceHDR = true
UseHDR10 = true
SkipColorSpace = false

[V-Sync]
OverrideVsync = true
ForceVsync = false
SyncInterval = 0

[Plugins]
LoadReshade = true
```

The old generic DX11 upscaler-output HUD-less option is deliberately disabled in the final HDR path. The RenoDX provider supplies a post-tone-map HUD-less resource matching the final HDR10 representation instead.

## Building and installation

1. Build this OptiScaler branch as `Release | x64` using the upstream build instructions.
2. Build the `falcomengine` target from the matching RenoDX provider branch; it produces `renodx-falcomengine.addon64`.
3. Obtain official production/signed Streamline and DLSSG runtime files from NVIDIA. Do not use or redistribute development/unsigned plugins as a public package.
4. Install OptiScaler, the RenoDX Falcom Engine add-on, Falcom Engine+ and ReShade according to their upstream instructions.
5. Apply the configuration above, enable native DLSS Super Resolution in the game, and start with DLSSG 2x.

No NVIDIA binary, game file, user log or captured frame is distributed by this branch.

### Source build verification

Both public source branches were rebuilt as `Release | x64` on 2026-09-05 after the provider cost and resource-lifetime work:

- OptiScaler produced `OptiScaler.dll` (SHA-256 `CEA9797A13E3A1D8C785572B1E15E8AD0DE6698564B632743D1967DA52DA38A4`).
- RenoDX target `falcomengine` produced `renodx-falcomengine.addon64` (SHA-256 `6E2EE9CCABE0F693CEFFC4B312BE886C0D215288719B262CADC96C177DDDCD0D`).
- `dumpbin /exports` confirmed `RenoDX_GetSoraFGResourcesV1` in the rebuilt RenoDX add-on.

These hashes document the local build check only; the binaries are not committed or distributed.

### Frame-generation cost

Clean PresentMon 2.3.1 captures used unique sessions and `--terminate_after_timed`. With the optimized high-quality provider, paired measurements on the matched test scene were:

| Mode | Source time | Source FPS | Output FPS | Cost versus FG OFF |
|---|---:|---:|---:|---:|
| FG OFF | 17.7802 ms | 56.24 | 56.23 | — |
| 2x | 19.9407 ms | 50.15 | 100.32 | +2.1606 ms |
| 3x | 21.3271 ms | 46.89 | 140.74 | +3.5469 ms |
| 4x | 23.0007 ms | 43.48 | 173.98 | +5.2205 ms |

The original high-quality RenoDX producer cost approximately 0.58–0.64 ms/source frame. Fusing the FP16 scene capture and HDR10/UI export into existing renderer passes reduced the measured 2x source time by 0.3887 ms (1.95%) and left approximately 0.19 ms of title-specific provider cost. Opti import CPU time was approximately 0.009 ms. The remaining multiplier-dependent cost is primarily DLSSG/MFG inference and presenter back-pressure rather than a generic tracking or extra-copy path.

## Known limitations and required follow-up

- Only one Windows/NVIDIA/HDR system has validated the final pipeline.
- A one-hour DLSSG 2x soak and a 30-minute 4x stress run are still required.
- Resolution changes, fullscreen changes, HDR on/off and repeated device/resource recreation need dedicated regression tests.
- RTX 40-series DLSSG 2x needs independent validation.
- SDR presentation and non-RenoDX fallback behavior need independent validation.
- The provider ABI and OptiScaler import path are hard-coded for this game.
- The branch should be tested against upscaler/DLSSG lifecycle resets and resource-transition handling before upstreaming.
- Performance cost and present pacing were measured with PresentMon and targeted internal timing. End-to-end click-to-photon latency still requires independent hardware instrumentation.
- HDR screenshots captured through ordinary SDR tools are not reliable evidence of HDR luminance or color accuracy.

## Upstreaming recommendation

The work should be proposed upstream in small, reviewable units:

1. Generic DX11-to-D3D12 FG presentation and resource lifetime support.
2. A generic, versioned HUD-less/UI provider ABI.
3. HDR10 color-space and representation validation.
4. Visible-presenter overlay support.
5. Game-specific RenoDX provider kept in the RenoDX repository.

This report and branch are intended to provide reproducible evidence and design input before a pull request, not to bypass maintainer review.

## Licensing and attribution

- OptiScaler modifications remain under OptiScaler's GPL-3.0 license.
- RenoDX provider modifications remain under RenoDX's MIT license.
- NVIDIA Streamline and DLSSG components remain subject to NVIDIA's licenses and are not included here.
- OptiScaler and RenoDX upstream authors retain credit for their respective projects.
- The proof of concept was developed and validated collaboratively with OpenAI Codex; runtime observations were confirmed by the local tester and logs.

This project is not affiliated with Nihon Falcom, NVIDIA, OptiScaler or RenoDX.
