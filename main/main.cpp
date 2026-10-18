/**
 * @file main.cpp
 * @brief EasyTier-ESP 示例：NVS、TCP/IP、WiFi、HTTP 与组网流程。
 *
 * @details 组网全部在 app_main（main 任务）执行；WiFi/IP 事件回调只更新状态并置位。
 *          应用层只有一个额外任务（HTTPD，由 esp_http_server 创建）。
 */
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

#include "easytier-esp.hpp"
#include "http_control.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "et_example";

/// WiFi 连接状态发生变化（取得 IP 或断开）的事件位。
constexpr EventBits_t kUnderlayChangedBit = BIT0;

EventGroupHandle_t g_events = nullptr;
/// WiFi 当前是否持有 IP（事件任务写、main 任务读）。
std::atomic<bool> g_wifi_has_ip{false};
int g_retry = 0;

#if CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED
/// 组网是否已成功启动。
bool g_started = false;
/// 运行期实例；在 app_main 中构造，生命周期覆盖整个应用。
std::optional<et::Inst> g_inst;
#endif

#if CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED && defined(EASYTIER_EXAMPLE_TOML_EMBEDDED)
extern const uint8_t easytier_toml_start[] asm("_binary_easytier_toml_start");
extern const uint8_t easytier_toml_end[] asm("_binary_easytier_toml_end");
#endif

/** @brief WiFi/IP 事件：只更新状态并重连，耗时的组网处理留给 app_main。 */
void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_has_ip.store(false);
        xEventGroupSetBits(g_events, kUnderlayChangedBit);
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
        g_wifi_has_ip.store(true);
        xEventGroupSetBits(g_events, kUnderlayChangedBit);
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
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.ssid), CONFIG_EXAMPLE_WIFI_SSID,
                 sizeof(sta_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.password),
                 CONFIG_EXAMPLE_WIFI_PASSWORD, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

#if CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED

/**
 * @brief 组件事件回调：只做打印，保持快速返回。
 * @note 回调在控制面任务（`et_proto`）上下文中同步执行，只允许调用非阻塞接口。
 */
[[maybe_unused]] void on_easytier_event(const et::Event& event, void*) noexcept {
    switch (event.type) {
        case et::EventType::Started:
            ESP_LOGI(kTag, "easytier started, peer_id=%u",
                     static_cast<unsigned>(event.status.peer_id));
            break;
        case et::EventType::PeerUp:
            ESP_LOGI(kTag, "peer connected: %u", static_cast<unsigned>(event.peer_id));
            break;
        case et::EventType::NetworkReady:
            ESP_LOGI(kTag, "network ready, virtual ip=%u.%u.%u.%u/%u",
                     static_cast<unsigned>((event.status.virtual_ipv4 >> 24) & 0xff),
                     static_cast<unsigned>((event.status.virtual_ipv4 >> 16) & 0xff),
                     static_cast<unsigned>((event.status.virtual_ipv4 >> 8) & 0xff),
                     static_cast<unsigned>(event.status.virtual_ipv4 & 0xff),
                     static_cast<unsigned>(event.status.prefix_len));
            break;
        case et::EventType::PeerDown:
            ESP_LOGW(kTag, "peer disconnected: %u", static_cast<unsigned>(event.peer_id));
            break;
        case et::EventType::Error:
            ESP_LOGE(kTag, "easytier error: 0x%x", event.status.last_error);
            break;
        case et::EventType::Stopped:
            ESP_LOGI(kTag, "easytier stopped");
            break;
    }
}

/** @brief 把 16 字节 UUID 格式化成 36 字符文本（8-4-4-4-12）。 */
[[maybe_unused]] void format_uuid(const std::array<uint8_t, 16>& id,
                                  std::array<char, 37>& out) {
    (void)snprintf(out.data(), out.size(),
                   "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                   id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9],
                   id[10], id[11], id[12], id[13], id[14], id[15]);
}

