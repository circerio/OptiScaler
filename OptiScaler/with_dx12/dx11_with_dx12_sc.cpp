#include "pch.h"
#include "dx11_with_dx12_sc.h"

#include <with_dx12/with_dx12.h>

#include <hooks/FG_Hooks.h>
#include <menu/menu_overlay_dx.h>

#include <Util.h>
#include <Config.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <chrono>

#pragma intrinsic(_ReturnAddress)

static int scCount = 0;

namespace
{
template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

void SafeCloseHandle(HANDLE& value)
{
    if (value != nullptr)
    {
        CloseHandle(value);
        value = nullptr;
    }
}

void TransitionResource(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (commandList == nullptr || resource == nullptr || beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;

    commandList->ResourceBarrier(1, &barrier);
}

UINT ResolveBufferCount(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferCount > 0)
        return desc.BufferCount;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)) && desc1.BufferCount > 0)
        return desc1.BufferCount;

    return 2;
}

DXGI_FORMAT ResolveBufferFormat(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferDesc.Format != DXGI_FORMAT_UNKNOWN)
        return desc.BufferDesc.Format;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)))
        return desc1.Format;

    return DXGI_FORMAT_UNKNOWN;
}

constexpr uint32_t SORA_FG_INTEROP_VERSION = 3;
constexpr uint32_t SORA_FG_FLAG_HUDLESS = 1u << 0;
constexpr uint32_t SORA_FG_FLAG_UI = 1u << 1;
constexpr uint32_t SORA_FG_FLAG_HDR10_PQ_BT2020 = 1u << 2;
constexpr uint32_t SORA_FG_FLAG_UI_PREMULTIPLIED = 1u << 3;
constexpr uint32_t SORA_FG_FLAG_UI_FP16 = 1u << 4;
constexpr uint32_t SORA_FG_FLAG_UI_ALPHA_ONLY = 1u << 5;
constexpr uint32_t SORA_FG_FLAG_BIAS_CURRENT_COLOR = 1u << 6;

struct SoraFGInteropResourcesV1
{
    uint32_t structSize;
    uint32_t version;
    uint64_t frameId;
    uint32_t width;
    uint32_t height;
    uint32_t hudlessFormat;
    uint32_t uiFormat;
    uint32_t flags;
    uint32_t reserved;
    HANDLE hudlessHandle;
    HANDLE uiHandle;
    uint64_t applicationFrameId;
    uint64_t replayFrameId;
    uint64_t finalResourceIdentity;
    uint64_t hudlessResourceIdentity;
    uint64_t uiResourceIdentity;
    uint32_t replayDrawCount;
    int32_t experimentMode;
    uint32_t biasCurrentColorFormat;
    uint32_t biasCurrentColorReserved;
    HANDLE biasCurrentColorHandle;
    uint64_t biasCurrentColorIdentity;
};

using PFN_RenoDX_GetSoraFGResourcesV1 = BOOL (*)(SoraFGInteropResourcesV1* output);
using PFN_RenoDX_SetSoraFGUIExperimentModeV1 = BOOL (*)(int32_t mode);
using PFN_RenoDX_SetSoraFGMarkerVelocityV1 = BOOL (*)(float pixelsPerFrame);
using PFN_RenoDX_NotifySoraFGFrameBoundaryV1 = BOOL (*)();
using PFN_RenoDX_NotifyFalcomEnginePlusFrameBoundaryV1 = BOOL (*)();
} // namespace

bool Dx11wDx12SC::IsHdrInteropRequested()
{
    return Config::Instance()->ForceHDR.value_or_default();
}

