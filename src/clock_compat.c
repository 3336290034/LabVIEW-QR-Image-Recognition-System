/**
 * clock_gettime64_compat.c - 兼容层
 * 
 * 新版 mingw-w64 CRT 中 clock_gettime64 已被合并/移除，
 * 但旧版编译的 ZBar 仍引用 clock_gettime64。
 * 此文件使用 Windows API 直接实现来桥接差异。
 */

#include <windows.h>
#include <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

__attribute__((used))
int clock_gettime64(clockid_t clk_id, struct timespec *tp) {
    if (!tp) return -1;
    
    ULARGE_INTEGER ul;
    FILETIME ft;
    
    if (clk_id == CLOCK_MONOTONIC) {
        /* 单调时钟：使用 GetTickCount64 或 QueryPerformanceCounter */
        LARGE_INTEGER freq, count;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&count);
        tp->tv_sec = (long long)(count.QuadPart / freq.QuadPart);
        tp->tv_nsec = (long long)((count.QuadPart % freq.QuadPart) * 1000000000ULL / freq.QuadPart);
        return 0;
    }
    
    /* 默认：REALTIME 时钟，使用 GetSystemTimePreciseAsFileTime */
    GetSystemTimePreciseAsFileTime(&ft);
    ul.LowPart = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    
    /* FILETIME 是 100ns 精度，从 1601-01-01 起 */
    /* Unix epoch 是 1970-01-01，差值 = 11644473600 秒 */
    ul.QuadPart -= 116444736000000000ULL;
    
    tp->tv_sec = (long long)(ul.QuadPart / 10000000ULL);
    tp->tv_nsec = (long long)((ul.QuadPart % 10000000ULL) * 100);
    
    return 0;
}