/** @brief 状态名（日志用）。 */
const char* state_name(et::State state) {
    switch (state) {
        case et::State::Uninitialized: return "uninitialized";
        case et::State::Stopped: return "stopped";
        case et::State::WaitingUnderlay: return "waiting-underlay";
        case et::State::Connecting: return "connecting";
        case et::State::Handshaking: return "handshaking";
        case et::State::Syncing: return "syncing";
        case et::State::Ready: return "ready";
        case et::State::Stopping: return "stopping";
        case et::State::Error: return "error";
    }
    return "unknown";
}

/** @brief 打印当前组网状态（main 主循环每 10 秒调用一次）。 */
void log_status() {
    if (!g_inst.has_value()) {
        return;
    }
    et::Status status{};
    if (g_inst->get_status(status) != ESP_OK) {
        return;
    }
    char vip[24] = "none";
    if (status.virtual_ipv4 != 0) {
        (void)snprintf(vip, sizeof(vip), "%u.%u.%u.%u/%u",
                       static_cast<unsigned>((status.virtual_ipv4 >> 24) & 0xFF),
                       static_cast<unsigned>((status.virtual_ipv4 >> 16) & 0xFF),
                       static_cast<unsigned>((status.virtual_ipv4 >> 8) & 0xFF),
                       static_cast<unsigned>(status.virtual_ipv4 & 0xFF),
                       static_cast<unsigned>(status.prefix_len));
    }
    ESP_LOGI(kTag,
             "status: state=%s peer_id=0x%08x vip=%s direct=%u routes=%u dropped=%llu err=0x%x",
             state_name(status.state), static_cast<unsigned>(status.peer_id), vip,
             static_cast<unsigned>(status.direct_peer_count),
             static_cast<unsigned>(status.reachable_route_count),
             static_cast<unsigned long long>(status.dropped_packets),
             static_cast<int>(status.last_error));
}

/** @brief 打印实际生效的配置（TOML 映射结果 + 身份补齐）。 */
[[maybe_unused]] void log_effective_config() {
    if (!g_inst.has_value()) {
        return;
    }
    const et::Settings cfg = g_inst->get_config_info();
    std::array<char, 37> uuid{};
    format_uuid(cfg.instance_id, uuid);
    ESP_LOGI(kTag, "config: instance=%s hostname=%s network=%s instance_id=%s",
             cfg.instance_name.c_str(), cfg.hostname.c_str(), cfg.network_name.c_str(),
             uuid.data());
    for (size_t i = 0; i < cfg.peers.size(); ++i) {
        ESP_LOGI(kTag, "config: peer[%u]=%s:%u", static_cast<unsigned>(i),
                 cfg.peers[i].host.c_str(), static_cast<unsigned>(cfg.peers[i].port));
    }
    if (cfg.has_static_ipv4) {
        ESP_LOGI(kTag, "config: ipv4=%u.%u.%u.%u/%u mtu=%u",
                 static_cast<unsigned>((cfg.static_ipv4 >> 24) & 0xFF),
                 static_cast<unsigned>((cfg.static_ipv4 >> 16) & 0xFF),
                 static_cast<unsigned>((cfg.static_ipv4 >> 8) & 0xFF),
                 static_cast<unsigned>(cfg.static_ipv4 & 0xFF),
                 static_cast<unsigned>(cfg.static_prefix), static_cast<unsigned>(cfg.mtu));
    } else if (cfg.dhcp) {
        const uint32_t subnet = cfg.dhcp_subnet;
        ESP_LOGI(kTag, "config: dhcp=%u.%u.%u.%u/%u mtu=%u",
                 static_cast<unsigned>((subnet >> 24) & 0xFF),
                 static_cast<unsigned>((subnet >> 16) & 0xFF),
                 static_cast<unsigned>((subnet >> 8) & 0xFF),
                 static_cast<unsigned>(subnet & 0xFF),
                 static_cast<unsigned>(cfg.dhcp_prefix), static_cast<unsigned>(cfg.mtu));
    } else {
        ESP_LOGW(kTag, "config: 既没有静态 ipv4 也没有启用 dhcp，虚拟网卡不会创建");
    }
    ESP_LOGI(kTag, "config: encryption=%s", cfg.enable_encryption ? "aes-gcm" : "disabled");
}