DXGI_FORMAT Dx11wDx12SC::ResolveInteropFormat(DXGI_FORMAT requestedFormat)
{
    if (!IsHdrInteropRequested())
        return requestedFormat;

    // Streamline DLSS Frame Generation supports HDR10 presentation, not scRGB. Keep the
    // hidden D3D11 and visible D3D12 swapchains format-identical so the cross-API
    // CopyResource remains valid.
    const bool useHdr10 = Config::Instance()->UseHDR10.value_or_default() ||
                          State::Instance().activeFgOutput == FGOutput::DLSSG;
    return useHdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

DXGI_COLOR_SPACE_TYPE Dx11wDx12SC::ResolveInteropColorSpace()
{
    const bool useHdr10 = Config::Instance()->UseHDR10.value_or_default() ||
                          State::Instance().activeFgOutput == FGOutput::DLSSG;
    return useHdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                    : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

Dx11wDx12SC::Dx11wDx12SC(IDXGISwapChain* real, IDXGISwapChain4* fgSC, ID3D11Device* pDevice, HWND hWnd, UINT flags)
    : _real(real), _fgSwapChain(fgSC), _dx11Device(pDevice), _handle(hWnd), _refcount(1)
{
    _id = ++scCount;
    _lastFlags = flags;

    if (_real != nullptr)
    {
        _real->AddRef();
        _real->QueryInterface(IID_PPV_ARGS(&_real1));
        _real->QueryInterface(IID_PPV_ARGS(&_real2));
        _real->QueryInterface(IID_PPV_ARGS(&_real3));
        _real->QueryInterface(IID_PPV_ARGS(&_real4));
    }

    if (_fgSwapChain != nullptr)
        _fgSwapChain->AddRef();

    if (_dx11Device != nullptr)
    {
        _dx11Device->AddRef();
        auto device5Result = _dx11Device->QueryInterface(IID_PPV_ARGS(&_dx11Device5));
        if (FAILED(device5Result))
            LOG_WARN("ID3D11Device5 unavailable: {:X}", (UINT) device5Result);

        _dx11Device->GetImmediateContext(&_dx11Context);
        if (_dx11Context != nullptr)
        {
            auto context4Result = _dx11Context->QueryInterface(IID_PPV_ARGS(&_dx11Context4));
            if (FAILED(context4Result))
                LOG_WARN("ID3D11DeviceContext4 unavailable: {:X}", (UINT) context4Result);
        }
    }

    if (WithDx12::PrepareD3D12ForD3D11(_dx11Device, D3D_FEATURE_LEVEL_11_0))
    {
        _dx12Device = WithDx12::GetD3D12Device();
        _dx12CommandQueue = WithDx12::GetD3D12CommandQueue();
    }
    else
    {
        LOG_ERROR("failed to resolve D3D12 device/queue");
    }

    State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

    _RefreshCachedSwapchainDesc();

    if (!_ApplyInteropColorSpace() && IsHdrInteropRequested())
        LOG_WARN("Dx11wDx12SC failed to apply HDR color space during creation");

    LOG_INFO("Dx11wDx12SC {} created, real: {:X}, fg: {:X}, dx11: {:X}, dx12: {:X}, queue: {:X}", _id, (UINT64) _real,
             (UINT64) _fgSwapChain, (UINT64) _dx11Device, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
}

Dx11wDx12SC::~Dx11wDx12SC()
{
    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropObjects();

    SafeRelease(_real4);
    SafeRelease(_real3);
    SafeRelease(_real2);
    SafeRelease(_real1);
    SafeRelease(_fgSwapChain);
    SafeRelease(_real);
    SafeRelease(_dx11Context4);
    SafeRelease(_dx11Context);
    SafeRelease(_dx11Device5);
    SafeRelease(_dx11Device);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (ppvObject == nullptr)
        return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain1*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain2*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain3*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4 == nullptr && _fgSwapChain == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    if (riid == __uuidof(Dx11wDx12SC))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::AddRef()
{
    auto count = InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", count, Util::WhoIsTheCaller(_ReturnAddress()));
    return count;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", ret, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
        if (State::Instance().currentSwapchain == this)
            State::Instance().currentSwapchain = nullptr;

        if (State::Instance().currentWrappedSwapchain == this)
            State::Instance().currentWrappedSwapchain = nullptr;

        if (State::Instance().currentRealSwapchain == _real)
            State::Instance().currentRealSwapchain = nullptr;

        FGHooks::ClearDx12InteropPresentSC(_fgSwapChain);

        if (State::Instance().currentFGSwapchain == _fgSwapChain)
            State::Instance().currentFGSwapchain = nullptr;

        if (State::Instance().currentD3D11Device == _dx11Device)
            State::Instance().currentD3D11Device = nullptr;

        State::Instance().swapchainInteropApi = SwapchainInteropApi::None;

        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr)
        {
            fg->Deactivate();
            fg->ReleaseSwapchain(_handle);
        }

        delete this;
    }

    return ret;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real != nullptr ? _real->SetPrivateData(Name, DataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real != nullptr ? _real->SetPrivateDataInterface(Name, pUnknown) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real != nullptr ? _real->GetPrivateData(Name, pDataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetParent(REFIID riid, void** ppParent)
{
    return _real != nullptr ? _real->GetParent(riid, ppParent) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDevice(REFIID riid, void** ppDevice)
{
    return _real != nullptr ? _real->GetDevice(riid, ppDevice) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(UINT SyncInterval, UINT Flags)
{
    using PerfClock = std::chrono::steady_clock;
    const auto perfStart = PerfClock::now();

    if (_real == nullptr || _fgSwapChain == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    if (_fg == nullptr)
        _fg = State::Instance().currentFG;

    if ((Flags & DXGI_PRESENT_TEST) != 0)
        return _real->Present(SyncInterval, Flags);

    if (!_InitInteropObjects())
        return DXGI_ERROR_DEVICE_REMOVED;

    auto dx11Index = _GetDx11BackBufferIndexForPresent();

    if (!_RequestSharedBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    auto perfStageStart = PerfClock::now();
    if (!_CopyDx11BackBufferToShared(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;
    const auto perfAfterDx11Copy = PerfClock::now();

    if (!_WaitDx11ThenDx12())
        return DXGI_ERROR_DEVICE_REMOVED;
    const auto perfAfterDx11Sync = PerfClock::now();

    // RenoDX records its HUD-less/UI passes on this same immediate context,
    // so the fence above also makes those shared resources ready for D3D12.
    // When both resources are disabled, do not poll the provider: its consumer
    // heartbeat will expire and RenoDX can stop the producer-side capture and
    // composition passes as well.
    const auto config = Config::Instance();
    if (!config->FGDisableHudless.value_or_default() || !config->FGDisableUI.value_or_default())
        _ImportSoraFGResources();
    const auto perfAfterResourceImport = PerfClock::now();

    if (!_CopyDx11SharedToDx12FGBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;
    const auto perfAfterDx12CopySubmit = PerfClock::now();

    if (!_WaitForInteropCopyOnPresentQueue())
        return DXGI_ERROR_DEVICE_REMOVED;
    const auto perfAfterPresentQueueWait = PerfClock::now();

    const bool fgHookedPresenter =
        State::Instance().currentFGSwapchain == _fgSwapChain && !FGHooks::IsDx12InteropPresentSC(_fgSwapChain);

    // The game-facing DX11 swapchain is never presented in this wrapper.
    // For a plain external DX12 presenter, draw Opti's overlay here.
    // For a real FG swapchain, FGHooks::FGPresent/LocalPresent owns overlay/present-side work;
    // drawing it here would double-enter the overlay path before the FG present hook.
    if (!fgHookedPresenter)
        MenuOverlayDx::Present(_fgSwapChain, SyncInterval, Flags, nullptr, _dx12CommandQueue, _handle, false);
    else
        LOG_TRACE("real FG presenter detected; skipping wrapper overlay path");

    LOG_DEBUG("frame {}, dx11Index {}, real3Index {}, fakeIndex {}, bufferCount {}", State::Instance().frameCount,
              dx11Index, _real3 != nullptr ? _real3->GetCurrentBackBufferIndex() : 0xFFFFFFFF, _currentFakeIndex,
              _bufferCount);

    auto perfBeforeHiddenPresent = PerfClock::now();
    const bool skipHiddenPresent = Config::Instance()->Dx11SkipHiddenPresent.value_or_default();
    const bool nonBlockingHiddenPresent =
        Config::Instance()->Dx11NonBlockingHiddenPresent.value_or_default();
    bool providerBoundaryNotified = false;
    if (skipHiddenPresent)
    {
        const auto module = GetModuleHandleW(L"renodx-falcomengine.addon64");
        if (module != nullptr)
        {
            const auto notifyFrameBoundary = reinterpret_cast<PFN_RenoDX_NotifySoraFGFrameBoundaryV1>(
                GetProcAddress(module, "RenoDX_NotifySoraFGFrameBoundaryV1"));
            providerBoundaryNotified = notifyFrameBoundary != nullptr && notifyFrameBoundary();
        }

        const auto plusModule = GetModuleHandleW(L"renodx-falcomengine-plus.addon64");
        if (providerBoundaryNotified && plusModule != nullptr)
        {
            const auto notifyPlusFrameBoundary =
                reinterpret_cast<PFN_RenoDX_NotifyFalcomEnginePlusFrameBoundaryV1>(
                    GetProcAddress(plusModule, "RenoDX_NotifyFalcomEnginePlusFrameBoundaryV1"));
            providerBoundaryNotified =
                notifyPlusFrameBoundary != nullptr && notifyPlusFrameBoundary();
        }

        if (providerBoundaryNotified)
            ++_perfProviderBoundaryCount;
    }

    if (_real != nullptr && !providerBoundaryNotified)
    {
        UINT realFlags = Flags;

        if (nonBlockingHiddenPresent)
            realFlags |= DXGI_PRESENT_DO_NOT_WAIT;

        auto realPresentResult = _real->Present(0, realFlags);

        if (realPresentResult == DXGI_ERROR_WAS_STILL_DRAWING)
            ++_perfHiddenPresentWouldBlockCount;
        else if (SUCCEEDED(realPresentResult))
            ++_perfHiddenPresentSuccessCount;
        else
            LOG_WARN("hidden real DX11 Present failed: {:X}", (UINT) realPresentResult);
    }
    const auto perfAfterHiddenPresent = PerfClock::now();

    auto result = _fgSwapChain->Present(SyncInterval, Flags);
    const auto perfAfterFgPresent = PerfClock::now();

    if (SUCCEEDED(result))
        _AdvanceFakeBackBufferIndex();
    else
        LOG_ERROR("fg Present failed: {:X}", (UINT) result);

    const auto elapsedMs = [](auto begin, auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };

    _perfDx11CopyMs += elapsedMs(perfStageStart, perfAfterDx11Copy);
    _perfDx11SyncMs += elapsedMs(perfAfterDx11Copy, perfAfterDx11Sync);
    _perfResourceImportMs += elapsedMs(perfAfterDx11Sync, perfAfterResourceImport);
    _perfDx12CopySubmitMs += elapsedMs(perfAfterResourceImport, perfAfterDx12CopySubmit);
    _perfPresentQueueWaitMs += elapsedMs(perfAfterDx12CopySubmit, perfAfterPresentQueueWait);
    _perfOverlayMs += elapsedMs(perfAfterPresentQueueWait, perfBeforeHiddenPresent);
    _perfHiddenPresentMs += elapsedMs(perfBeforeHiddenPresent, perfAfterHiddenPresent);
    _perfFgPresentMs += elapsedMs(perfAfterHiddenPresent, perfAfterFgPresent);
    _perfTotalPresentMs += elapsedMs(perfStart, perfAfterFgPresent);

    constexpr UINT64 PERF_LOG_INTERVAL = 300;
    if (++_perfSampleCount >= PERF_LOG_INTERVAL)
    {
        const auto sampleCount = _perfSampleCount;
        const auto inverseSampleCount = 1.0 / static_cast<double>(sampleCount);
        LOG_INFO("Dx11wDx12 interop CPU averages over {} source frames [ms]: total {:.3f}, dx11 copy {:.3f}, "
                 "dx11 flush/sync {:.3f}, RenoDX import {:.3f}, dx12 copy submit {:.3f}, present-queue wait "
                 "submit {:.3f}, overlay {:.3f}, hidden Present {:.3f}, FG Present {:.3f}, allocator blocking "
                 "{:.3f} ({} waits), dedicated queue {}, hidden Present {} ({} succeeded, {} would-block, {} "
                 "provider-boundary calls)",
                 sampleCount, _perfTotalPresentMs * inverseSampleCount, _perfDx11CopyMs * inverseSampleCount,
                 _perfDx11SyncMs * inverseSampleCount, _perfResourceImportMs * inverseSampleCount,
                 _perfDx12CopySubmitMs * inverseSampleCount, _perfPresentQueueWaitMs * inverseSampleCount,
                 _perfOverlayMs * inverseSampleCount, _perfHiddenPresentMs * inverseSampleCount,
                 _perfFgPresentMs * inverseSampleCount, _perfAllocatorWaitMs * inverseSampleCount, _perfAllocatorWaitCount,
                 _usingDedicatedInteropQueue ? "yes" : "no",
                 skipHiddenPresent ? "provider-aware skip" : (nonBlockingHiddenPresent ? "nonblocking" : "blocking"),
                 _perfHiddenPresentSuccessCount, _perfHiddenPresentWouldBlockCount, _perfProviderBoundaryCount);

        _perfSampleCount = 0;
        _perfAllocatorWaitCount = 0;
        _perfHiddenPresentWouldBlockCount = 0;
        _perfHiddenPresentSuccessCount = 0;
        _perfProviderBoundaryCount = 0;
        _perfDx11CopyMs = 0.0;
        _perfDx11SyncMs = 0.0;
        _perfResourceImportMs = 0.0;
        _perfDx12CopySubmitMs = 0.0;
        _perfPresentQueueWaitMs = 0.0;
        _perfOverlayMs = 0.0;
        _perfHiddenPresentMs = 0.0;
        _perfFgPresentMs = 0.0;
        _perfTotalPresentMs = 0.0;
        _perfAllocatorWaitMs = 0.0;
    }

    return result;
}

bool Dx11wDx12SC::_ImportSoraFGResources()
{
    if (_fg == nullptr || _dx12Device == nullptr || State::Instance().activeFgOutput != FGOutput::DLSSG ||
        !_fg->IsActive() || _fg->IsPaused())
        return false;

    const auto module = GetModuleHandleW(L"renodx-falcomengine.addon64");
    if (module == nullptr)
        return false;

    const auto getResources = reinterpret_cast<PFN_RenoDX_GetSoraFGResourcesV1>(
        GetProcAddress(module, "RenoDX_GetSoraFGResourcesV1"));
    if (getResources == nullptr)
        return false;

    const auto experimentMode = Config::Instance()->FGDLSSGSoraUIExperimentMode.value_or_default();
    const auto setExperimentMode = reinterpret_cast<PFN_RenoDX_SetSoraFGUIExperimentModeV1>(
        GetProcAddress(module, "RenoDX_SetSoraFGUIExperimentModeV1"));
    if (setExperimentMode == nullptr || !setExperimentMode(experimentMode))
    {
        LOG_ERROR("RenoDX Sora FG experiment control ABI is unavailable or rejected mode {}", experimentMode);
        return false;
    }

    if (experimentMode == 8 || experimentMode == 9 || experimentMode == 11 || experimentMode == 12)
    {
        const auto velocity = Config::Instance()->FGDLSSGSoraMarkerVelocityPixelsPerFrame.value_or_default();
        const auto setVelocity = reinterpret_cast<PFN_RenoDX_SetSoraFGMarkerVelocityV1>(
            GetProcAddress(module, "RenoDX_SetSoraFGMarkerVelocityV1"));
        if (setVelocity == nullptr || !setVelocity(velocity))
        {
            LOG_ERROR("RenoDX Sora FG marker velocity ABI is unavailable or rejected {} px/application-frame",
                      velocity);
            return false;
        }
    }

    SoraFGInteropResourcesV1 info {};
    info.structSize = sizeof(info);
    if (!getResources(&info) || info.version != SORA_FG_INTEROP_VERSION || info.frameId == 0 ||
        info.frameId == _lastSoraFGFrame)
    {
        return false;
    }

    const bool expectBiasCurrentColor = experimentMode == 11;
    const uint32_t requiredUiFlag = experimentMode >= 2 ? SORA_FG_FLAG_UI_PREMULTIPLIED
                                                        : SORA_FG_FLAG_UI_ALPHA_ONLY;
    const DXGI_FORMAT expectedUiFormat = experimentMode >= 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                              : DXGI_FORMAT_R16_FLOAT;
    const uint32_t requiredFlags = SORA_FG_FLAG_HUDLESS | SORA_FG_FLAG_UI |
                                   SORA_FG_FLAG_HDR10_PQ_BT2020 | SORA_FG_FLAG_UI_FP16 | requiredUiFlag |
                                   (expectBiasCurrentColor ? SORA_FG_FLAG_BIAS_CURRENT_COLOR : 0u);
    if ((info.flags & requiredFlags) != requiredFlags || info.hudlessHandle == nullptr || info.uiHandle == nullptr ||
        info.hudlessFormat != DXGI_FORMAT_R10G10B10A2_UNORM ||
        info.uiFormat != expectedUiFormat || info.width == 0 || info.height == 0 ||
        info.experimentMode != experimentMode ||
        (expectBiasCurrentColor &&
         (info.biasCurrentColorHandle == nullptr || info.biasCurrentColorFormat != DXGI_FORMAT_R16_FLOAT)) ||
        _bufferFormat != DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        LOG_ERROR("RenoDX Sora FG resource contract mismatch. frame {}, mode {}/{}, flags {:X}, formats {}/{} size {}x{}",
                  info.frameId, info.experimentMode, experimentMode, info.flags, info.hudlessFormat, info.uiFormat,
                  info.width, info.height);
        return false;
    }

    // Keep polling in the one-resource-disabled diagnostic state so RenoDX's
    // producer remains active, but bypass both tags. The Sora contract is a
    // matched HUD-less/UI pair; submitting only one would be both invalid for
    // recomposition and would confound backend cost with producer cost.
    if (Config::Instance()->FGDisableHudless.value_or_default() ||
        Config::Instance()->FGDisableUI.value_or_default())
    {
        _lastSoraFGFrame = info.frameId;
        if (!_soraFGInteropLogged)
        {
            LOG_INFO("RenoDX Sora FG producer active, but HUD-less/UI import bypassed by DisableHudless/DisableUI");
            _soraFGInteropLogged = true;
        }
        return false;
    }

    SoraFGOpenedResources* opened = nullptr;
    for (auto& slot : _soraFGResources)
    {
        if (slot.hudlessHandle == info.hudlessHandle && slot.uiHandle == info.uiHandle &&
            slot.biasCurrentColorHandle == info.biasCurrentColorHandle)
        {
            opened = &slot;
            break;
        }
    }

    if (opened == nullptr)
    {
        for (auto& slot : _soraFGResources)
        {
            if (slot.hudless == nullptr && slot.ui == nullptr && slot.biasCurrentColor == nullptr)
            {
                opened = &slot;
                break;
            }
        }
    }

    // A fifth unique handle means RenoDX recreated the four-slot set, for
    // example after a resolution change. Drop the old D3D12 views as a unit.
    if (opened == nullptr)
    {
        _ReleaseSoraFGResources();
        opened = &_soraFGResources[0];
    }

    if (opened->hudless == nullptr || opened->ui == nullptr ||
        (expectBiasCurrentColor && opened->biasCurrentColor == nullptr))
    {
        SafeRelease(opened->hudless);
        SafeRelease(opened->ui);
        SafeRelease(opened->biasCurrentColor);
        opened->hudlessHandle = info.hudlessHandle;
        opened->uiHandle = info.uiHandle;
        opened->biasCurrentColorHandle = info.biasCurrentColorHandle;

        auto result = _dx12Device->OpenSharedHandle(info.hudlessHandle, IID_PPV_ARGS(&opened->hudless));
        if (FAILED(result) || opened->hudless == nullptr)
        {
            LOG_ERROR("OpenSharedHandle for RenoDX Sora HUD-less failed: {:X}", (UINT) result);
            return false;
        }

        result = _dx12Device->OpenSharedHandle(info.uiHandle, IID_PPV_ARGS(&opened->ui));
        if (FAILED(result) || opened->ui == nullptr)
        {
            LOG_ERROR("OpenSharedHandle for RenoDX Sora UI failed: {:X}", (UINT) result);
            SafeRelease(opened->hudless);
            return false;
        }

        if (expectBiasCurrentColor)
        {
            result = _dx12Device->OpenSharedHandle(info.biasCurrentColorHandle,
                                                   IID_PPV_ARGS(&opened->biasCurrentColor));
            if (FAILED(result) || opened->biasCurrentColor == nullptr)
            {
                LOG_ERROR("OpenSharedHandle for RenoDX Sora bias-current-color failed: {:X}", (UINT) result);
                SafeRelease(opened->hudless);
                SafeRelease(opened->ui);
                return false;
            }
        }

        const auto hudlessDesc = opened->hudless->GetDesc();
        const auto uiDesc = opened->ui->GetDesc();
        if (hudlessDesc.Width != info.width || hudlessDesc.Height != info.height ||
            hudlessDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM || uiDesc.Width != info.width ||
            uiDesc.Height != info.height || uiDesc.Format != expectedUiFormat ||
            (expectBiasCurrentColor &&
             (opened->biasCurrentColor->GetDesc().Width != info.width ||
              opened->biasCurrentColor->GetDesc().Height != info.height ||
              opened->biasCurrentColor->GetDesc().Format != DXGI_FORMAT_R16_FLOAT)))
        {
            LOG_ERROR("Opened RenoDX Sora FG resource descriptions do not match the export contract");
            SafeRelease(opened->hudless);
            SafeRelease(opened->ui);
            SafeRelease(opened->biasCurrentColor);
            return false;
        }
    }

    const int frameIndex = _fg->GetIndex();
    Dx12Resource hudless {};
    hudless.type = FG_ResourceType::HudlessColor;
    hudless.resource = opened->hudless;
    hudless.width = info.width;
    hudless.height = info.height;
    hudless.state = D3D12_RESOURCE_STATE_COMMON;
    hudless.validity = FG_ResourceValidity::UntilPresent;
    hudless.frameIndex = frameIndex;

    Dx12Resource ui {};
    ui.type = FG_ResourceType::UIColor;
    ui.resource = opened->ui;
    ui.width = info.width;
    ui.height = info.height;
    ui.state = D3D12_RESOURCE_STATE_COMMON;
    ui.validity = FG_ResourceValidity::UntilPresent;
    ui.frameIndex = frameIndex;

    Dx12Resource biasCurrentColor {};
    if (expectBiasCurrentColor)
    {
        biasCurrentColor.type = FG_ResourceType::BiasCurrentColor;
        biasCurrentColor.resource = opened->biasCurrentColor;
        biasCurrentColor.width = info.width;
        biasCurrentColor.height = info.height;
        biasCurrentColor.state = D3D12_RESOURCE_STATE_COMMON;
        biasCurrentColor.validity = FG_ResourceValidity::UntilPresent;
        biasCurrentColor.frameIndex = frameIndex;
    }

    // Bias and UI are inert without HUD-less/recomposition. Tag them first so
    // an unexpected later-call failure cannot leave DLSSG consuming HUD-less
    // while interpolating the final UI as part of the scene.
    const bool biasAccepted = !expectBiasCurrentColor || _fg->SetResource(&biasCurrentColor);
    const bool uiAccepted = biasAccepted && _fg->SetResource(&ui);
    const bool hudlessAccepted = uiAccepted && _fg->SetResource(&hudless);
    if (!hudlessAccepted || !uiAccepted || !biasAccepted)
    {
        LOG_WARN("RenoDX Sora FG resources were not accepted for frame {} (HUD-less {}, UI {}, bias {})",
                 info.frameId, hudlessAccepted, uiAccepted, biasAccepted);
        return false;
    }

    _lastSoraFGApplicationFrame = info.applicationFrameId;
    _lastSoraFGReplayFrame = info.replayFrameId;
    _lastSoraFGFinalResourceIdentity = info.finalResourceIdentity;
    _lastSoraFGHudlessResourceIdentity = info.hudlessResourceIdentity;
    _lastSoraFGUIResourceIdentity = info.uiResourceIdentity;
    _lastSoraFGBiasCurrentColorIdentity = info.biasCurrentColorIdentity;
    _lastSoraFGOpenedHudlessIdentity = reinterpret_cast<uint64_t>(opened->hudless);
    _lastSoraFGOpenedUIIdentity = reinterpret_cast<uint64_t>(opened->ui);
    _lastSoraFGOpenedBiasCurrentColorIdentity = reinterpret_cast<uint64_t>(opened->biasCurrentColor);
    _lastSoraFGReplayDrawCount = info.replayDrawCount;
    _lastSoraFGExperimentMode = info.experimentMode;
    _lastSoraFGFrame = info.frameId;
    if (!_soraFGInteropLogged)
    {
        LOG_INFO("RenoDX Sora FG interop active: HUD-less R10 PQ/BT.2020 + {} (format {}){}, {}x{}, experiment mode {}",
                 experimentMode >= 2 ? "premultiplied UI RGBA16F" : "UI alpha R16F", info.uiFormat,
                 expectBiasCurrentColor ? " + R16F bias-current-color" : "", info.width, info.height,
                 experimentMode);
        _soraFGInteropLogged = true;
    }
    return true;
}

void Dx11wDx12SC::_ReleaseSoraFGResources()
{
    for (auto& slot : _soraFGResources)
    {
        SafeRelease(slot.hudless);
        SafeRelease(slot.ui);
        SafeRelease(slot.biasCurrentColor);
        slot.hudlessHandle = nullptr;
        slot.uiHandle = nullptr;
        slot.biasCurrentColorHandle = nullptr;
    }
    _lastSoraFGFrame = 0;
    _lastSoraFGApplicationFrame = 0;
    _lastSoraFGReplayFrame = 0;
    _lastSoraFGFinalResourceIdentity = 0;
    _lastSoraFGHudlessResourceIdentity = 0;
    _lastSoraFGUIResourceIdentity = 0;
    _lastSoraFGBiasCurrentColorIdentity = 0;
    _lastSoraFGOpenedHudlessIdentity = 0;
    _lastSoraFGOpenedUIIdentity = 0;
    _lastSoraFGOpenedBiasCurrentColorIdentity = 0;
    _lastSoraFGReplayDrawCount = 0;
    _lastSoraFGExperimentMode = -1;
    _soraFGInteropLogged = false;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    return _real != nullptr ? _real->GetBuffer(Buffer, riid, ppSurface) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Dx11wDx12SC SetFullscreenState: {}, target: {:X}, caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetFullscreenState(Fullscreen, pTarget);

    return _real != nullptr ? _real->SetFullscreenState(Fullscreen, pTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenState(pFullscreen, ppTarget);

    return _real != nullptr ? _real->GetFullscreenState(pFullscreen, ppTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    auto result = _real->GetDesc(pDesc);
    if (SUCCEEDED(result) && pDesc != nullptr)
        pDesc->OutputWindow = _handle;

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                     UINT SwapChainFlags)
{
    const auto interopFormat = ResolveInteropFormat(NewFormat);
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) interopFormat, SwapChainFlags);

    if (!_WaitForCopyQueueIdle())
        LOG_WARN("continuing ResizeBuffers after copy fence wait failure");

    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropBackBuffers();

    HRESULT realResult = _real != nullptr ? _real->ResizeBuffers(BufferCount, Width, Height, interopFormat, SwapChainFlags)
                                          : DXGI_ERROR_DEVICE_REMOVED;

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
        fgResult = _fgSwapChain->ResizeBuffers(BufferCount, Width, Height, interopFormat, SwapChainFlags);

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        _RefreshCachedSwapchainDesc();
        _ApplyInteropColorSpace();

        if (Config::Instance()->FGEnabled.value_or_default())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

        if (State::Instance().currentFeature == nullptr)
        {
            State::Instance().screenWidth = static_cast<float>(Width);
            State::Instance().screenHeight = static_cast<float>(Height);
            State::Instance().lastMipBias = 100.0f;
            State::Instance().lastMipBiasMax = -100.0f;
        }
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->ResizeTarget(pNewTargetParameters)
               : (_real != nullptr ? _real->ResizeTarget(pNewTargetParameters) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetContainingOutput(ppOutput)
               : (_real != nullptr ? _real->GetContainingOutput(ppOutput) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _fgSwapChain != nullptr ? _fgSwapChain->GetFrameStatistics(pStats)
                                   : (_real != nullptr ? _real->GetFrameStatistics(pStats) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetLastPresentCount(pLastPresentCount)
               : (_real != nullptr ? _real->GetLastPresentCount(pLastPresentCount) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _real1 != nullptr ? _real1->GetDesc1(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenDesc(pDesc);

    return _real1 != nullptr ? _real1->GetFullscreenDesc(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetHwnd(HWND* pHwnd)
{
    if (pHwnd == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    *pHwnd = _handle;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1 != nullptr ? _real1->GetCoreWindow(refiid, ppUnk) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present1(UINT SyncInterval, UINT Flags,
                                                const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    UNREFERENCED_PARAMETER(pPresentParameters);
    return Present(SyncInterval, Flags);
}

BOOL STDMETHODCALLTYPE Dx11wDx12SC::IsTemporaryMonoSupported(void)
{
    return _real1 != nullptr ? _real1->IsTemporaryMonoSupported() : FALSE;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1 != nullptr ? _real1->GetRestrictToOutput(ppRestrictToOutput) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->SetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->GetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1 != nullptr ? _real1->SetRotation(Rotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1 != nullptr ? _real1->GetRotation(pRotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetSourceSize(UINT Width, UINT Height)
{
    return _real2 != nullptr ? _real2->SetSourceSize(Width, Height) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2 != nullptr ? _real2->GetSourceSize(pWidth, pHeight) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMaximumFrameLatency(UINT MaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetMaximumFrameLatency(MaxLatency);

    return _real2 != nullptr ? _real2->SetMaximumFrameLatency(MaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetMaximumFrameLatency(pMaxLatency);

    return _real2 != nullptr ? _real2->GetMaximumFrameLatency(pMaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HANDLE STDMETHODCALLTYPE Dx11wDx12SC::GetFrameLatencyWaitableObject(void)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFrameLatencyWaitableObject();

    return _real2 != nullptr ? _real2->GetFrameLatencyWaitableObject() : nullptr;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->SetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->GetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

UINT STDMETHODCALLTYPE Dx11wDx12SC::GetCurrentBackBufferIndex(void)
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _currentFakeIndex;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                              UINT* pColorSpaceSupport)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);

    return _real3 != nullptr ? _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport)
                             : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    if (IsHdrInteropRequested())
        ColorSpace = ResolveInteropColorSpace();

    State::Instance().isHdrActive = ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetColorSpace1(ColorSpace);

    return _real3 != nullptr ? _real3->SetColorSpace1(ColorSpace) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                      UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                      IUnknown* const* ppPresentQueue)
{
    const auto interopFormat = ResolveInteropFormat(Format);
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) interopFormat, SwapChainFlags);

    if (!_WaitForCopyQueueIdle())
        LOG_WARN("continuing ResizeBuffers1 after copy fence wait failure");

    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropBackBuffers();

    HRESULT realResult = _real3 != nullptr ? _real3->ResizeBuffers1(BufferCount, Width, Height, interopFormat, SwapChainFlags,
                                                                    pCreationNodeMask, ppPresentQueue)
                                           : ResizeBuffers(BufferCount, Width, Height, interopFormat, SwapChainFlags);

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
    {
        // The game's ppPresentQueue is not valid for the DX12 FG swapchain. Use ResizeBuffers for phase 1.
        fgResult = _fgSwapChain->ResizeBuffers(BufferCount, Width, Height, interopFormat, SwapChainFlags);
    }

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1 results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        _RefreshCachedSwapchainDesc();
        _ApplyInteropColorSpace();

        if (Config::Instance()->FGEnabled.value_or_default())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetHDRMetaData(Type, Size, pMetaData);

    return _real4 != nullptr ? _real4->SetHDRMetaData(Type, Size, pMetaData) : DXGI_ERROR_DEVICE_REMOVED;
}

bool Dx11wDx12SC::_ApplyInteropColorSpace()
{
    if (!IsHdrInteropRequested())
        return true;

    if (_fgSwapChain == nullptr)
        return false;

    const auto colorSpace = ResolveInteropColorSpace();
    UINT support = 0;
    auto result = _fgSwapChain->CheckColorSpaceSupport(colorSpace, &support);
    if (FAILED(result))
    {
        LOG_ERROR("Dx11wDx12SC CheckColorSpaceSupport({}) failed: {:X}", (UINT) colorSpace, (UINT) result);
        return false;
    }

    if ((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0)
    {
        LOG_ERROR("Dx11wDx12SC color space {} is not supported for presentation", (UINT) colorSpace);
        return false;
    }

    result = _fgSwapChain->SetColorSpace1(colorSpace);
    if (FAILED(result))
    {
        LOG_ERROR("Dx11wDx12SC SetColorSpace1({}) failed: {:X}", (UINT) colorSpace, (UINT) result);
        return false;
    }

    State::Instance().isHdrActive = true;
    LOG_INFO("Dx11wDx12SC HDR presentation active: format {}, color space {}", (UINT) _bufferFormat,
             (UINT) colorSpace);
    return true;
}

bool Dx11wDx12SC::_InitInteropObjects()
{
    if (_interopInitialized)
    {
        _RefreshCachedSwapchainDesc();

        if (_bufferCount == 0)
        {
            LOG_ERROR("InitInteropObjects resolved zero backbuffers");
            return false;
        }

        if (_sharedDx11BackBufferCopies.size() != _bufferCount)
            _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

        if (_openedDx11BackBuffers.size() != _bufferCount)
            _openedDx11BackBuffers.resize(_bufferCount, nullptr);

        if (_sharedBackBufferHandles.size() != _bufferCount)
            _sharedBackBufferHandles.resize(_bufferCount, nullptr);

        if (_openedDx11BackBufferStates.size() != _bufferCount)
            _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

        return true;
    }

    if (_dx11Device == nullptr || _dx11Device5 == nullptr || _dx11Context4 == nullptr || _dx12Device == nullptr ||
        _dx12CommandQueue == nullptr)
    {
        LOG_ERROR("interop objects missing: dx11 {}, dx11-5 {}, ctx4 {}, dx12 {}, queue {}", (UINT64) _dx11Device,
                  (UINT64) _dx11Device5, (UINT64) _dx11Context4, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
        return false;
    }

    HRESULT result = S_OK;

    if (_interopCommandQueue == nullptr && Config::Instance()->Dx11DedicatedInteropQueue.value_or_default())
    {
        auto queueDesc = _dx12CommandQueue->GetDesc();
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        result = _dx12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_interopCommandQueue));
        if (SUCCEEDED(result) && _interopCommandQueue != nullptr)
        {
            _usingDedicatedInteropQueue = true;
            LOG_INFO("Dx11wDx12 created dedicated interop Direct queue: {:X}; DLSSG presenting queue: {:X}",
                     (UINT64) _interopCommandQueue, (UINT64) _dx12CommandQueue);
        }
        else
            LOG_WARN("Dx11wDx12 failed to create dedicated interop queue ({:X}); falling back to DLSSG presenting "
                     "queue",
                     (UINT) result);
    }

    if (_interopCommandQueue == nullptr)
    {
        _interopCommandQueue = _dx12CommandQueue;
        _interopCommandQueue->AddRef();
        _usingDedicatedInteropQueue = false;
        LOG_INFO("Dx11wDx12 interop work uses DLSSG presenting queue (dedicated queue disabled or unavailable): {:X}",
                 (UINT64) _interopCommandQueue);
    }

    const UINT copyAllocatorCount = std::max<UINT>(_bufferCount != 0 ? _bufferCount : 3, 3);

    if (_copyAllocators.size() != copyAllocatorCount)
    {
        for (auto& allocator : _copyAllocators)
            SafeRelease(allocator);

        _copyAllocators.assign(copyAllocatorCount, nullptr);
    }

    if (_copyAllocatorFenceValues.size() != copyAllocatorCount)
        _copyAllocatorFenceValues.assign(copyAllocatorCount, 0);

    if (_copyCommandLists.size() != copyAllocatorCount)
    {
        for (auto& commandList : _copyCommandLists)
            SafeRelease(commandList);

        _copyCommandLists.assign(copyAllocatorCount, nullptr);
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyAllocators[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_copyAllocators[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandAllocator[{}] failed: {:X}", i, (UINT) result);
            return false;
        }
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyCommandLists[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _copyAllocators[i], nullptr,
                                                IID_PPV_ARGS(&_copyCommandLists[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandList failed: {:X}", (UINT) result);
            return false;
        }

        _copyCommandLists[i]->Close();
    }

    if (_copyFence == nullptr)
    {
        result = _dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_copyFence));
        if (FAILED(result))
        {
            LOG_ERROR("Create copy fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_copyFenceEvent == nullptr)
    {
        _copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (_copyFenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent for copy fence failed");
            return false;
        }
    }

    if (_dx11Fence == nullptr)
    {
        result = _dx11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_dx11Fence));
        if (FAILED(result))
        {
            LOG_ERROR("D3D11 CreateFence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_sharedFenceHandle == nullptr)
    {
        result = _dx11Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &_sharedFenceHandle);
        if (FAILED(result))
        {
            LOG_ERROR("CreateSharedHandle for D3D11 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_dx12SharedFence == nullptr)
    {
        result = _dx12Device->OpenSharedHandle(_sharedFenceHandle, IID_PPV_ARGS(&_dx12SharedFence));
        if (FAILED(result))
        {
            LOG_ERROR("OpenSharedHandle for D3D12 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    _RefreshCachedSwapchainDesc();

    if (_bufferCount == 0)
    {
        LOG_ERROR("InitInteropObjects resolved zero backbuffers");
        return false;
    }

    _sharedDx11BackBufferCopies.assign(_bufferCount, nullptr);
    _openedDx11BackBuffers.assign(_bufferCount, nullptr);
    _sharedBackBufferHandles.assign(_bufferCount, nullptr);
    _openedDx11BackBufferStates.assign(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    _interopInitialized = true;
    return true;
}

bool Dx11wDx12SC::_RequestSharedBackBuffer(UINT index)
{
    if (_real == nullptr || _dx11Device == nullptr || _dx12Device == nullptr)
        return false;

    if (_bufferCount == 0)
        _RefreshCachedSwapchainDesc();

    if (index >= _bufferCount)
    {
        LOG_ERROR("backbuffer index {} out of range {}", index, _bufferCount);
        return false;
    }

    if (_sharedDx11BackBufferCopies.size() <= index)
        _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

    if (_openedDx11BackBuffers.size() <= index)
        _openedDx11BackBuffers.resize(_bufferCount, nullptr);

    if (_sharedBackBufferHandles.size() <= index)
        _sharedBackBufferHandles.resize(_bufferCount, nullptr);

    if (_openedDx11BackBufferStates.size() <= index)
        _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    if (_openedDx11BackBuffers[_currentFakeIndex] != nullptr)
        return true;

    // Read Dx11 sc backbuffer
    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer({}) failed: {:X}", index, (UINT) result);
        return false;
    }

    // Create shared copy
    IDXGIResource1* dxgiResource = nullptr;
    if (_sharedBackBufferHandles[_currentFakeIndex] == nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        sourceTexture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        result = _dx11Device->CreateTexture2D(&desc, nullptr, &_sharedDx11BackBufferCopies[_currentFakeIndex]);
        if (FAILED(result) || _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("CreateTexture2D shadow buffer {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = _sharedDx11BackBufferCopies[_currentFakeIndex]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
        if (FAILED(result) || dxgiResource == nullptr)
        {
            LOG_ERROR("shadow IDXGIResource1 query {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                                  &_sharedBackBufferHandles[_currentFakeIndex]);
        dxgiResource->Release();

        if (FAILED(result) || _sharedBackBufferHandles[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("shadow CreateSharedHandle {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }
    }

    // Dx12 handle to Dx11 shared texture
    result = _dx12Device->OpenSharedHandle(_sharedBackBufferHandles[_currentFakeIndex],
                                           IID_PPV_ARGS(&_openedDx11BackBuffers[_currentFakeIndex]));
    sourceTexture->Release();

    if (FAILED(result) || _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        LOG_ERROR("OpenSharedHandle for backbuffer {} failed: {:X}", _currentFakeIndex, (UINT) result);
        return false;
    }

    _openedDx11BackBufferStates[_currentFakeIndex] = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

bool Dx11wDx12SC::_CopyDx11BackBufferToShared(UINT index)
{
    if (_currentFakeIndex >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    if (_dx11Context == nullptr || _real == nullptr)
        return false;

    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer for shadow copy {} failed: {:X}", index, (UINT) result);
        return false;
    }

    LOG_DEBUG("Copying DX11 backbuffer {} sourceTexture: {:X} to shadow copy {:X}", index, (size_t) sourceTexture,
              (size_t) _sharedDx11BackBufferCopies[_currentFakeIndex]);

    _dx11Context->CopyResource(_sharedDx11BackBufferCopies[_currentFakeIndex], sourceTexture);
    sourceTexture->Release();
    return true;
}

bool Dx11wDx12SC::_WaitDx11ThenDx12()
{
    if (_dx11Context4 == nullptr || _dx11Fence == nullptr || _interopCommandQueue == nullptr ||
        _dx12SharedFence == nullptr)
        return false;

    const auto waitValue = _sharedFenceValue++;

    auto result = _dx11Context4->Signal(_dx11Fence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("DX11 Signal failed: {:X}", (UINT) result);
        return false;
    }

    _dx11Context4->Flush();

    // Important:
    // Do not block the FG/present queue on the D3D11 shared fence.
    // Isolate the cross-API wait on the interop copy queue.
    result = _interopCommandQueue->Wait(_dx12SharedFence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("interop copy queue Wait on D3D11 fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_WaitForCopyAllocator(UINT slot)
{
    if (_copyFence == nullptr || _copyFenceEvent == nullptr)
        return true;

    if (slot >= _copyAllocatorFenceValues.size())
    {
        LOG_ERROR("copy allocator slot {} out of range {}", slot, _copyAllocatorFenceValues.size());
        return false;
    }

    const auto fenceValue = _copyAllocatorFenceValues[slot];

    if (fenceValue == 0)
        return true;

    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue >= fenceValue)
        return true;

    auto result = _copyFence->SetEventOnCompletion(fenceValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator fence SetEventOnCompletion failed. slot {}, fence {}, completed {}, result {:X}",
                  slot, fenceValue, completedValue, (UINT) result);
        return false;
    }

    const auto waitStart = std::chrono::steady_clock::now();
    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    _perfAllocatorWaitMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    ++_perfAllocatorWaitCount;
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("copy allocator fence wait failed. slot {}, fence {}, completed {}, waitResult {:X}", slot,
                  fenceValue, _copyFence->GetCompletedValue(), waitResult);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(UINT dx11Index)
{
    if (_copyAllocators.empty() || _copyCommandLists.empty() || _interopCommandQueue == nullptr || _copyFence == nullptr ||
        _fgSwapChain == nullptr || _currentFakeIndex >= _openedDx11BackBuffers.size() ||
        _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    const UINT copySlot = _currentFakeIndex;
    auto allocator = _copyAllocators[copySlot];

    if (allocator == nullptr)
        return false;

    if (!_WaitForCopyAllocator(copySlot))
        return false;

    auto result = allocator->Reset();
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator[{}] reset failed: {:X}", copySlot, (UINT) result);
        return false;
    }

    result = _copyCommandLists[copySlot]->Reset(allocator, nullptr);
    if (FAILED(result))
    {
        LOG_ERROR("copy command list reset failed: {:X}", (UINT) result);
        return false;
    }

    UINT fgIndex = _fgSwapChain->GetCurrentBackBufferIndex();
    LOG_DEBUG("dx11Index {}, fgIndex {}", dx11Index, fgIndex);

    ID3D12Resource* fgBackBuffer = nullptr;
    result = _fgSwapChain->GetBuffer(fgIndex, IID_PPV_ARGS(&fgBackBuffer));
    if (FAILED(result) || fgBackBuffer == nullptr)
    {
        LOG_ERROR("FG GetBuffer({}) failed: {:X}", fgIndex, (UINT) result);
        _copyCommandLists[copySlot]->Close();
        return false;
    }

    if (Config::Instance()->FGDLSSGSoraUIFrameDiagnostics.value_or_default() && _lastSoraFGFrame != 0)
    {
        LOG_INFO("[SoraFGFrame] provider={} app={} replay={} replayDraws={} mode={} dx11Index={} bridgeSlot={} "
                 "fgBackbufferIndex={} producerFinal={:X} producerHudless={:X} producerUI={:X} "
                 "producerBias={:X} openedHudless={:X} openedUI={:X} openedBias={:X} bridgeFinal={:X} "
                 "fgBackbuffer={:X} optiFGFrame={} optiFGIndex={}",
                 _lastSoraFGFrame, _lastSoraFGApplicationFrame, _lastSoraFGReplayFrame,
                 _lastSoraFGReplayDrawCount, _lastSoraFGExperimentMode, dx11Index, copySlot, fgIndex,
                 _lastSoraFGFinalResourceIdentity, _lastSoraFGHudlessResourceIdentity,
                 _lastSoraFGUIResourceIdentity, _lastSoraFGBiasCurrentColorIdentity,
                 _lastSoraFGOpenedHudlessIdentity, _lastSoraFGOpenedUIIdentity,
                 _lastSoraFGOpenedBiasCurrentColorIdentity,
                 reinterpret_cast<uint64_t>(_openedDx11BackBuffers[copySlot]),
                 reinterpret_cast<uint64_t>(fgBackBuffer), _fg->FrameCount(), _fg->GetIndex());
    }

    auto sourceBefore = _openedDx11BackBufferStates[copySlot];

    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], sourceBefore,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);

    _copyCommandLists[copySlot]->CopyResource(fgBackBuffer, _openedDx11BackBuffers[copySlot]);

    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], D3D12_RESOURCE_STATE_COPY_SOURCE,
                       sourceBefore);

    _openedDx11BackBufferStates[copySlot] = D3D12_RESOURCE_STATE_COMMON;

    fgBackBuffer->Release();

    result = _copyCommandLists[copySlot]->Close();
    if (FAILED(result))
    {
        LOG_ERROR("copy command list close failed: {:X}", (UINT) result);
        return false;
    }

    ID3D12CommandList* lists[] = { _copyCommandLists[copySlot] };
    _interopCommandQueue->ExecuteCommandLists(1, lists);

    const auto signalValue = ++_copyFenceValue;

    result = _interopCommandQueue->Signal(_copyFence, signalValue);
    if (FAILED(result))
    {
        LOG_ERROR("interop copy fence signal failed: {:X}", (UINT) result);
        return false;
    }

    _copyAllocatorFenceValues[copySlot] = signalValue;
    _lastInteropCopyFenceValue = signalValue;

    return true;
}

bool Dx11wDx12SC::_WaitForInteropCopyOnPresentQueue()
{
    if (_fg == nullptr || _copyFence == nullptr)
        return false;

    if (_lastInteropCopyFenceValue == 0)
        return true;

    auto result = _fg->GetCommandQueue()->Wait(_copyFence, _lastInteropCopyFenceValue);
    if (FAILED(result))
    {
        LOG_ERROR("present queue Wait on interop copy fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_WaitForCopyQueueIdle()
{
    if (_copyFence == nullptr || _copyFenceEvent == nullptr)
        return true;

    UINT64 waitValue = _lastInteropCopyFenceValue;

    for (const auto fenceValue : _copyAllocatorFenceValues)
        waitValue = std::max(waitValue, fenceValue);

    if (waitValue == 0)
        return true;

    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue >= waitValue)
        return true;

    auto result = _copyFence->SetEventOnCompletion(waitValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy queue idle SetEventOnCompletion failed. fence {}, completed {}, result {:X}", waitValue,
                  completedValue, (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("copy queue idle wait failed. fence {}, completed {}, waitResult {:X}", waitValue,
                  _copyFence->GetCompletedValue(), waitResult);
        return false;
    }

    for (auto& fenceValue : _copyAllocatorFenceValues)
        fenceValue = 0;

    _lastInteropCopyFenceValue = 0;

    return true;
}

void Dx11wDx12SC::_ReleaseInteropBackBuffers()
{
    _interopInitialized = false;
    _ReleaseSoraFGResources();

    for (auto& resource : _openedDx11BackBuffers)
        SafeRelease(resource);

    for (auto& texture : _sharedDx11BackBufferCopies)
        SafeRelease(texture);

    for (auto& handle : _sharedBackBufferHandles)
        SafeCloseHandle(handle);

    _openedDx11BackBufferStates.clear();
    _openedDx11BackBuffers.clear();
    _sharedDx11BackBufferCopies.clear();
    _sharedBackBufferHandles.clear();
}

void Dx11wDx12SC::_ReleaseInteropObjects()
{
    _WaitForCopyQueueIdle();
    _ReleaseInteropBackBuffers();

    _lastInteropCopyFenceValue = 0;

    SafeRelease(_dx12SharedFence);
    SafeRelease(_dx11Fence);
    SafeCloseHandle(_dx11FenceEvent);
    SafeCloseHandle(_sharedFenceHandle);

    for (auto& cmdList : _copyCommandLists)
        SafeRelease(cmdList);

    _copyCommandLists.clear();

    for (auto& allocator : _copyAllocators)
        SafeRelease(allocator);

    _copyAllocators.clear();
    _copyAllocatorFenceValues.clear();

    SafeRelease(_copyFence);
    SafeCloseHandle(_copyFenceEvent);
    _copyFenceValue = 1;

    SafeRelease(_interopCommandQueue);
    _usingDedicatedInteropQueue = false;

    _interopInitialized = false;
}

void Dx11wDx12SC::_RefreshCachedSwapchainDesc()
{
    _bufferCount = ResolveBufferCount(_real, _real1);
    _bufferFormat = ResolveBufferFormat(_real, _real1);
    _currentFakeIndex = _bufferCount > 0 ? _currentFakeIndex : 0;
}

UINT Dx11wDx12SC::_GetDx11BackBufferIndexForPresent() const
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _bufferCount > 0 ? _currentFakeIndex : 0;
}

void Dx11wDx12SC::_AdvanceFakeBackBufferIndex() { _currentFakeIndex = (_currentFakeIndex + 1) % _bufferCount; }
