# DX11 engine-aware DLSS Frame Generation 技術實證

## 《空之軌跡 the 1st》

> [!WARNING]
> 這是僅含 source 的工程 Proof of Concept，不是 OptiScaler 或 RenoDX 官方版本，也還不適合作為一鍵安裝正式版。請勿在受 anti-cheat 保護的程式中使用 DLL injection。

[English version](TRAILS1ST_DX11_DLSSG_POC.md)

## 結果

此分支證明：具備 temporal upscaling integration 的 DirectX 11 遊戲，可以將遊戲引擎資料送入 DirectX 12 Frame Generation backend，而不必替換整個 renderer，也不必退回只分析最終畫面的 optical flow。

測試對象為 PC 版 **《空之軌跡 the 1st》** `1.0.5.0`。遊戲使用 DirectX 11，原生具備 DLSS Super Resolution，但沒有原生 Frame Generation integration。

最終測試路徑為：

```text
Falcom DirectX 11 renderer
  -> 原生 DLSS Super Resolution inputs
     （color、depth、motion vectors、jitter、frame state）
  -> OptiScaler DX11/DX12 input bridge
  -> RenoDX FP16 HDR scene processing
  -> RenoDX 最終 tone/gamut mapping 與 PQ encoding
  -> 相符的 HUD-less HDR10 與 FP16 premultiplied UI resources
  -> 以 shared NT handles 在 D3D12 bridge 開啟
  -> Streamline DLSS Frame Generation
  -> D3D12 HDR10 presenter
```

這是 engine-aware Frame Generation，不等同於 Lossless Scaling、driver Smooth Motion 或其他 final-frame optical-flow injector。

## Source 版本

- OptiScaler upstream base：`4f17a05da871a583b9313ee74d33acfcf074a03d`
- 此報告加入前的 OptiScaler PoC code：`620f06334a74a4b9a8f10abee2a2ba45c38f58cd`
- RenoDX upstream base：`1e7366dde741c551de218ea4b5c7190bcd6ac595`
- RenoDX game provider：`b1811b9270f0fc5a55be7a0ce973452b8dde7fbb`
- RenoDX provider branch：`https://github.com/circerio/renodx/tree/public/trails1st-dx11-dlssg-provider`

OptiScaler branch 刻意保留為 PoC commit series，目前仍有實驗歷史與遊戲專用 coupling；送 upstream PR 前應拆成較小、通用的變更。

## Pipeline classification

### 內部 HDR processing

RenoDX 的 HDR-critical resources 維持 `DXGI_FORMAT_R16G16B16A16_FLOAT`。Runtime resource-upgrade log 顯示，相關全解析度資源由 `R11G11B10_FLOAT` 升級為 `R16G16B16A16_FLOAT`。

遊戲專用 FG 修改沒有取代可見 RGB 的 tone mapping、grading、bloom、TAA、lighting 或 Falcom Engine+ processing。可見 pipeline shader 唯一變更是為 UI coverage capture 初始化 alpha；RGB 未改。

### 最終 representation

最終 presentation boundary 使用：

| Resource | Format | Representation |
|---|---|---|
| Visible final color | `R10G10B10A2_UNORM` | PQ、BT.2020、HDR10 |
| HUD-less color | `R10G10B10A2_UNORM` | PQ、BT.2020、HDR10 |
| UI color and alpha | `R16G16B16A16_FLOAT` | premultiplied high-precision UI |
| D3D12 swapchain | `R10G10B10A2_UNORM` | `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` |

因此 HUD-less 與 final color 在 DLSSG boundary 的 representation 一致。UI alpha 沒有使用 `R10G10B10A2_UNORM` 的 2-bit alpha。

之前的 scRGB 路徑使用 FP16 `extended_srgb_linear` swapchain。Streamline DLSSG presentation 目前使用 HDR10，因此只在最後 boundary 轉換。這不是 RenoDX 內部降精度，但也不是與舊 scRGB transport 位元級完全相同。

## Resource chain

RenoDX provider 維持四個 slots，每個 slot 包含：

1. FP16 HUD-less linear scene capture。
2. Final HDR10 PQ copy。
3. Shared HDR10 PQ HUD-less texture。
4. Shared RGBA16F premultiplied UI texture。

Provider 透過 `RenoDX_GetSoraFGResourcesV1` 匯出有版本的 structure。OptiScaler 在 D3D12 device 開啟 shared handles，驗證尺寸、format 與 flags，並標記同一 frame 的 resources。第五組新 handle 會被視為 provider resource recreation，並重置已開啟的 resource set。

此 ABI 目前仍是遊戲專用。通用 upstream design 應以有版本的 Frame Generation resource-provider contract 取代 `Sora` 與 `RenoDX` 名稱。

## Runtime 證據

最新 final-pipeline validation session 顯示：

```text
FrameGen.FGInput: upscaler
FrameGen.FGOutput: dlssg
DLSSG.InterpolationCount: 1
HDR.UseHDR10: true
Dx11wDx12SC HDR presentation active: format 24, color space 12
RenoDX Sora FG interop active: HUD-less R10 PQ/BT.2020 + premultiplied UI RGBA16F, 3840x2160
DLSSG UI recomposition: enabled
```

