#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Screen Dimensions (Kindle Paperwhite 5 Carta 1200 Display: 1236 x 1648 Portrait) */
#define SCREEN_WIDTH_PORTRAIT   1236
#define SCREEN_HEIGHT_PORTRAIT  1648

#ifdef _WIN32
#define DEFAULT_SCREEN_W 800
#define DEFAULT_SCREEN_H 600
#else
#define DEFAULT_SCREEN_W SCREEN_WIDTH_PORTRAIT
#define DEFAULT_SCREEN_H SCREEN_HEIGHT_PORTRAIT
#endif

/* E-Ink Grayscale Palette (4-bit 16 levels of gray: 0=Black, 255=White) */
#define EINK_BLACK      0x00
#define EINK_VERY_DARK  0x22
#define EINK_DARK_GRAY  0x55
#define EINK_MID_GRAY   0x88
#define EINK_LIGHT_GRAY 0xBB
#define EINK_NEAR_WHITE 0xDD
#define EINK_WHITE      0xFF

/* Logging macros */
void win32_app_request_load(const char *path);
#define LOG_TAG "kindle_win32"
#define LOGI(fmt, ...) printf("[INFO][%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#define LOGW(fmt, ...) printf("[WARN][%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, "[ERR ][%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#ifdef DEBUG_WIN32
#define LOGD(fmt, ...) printf("[DBG ][%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#else
#define LOGD(fmt, ...) do {} while (0)
#endif

#ifdef _WIN32
__declspec(dllimport) void __stdcall Sleep(unsigned long dwMilliseconds);
static inline void sys_sleep_ms(uint32_t ms) {
    Sleep(ms);
}
#else
#include <unistd.h>
static inline void sys_sleep_ms(uint32_t ms) {
    usleep((useconds_t)ms * 1000);
}
#endif

void win32_translate_path(const char *win_path, char *out_host, size_t out_sz);

#endif /* COMMON_H */
