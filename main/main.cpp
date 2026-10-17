/**
 * @file main.cpp
 * @brief EasyTier-ESP 示例入口：NVS、TCP/IP、WiFi 与模块装配。
 *
 * @details 示例刻意保持"只做应用侧必须负责的事"，并按职责拆成三个模块：
 * 1. **本文件**：NVS、TCP/IP 栈、WiFi 连接与重连、装配并启动下面的两个模块；
 * 2. @ref easytier_app ：加载内嵌 TOML、启动组网任务、上报组网事件与周期状态；
 * 3. @ref http_control ：GPIO38 电平输出 + 监听 80 端口的 HTTP 控制服务。
 *
 * 协议、加密、RPC、路由与虚拟网卡全部由 components/easytier-esp 负责，
 * 本目录不含任何协议实现；本文件也不再直接引用 `et::` 任何符号。
 */
#include <cstring>

#include "easytier_app.hpp"
#include "http_control.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "et_example";

/** @brief WiFi 重连计数。 */
int g_retry = 0;

/** @brief WiFi 与 IP 事件处理（必须快速返回，重活交给各模块）。 */
void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // WiFi 断开：先让组件 disconnect() 撤销连接与路由，再重连 WiFi。
        easytier_app::on_ip_lost();
        const int max_retry = CONFIG_EXAMPLE_WIFI_MAX_RETRY;
        if (max_retry == 0 || g_retry < max_retry) {
            ++g_retry;
            ESP_LOGW(kTag, "wifi disconnected, reconnecting (%d)", g_retry);
            (void)esp_wifi_connect();
        } else {
            ESP_LOGE(kTag, "wifi reconnect attempts exhausted");
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(data);
        ESP_LOGI(kTag, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        g_retry = 0;
        // 首次调用会创建一次性组网启动任务；之后只再调用 connect()。
        easytier_app::on_ip_got();
    }
}

/** @brief 初始化 NVS（必要时擦除重建）。 */
void init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/** @brief 初始化 TCP/IP 栈与默认事件循环（HTTP 服务与 WiFi 都依赖它）。 */
void init_tcpip() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

/** @brief 初始化 WiFi STA 并开始连接。 */
void init_wifi() {
    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr));

    wifi_config_t sta_config{};
    // 凭据用的是字面量：Kconfig 里的 EXAMPLE_WIFI_SSID / EXAMPLE_WIFI_PASSWORD 目前
    // 没有被任何代码引用。要改成走 menuconfig，把下面两行的字面量换成这两个宏即可。
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.ssid), "ET_TEST_SSID",
                 sizeof(sta_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.password), "ET_TEST_PWD",
                 sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

}  // namespace

/**
 * @brief 应用入口。
 * @note app_main 必须使用 C 链接（ESP-IDF 启动流程按 C 符号查找）。
 */
extern "C" void app_main(void) {
    init_nvs();
    ESP_LOGI(kTag, "EasyTier-ESP example starting");

    // TCP/IP 先起来：HTTP 服务需要 socket，WiFi 也需要 netif 层的默认事件循环。
    init_tcpip();

    // 电平输出与 HTTP 服务尽早就绪（GPIO38 上电即高）。失败只告警：它是示例扩展，
    // 不应该拖累组网主流程。
    const esp_err_t http_err = http_control::start();
    if (http_err != ESP_OK) {
        ESP_LOGW(kTag, "http control service unavailable: 0x%x",
                 static_cast<unsigned>(http_err));
    }

    init_wifi();
    // 之后由 WiFi/IP 事件驱动：取得 IP → easytier_app::on_ip_got() 启动组网。
}
