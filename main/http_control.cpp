/**
 * @file http_control.cpp
 * @brief GPIO38 电平输出与监听 80 端口的 HTTP 控制服务，见 @ref http_control.hpp 。
 */
#include "http_control.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace http_control {
namespace {

constexpr const char* kTag = "et_http";

/// 电平输出引脚：要换引脚只改这里（换之前先确认本板该脚空闲，见头文件警告）。
constexpr gpio_num_t kLevelPin = GPIO_NUM_38;
/// 上电默认电平：1 = 高。
constexpr int kInitialLevel = 1;
/// HTTP 监听端口（固定 80，虚拟 IP 上直接 http://<虚拟IP>/ 访问）。
constexpr uint16_t kHttpPort = 80;
/// HTTPD 任务栈：只做字符串拼装与 gpio_set_level，4096 与 esp_http_server 默认值一致。
constexpr uint32_t kHttpTaskStack = 4096;
/// HTTPD 任务优先级：与组件控制面任务同级，低于 WiFi/TCPIP。
constexpr UBaseType_t kHttpTaskPriority = tskIDLE_PRIORITY + 5;
/// 只需要两个路由，按需收窄表大小。
constexpr uint16_t kMaxUriHandlers = 4;

httpd_handle_t s_server = nullptr;

/**
 * @brief 当前输出电平的"影子寄存器"。
 * @note 不用 @c gpio_get_level() 读回：输出脚若被外部强制拉低，读回的是外部电平，
 *       会让"翻转"方向出错。这里以软件记录为准，硬件只是被驱动过去。
 */
std::atomic<int> s_level{kInitialLevel};

/** @brief 翻转 GPIO38 电平并返回新电平。 */
int level_toggle() noexcept {
    int expected = s_level.load();
    int next = 0;
    do {
        next = expected ? 0 : 1;
    } while (!s_level.compare_exchange_weak(expected, next));
    (void)gpio_set_level(kLevelPin, next);
    return next;
}

/** @brief 处理翻转请求（`GET /` 与 `GET /toggle`）：返回一个极小的 HTML 页面。 */
esp_err_t handle_toggle(httpd_req_t* req) {
    const int level = level_toggle();
    // 只有一个 <h1>，正文就是当前电平状态；整个页面不到 200 字节。
    char body[192];
    const int len = std::snprintf(
        body, sizeof(body),
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<title>GPIO%d 电平</title></head><body><h1>%s</h1></body></html>",
        static_cast<int>(kLevelPin), level ? "高电平" : "低电平");
    if (len <= 0 || len >= static_cast<int>(sizeof(body))) {
        return httpd_resp_send_500(req);
    }
    (void)httpd_resp_set_type(req, "text/html; charset=utf-8");
    // 翻转带副作用：禁止缓存，否则浏览器刷新/代理重放不会真的再翻一次。
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ESP_LOGI(kTag, "GET %s -> gpio%d = %d", req->uri, static_cast<int>(kLevelPin), level);
    return httpd_resp_send(req, body, len);
}

/** @brief 把 GPIO38 配成推挽输出并置为默认高电平。 */
esp_err_t init_level_output() noexcept {
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << static_cast<unsigned>(kLevelPin);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(kLevelPin, kInitialLevel);
}

/** @brief 启动 HTTP 服务并注册路由；失败时回收半成品。 */
httpd_handle_t start_server() noexcept {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = kHttpPort;
    cfg.task_priority = kHttpTaskPriority;
    cfg.stack_size = kHttpTaskStack;
    cfg.max_uri_handlers = kMaxUriHandlers;
    // 端口被占满时回收最久未用的连接，避免一个挂住的监控客户端把服务顶死。
    cfg.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed on port %u", static_cast<unsigned>(kHttpPort));
        return nullptr;
    }
    // 两个入口都做同一件事：GET / 满足"直接访问 http://<虚拟IP>/"，/toggle 语义更明确。
    // 默认 URI 匹配是精确比较（uri_match_fn 为 NULL），所以 /favicon.ico 之类不会误触发。
    const char* const uris[] = {"/", "/toggle"};
    for (const char* uri : uris) {
        httpd_uri_t route{};
        route.uri = uri;
        route.method = HTTP_GET;
        route.handler = handle_toggle;
        route.user_ctx = nullptr;
        if (httpd_register_uri_handler(server, &route) != ESP_OK) {
            ESP_LOGE(kTag, "register %s failed", uri);
            (void)httpd_stop(server);
            return nullptr;
        }
    }
    return server;
}

}  // namespace

esp_err_t start() noexcept {
    if (s_server != nullptr) {
        return ESP_OK;  // 幂等
    }
    const esp_err_t pin_err = init_level_output();
    if (pin_err != ESP_OK) {
        ESP_LOGE(kTag, "gpio%d init failed: 0x%x", static_cast<int>(kLevelPin),
                 static_cast<int>(pin_err));
        return pin_err;
    }
    ESP_LOGI(kTag, "gpio%d enabled as output, level=%d", static_cast<int>(kLevelPin),
             kInitialLevel);

    s_server = start_server();
    if (s_server == nullptr) {
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "http listening on port %u: GET / or /toggle flips gpio%d",
             static_cast<unsigned>(kHttpPort), static_cast<int>(kLevelPin));
    return ESP_OK;
}

}  // namespace http_control
