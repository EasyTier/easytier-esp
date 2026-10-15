/*
 * @file log.hpp
 * @brief ESP-IDF 日志宏的编译期等级裁剪。
 */
#pragma once

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "sdkconfig.h"
#endif

/* 0=Closed, 1=Error, 2=Warning, 3=Info, 4=Debug. */
#if defined(CONFIG_EASYTIER_ESP_LOG_CLOSED)
#define ET_LOG_LEVEL 0
#elif defined(CONFIG_EASYTIER_ESP_LOG_ERROR)
#define ET_LOG_LEVEL 1
#elif defined(CONFIG_EASYTIER_ESP_LOG_WARNING)
#define ET_LOG_LEVEL 2
#elif defined(CONFIG_EASYTIER_ESP_LOG_DEBUG)
#define ET_LOG_LEVEL 4
#else
#define ET_LOG_LEVEL 3
#endif

#if defined(ESP_PLATFORM)

#if ET_LOG_LEVEL >= 1
#define ET_LOGE(...) ESP_LOGE(__VA_ARGS__)
#else
#define ET_LOGE(...) ((void)0)
#endif

#if ET_LOG_LEVEL >= 2
#define ET_LOGW(...) ESP_LOGW(__VA_ARGS__)
#else
#define ET_LOGW(...) ((void)0)
#endif

#if ET_LOG_LEVEL >= 3
#define ET_LOGI(...) ESP_LOGI(__VA_ARGS__)
#else
#define ET_LOGI(...) ((void)0)
#endif

#if ET_LOG_LEVEL >= 4
#define ET_LOGD(...) ESP_LOGD(__VA_ARGS__)
#else
#define ET_LOGD(...) ((void)0)
#endif

#else

/* Host-side builds keep the ESP_LOGX-compatible call shape but emit no logs. */
#define ET_LOGE(...) ((void)0)
#define ET_LOGW(...) ((void)0)
#define ET_LOGI(...) ((void)0)
#define ET_LOGD(...) ((void)0)

#endif
