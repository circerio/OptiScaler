#include "pch.h"
#include "FrameLimit.h"

#include "Config.h"
#include "State.h"
#include "Util.h"
// #include "hooks/D3D11Hooks.h"

namespace
{
std::mutex refreshRateMutex;
HWND refreshRateHwnd = nullptr;
HMONITOR refreshRateMonitor = nullptr;
int cachedRefreshRate = 0;
uint64_t refreshRateQueryTime = 0;
} // namespace

inline uint64_t FrameLimit::get_timestamp()
{
    FILETIME fileTime;
    GetSystemTimePreciseAsFileTime(&fileTime);

    uint64_t time = (static_cast<uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;

    return time * 100;
}

// https://learn.microsoft.com/en-us/windows/win32/sync/using-waitable-timer-objects
inline int FrameLimit::timer_sleep(int64_t hundred_ns)
{
    static HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due_time;

    due_time.QuadPart = -hundred_ns;

    if (!timer)
        return 1;

    if (!SetWaitableTimerEx(timer, &due_time, 0, NULL, NULL, NULL, 0))
        return 2;

    if (WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0)
        return 3;

    return 0;
};

inline int FrameLimit::busywait_sleep(int64_t ns)
{
    auto current_time = get_timestamp();
    auto wait_until = current_time + ns;
    while (current_time < wait_until)
    {
        current_time = get_timestamp();
    }
    return 0;
}

inline int FrameLimit::combined_sleep(int64_t ns)
{
    constexpr int64_t busywait_threshold = 2'000'000; // 2ms
    int status {};
    auto current_time = get_timestamp();
    if (ns <= busywait_threshold)
        status = busywait_sleep(ns);
    else
        status = timer_sleep((ns - busywait_threshold) / 100);

    if (int64_t sleep_deviation = ns - (get_timestamp() - current_time); sleep_deviation > 0 && !status)
        status = busywait_sleep(sleep_deviation);

    return status;
}

void FrameLimit::sleep(bool fgActive)
{
    static uint64_t previousFrameTime = 0;
    static float previousFpsCap = 0.0f;

    const auto fpsCap = get_native_fps_limit(fgActive);
    if (fpsCap <= 0.0f || !std::isfinite(fpsCap))
    {
        previousFrameTime = 0;
        previousFpsCap = 0.0f;
        return;
    }

    if (fpsCap != previousFpsCap)
    {
        previousFrameTime = 0;
        previousFpsCap = fpsCap;
    }

    const auto minIntervalNs = std::clamp(static_cast<uint64_t>(std::llround(1'000'000'000.0 / fpsCap)), 1ULL,
                                          100'000'000'000ULL);
    const auto currentTime = get_timestamp();
    const auto frameTime = previousFrameTime == 0 ? minIntervalNs : currentTime - previousFrameTime;

    if (frameTime < minIntervalNs)
    {
        if (auto res = combined_sleep(minIntervalNs - frameTime); res)
            LOG_ERROR("Sleep command failed: {}", res);
    }

    previousFrameTime = get_timestamp();
}

bool FrameLimit::is_fg_active()
{
    const auto& state = State::Instance();
    const auto fg = state.currentFG;

    if (!Config::Instance()->FGEnabled.value_or_default() || state.activeFgOutput == FGOutput::NoFG || fg == nullptr ||
        !fg->IsActive() || fg->IsPaused())
    {
        return false;
    }

    // DLSSG explicitly clears this value when interpolation stops.  Checking it avoids
    // retaining the automatic native-frame cap while the wrapper waits for new frame data.
    if (state.activeFgOutput == FGOutput::DLSSG)
        return state.dlssgDetectedInterpolationCount > 0;

    return true;
}

uint32_t FrameLimit::get_fg_multiplier(bool fgActive)
{
    if (!fgActive)
        return 1;

    const auto& state = State::Instance();
    if (state.activeFgOutput == FGOutput::DLSSG && state.dlssgDetectedInterpolationCount > 0)
        return static_cast<uint32_t>(state.dlssgDetectedInterpolationCount + 1);

    if (state.currentFG != nullptr)
        return std::max(2U, state.currentFG->GetInterpolatedFrameCount() + 1U);

    return 2;
}

int FrameLimit::get_refresh_rate()
{
    const auto fg = State::Instance().currentFG;
    const auto hwnd = fg != nullptr ? fg->Hwnd() : GetForegroundWindow();
    if (hwnd == nullptr)
        return 0;

    const auto monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    const auto now = get_timestamp();

    std::lock_guard<std::mutex> lock(refreshRateMutex);
    constexpr uint64_t refreshIntervalNs = 2'000'000'000ULL;
    if (hwnd != refreshRateHwnd || monitor != refreshRateMonitor || now - refreshRateQueryTime >= refreshIntervalNs)
    {
        refreshRateHwnd = hwnd;
        refreshRateMonitor = monitor;
        refreshRateQueryTime = now;

        const auto detectedRefreshRate = Util::GetActiveRefreshRate(hwnd);
        if (detectedRefreshRate != cachedRefreshRate)
        {
            LOG_INFO("Detected active refresh rate: {} Hz", detectedRefreshRate);
            cachedRefreshRate = detectedRefreshRate;
        }
    }

    return cachedRefreshRate;
}

float FrameLimit::get_output_fps_limit(bool fgActive)
{
    const auto manualLimit = Config::Instance()->FramerateLimit.value_or_default();
    if (manualLimit > 0.0f && std::isfinite(manualLimit))
        return manualLimit;

    if (!fgActive || !Config::Instance()->AutoFramerateLimit.value_or_default())
        return 0.0f;

    const auto refreshRate = get_refresh_rate();
    if (refreshRate <= 1)
        return 0.0f;

    const auto marginMs = std::clamp(Config::Instance()->AutoFramerateLimitMarginMs.value_or_default(), 0.0f, 10.0f);
    const auto outputLimit = 1000.0f / (1000.0f / static_cast<float>(refreshRate) + marginMs);
    return std::round(outputLimit * 10.0f) / 10.0f;
}

float FrameLimit::get_native_fps_limit(bool fgActive)
{
    const auto outputLimit = get_output_fps_limit(fgActive);
    if (outputLimit <= 0.0f)
        return 0.0f;

    return outputLimit / static_cast<float>(get_fg_multiplier(fgActive));
}
