#pragma once
#include "SysUtils.h"

class FrameLimit
{
    static uint64_t get_timestamp();
    static int timer_sleep(int64_t hundred_ns);
    static int busywait_sleep(int64_t ns);
    static int combined_sleep(int64_t ns);

  public:
    static bool is_fg_active();
    static uint32_t get_fg_multiplier(bool fgActive);
    static int get_refresh_rate();
    static float get_output_fps_limit(bool fgActive);
    static float get_native_fps_limit(bool fgActive);
    static void sleep(bool fgActive);
};