/**
 * @brief 解析内嵌 TOML 并启动组网（在 app_main / main 任务栈上执行）。
 * @return 成功构造并启动返回 true
 */
bool start_easytier() {
#if !defined(EASYTIER_EXAMPLE_TOML_EMBEDDED)
    // main/easytier.toml 不在版本库里（含真实地址与密钥），此构建没有内嵌配置：
    // 只保持 WiFi 连接，不启动组网，并明确提示如何补上配置。
    ESP_LOGW(kTag,
             "main/easytier.toml not found; create it and rebuild "
             "(TOML example: README.md)");
    return false;
#else
    size_t len = static_cast<size_t>(easytier_toml_end - easytier_toml_start);
    // EMBED_TXTFILES 会把 NUL 计入 end-start；它是字符串终止符，不是 TOML 内容。
    if (len != 0 && easytier_toml_start[len - 1] == 0) {
        --len;
    }
    const std::string_view toml(reinterpret_cast<const char*>(easytier_toml_start), len);

    et::RuntimeOptions runtime_options{};
    runtime_options.on_event = &on_easytier_event;
    // 构造即初始化：TOML 解析、身份补齐、结构校验与资源分配都在构造函数里完成，
    // 析构时自动停止并释放；构造失败的具体原因由组件以 Error 日志输出。
    g_inst.emplace(toml, runtime_options);
    if (!g_inst->valid()) {
        ESP_LOGE(kTag, "easytier config rejected: 0x%x", g_inst->last_error());
        g_inst.reset();
        return false;
    }

    log_effective_config();

    const esp_err_t start_err = g_inst->start();
    if (start_err != ESP_OK) {
        ESP_LOGE(kTag, "easytier start failed: 0x%x", static_cast<unsigned>(start_err));
        g_inst.reset();
        return false;
    }
    g_started = true;

    // TOML 解析与 Inst 构造都在 main 任务栈上完成；这里打印余量便于核对栈预算。
    ESP_LOGI(kTag, "main stack high water: %u bytes",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return true;
#endif  // EASYTIER_EXAMPLE_TOML_EMBEDDED
}

/** @brief 允许组件连接（IP 已取得）。 */
void connect_easytier() {
    if (!g_started || !g_inst.has_value()) {
        return;
    }
    const esp_err_t err = g_inst->connect();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "easytier connect failed: 0x%x", static_cast<unsigned>(err));
    }
}

/** @brief 让组件断开（WiFi 已断开）。 */
void disconnect_easytier() {
    if (!g_started || !g_inst.has_value()) {
        return;
    }
    const esp_err_t err = g_inst->disconnect();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "easytier disconnect failed: 0x%x", static_cast<unsigned>(err));
    }
}

#endif  // CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED

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

    // WiFi 事件回调需要它来唤醒 main 主循环。
    g_events = xEventGroupCreate();
    if (g_events == nullptr) {
        ESP_LOGE(kTag, "cannot create event group");
        return;
    }

    init_wifi();

#if CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED
    if (!start_easytier()) {
        // 配置缺失/非法：保留 WiFi + HTTP；WiFi 事件任务会继续自动重连。
        return;
    }

    // 首次可能已经拿到 IP；没有的话在主循环里等事件位。
    (void)xEventGroupClearBits(g_events, kUnderlayChangedBit);
    if (g_wifi_has_ip.load()) {
        connect_easytier();
    }
    log_status();

    for (;;) {
        const EventBits_t bits = xEventGroupWaitBits(
            g_events, kUnderlayChangedBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
        if (bits & kUnderlayChangedBit) {
            if (g_wifi_has_ip.load()) {
                connect_easytier();
            } else {
                disconnect_easytier();
            }
        }
        log_status();
    }
#else
    // 只连 WiFi + HTTP；组网功能关闭。
#endif
}