`format 24` 是 `DXGI_FORMAT_R10G10B10A2_UNORM`，color space `12` 是 `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`。

該 session 約 103 秒、共 5,456 行 log；啟動／重建期間 HDR presenter 初始化五次，provider 成功匯入，沒有 OptiScaler error line 或 device-removed event。ReShade Home overlay 也成功顯示在實際可見的 D3D12 presenter。

Runtime 使用 Streamline host SDK `2.11.1`；NVIDIA OTA production feature plugins 回報版本 `2.12.129`。

## 已完成驗證

### Temporal bridge 與 HUD-less proof

早期先以 FSR Frame Generation 階段隔離 temporal bridge，再處理 DLSSG/HDR integration。該階段確認：

- 遊戲的 depth 與 motion-vector resources 非空，並成功到達 D3D12 FG dispatch。
- FSR FG configure／dispatch 成功。
- Generated-frame callback 每個 source frame 回報一個 generated frame。
- 60 FPS source 測試輸出約 120 FPS，而不是把 source 降成 30 FPS。
- 未使用 HUD-less 時可重現 HUD splitting；啟用 pre-UI DLSS output 後消失。
- 測試過移動、對話、選單、場景切換與戰鬥。

這些結果證明 DX11 temporal-input bridge 與 HUD-less 行為，但不拿來冒充 final DLSSG/HDR implementation 的驗證。

### Final DLSSG、HDR 與 UI path

Final build 已在以下環境完成 3840 x 2160、240 Hz HDR runtime validation：

- Windows 11 build `10.0.26200`
- NVIDIA GeForce RTX 5070 Ti
- NVIDIA driver `616.56`
- MSI MPG322UX OLED，RGB 10 bits per color channel
- 最新 log session 使用 DLSSG 2x

迭代測試期間，使用者目視確認 color、HUD、操作手感、移動、選單與戰鬥正常。較早版本也測過 DLSSG 4x；關閉 V-Sync 後 presentation uniformity 大幅改善，但 4x 目前仍屬 experimental。

Final HDR/UI build 尚未完成長時間 soak test。Host system 另有與本模組無法歸因的穩定性問題，因此不宣稱已證明 crash stability。

## 重現設定

最終本機設定的相關部分為：

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

Final HDR path 刻意關閉舊的 generic DX11 upscaler-output HUD-less 選項。HUD-less 改由 RenoDX provider 在 post-tone-map 階段提供，確保與 final HDR10 representation 相符。

## Build 與安裝

1. 依 upstream 指示以 `Release | x64` build 此 OptiScaler branch。
2. 從對應 RenoDX provider branch build `renodx-falcomengine` target。
3. 從 NVIDIA 取得官方 production/signed Streamline 與 DLSSG runtime。公開 package 不應使用或重散布 development/unsigned plugins。
4. 依各 upstream 指示安裝 OptiScaler、RenoDX Falcom Engine add-on、Falcom Engine+ 與 ReShade。
5. 套用上述設定，在遊戲內開啟原生 DLSS Super Resolution，並先從 DLSSG 2x 開始。

此 branch 不包含 NVIDIA binary、遊戲檔案、使用者 log 或 captured frame。

## 已知限制與後續驗證

- Final pipeline 目前只在一台 Windows／NVIDIA／HDR 系統驗證。
- 尚需一小時 DLSSG 2x soak test 與 30 分鐘 4x stress run。
- 需要專門測試 resolution、fullscreen、HDR on/off 與反覆 device/resource recreation。
- 需要獨立驗證 RTX 40-series DLSSG 2x。
- SDR presentation 與沒有 RenoDX provider 時的 fallback 尚需獨立驗證。
- Provider ABI 與 OptiScaler import path 目前硬編碼給此遊戲。
- 送 upstream 前需針對 upscaler/DLSSG lifecycle reset 與 resource transition 做測試。
- 尚未以專門儀器量測 performance cost 與 click-to-photon latency。
- 一般 SDR capture 工具取得的 HDR screenshot 不能作為 HDR luminance 或 color accuracy 證據。

## Upstreaming 建議

建議拆成較小且可審查的單元：

1. 通用 DX11-to-D3D12 FG presentation 與 resource lifetime support。
2. 通用且有版本的 HUD-less／UI provider ABI。
3. HDR10 color-space 與 representation validation。
4. Visible-presenter overlay support。
5. 遊戲專用 RenoDX provider 保留在 RenoDX repository。

此報告與 branch 的目的，是在 PR 前提供可重現證據與設計討論素材，不是繞過 maintainer review。

## 授權與署名

- OptiScaler 修改延續 OptiScaler GPL-3.0 license。
- RenoDX provider 修改延續 RenoDX MIT license。
- NVIDIA Streamline／DLSSG components 仍受 NVIDIA license 規範，沒有包含於此。
- OptiScaler 與 RenoDX upstream authors 保留各自專案的 credit。
- 此 PoC 由本機測試者與 OpenAI Codex 協作開發及驗證；runtime observations 經本機目視與 log 確認。

本專案與 Nihon Falcom、NVIDIA、OptiScaler 或 RenoDX 均無官方隸屬關係。
