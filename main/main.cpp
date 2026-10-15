/**
 * @file main.cpp
 * @brief EasyTier-ESP 示例：连接 WiFi 并加入 EasyTier 虚拟网络。
 *
 * @details 示例刻意保持简单，只做应用侧必须负责的事：
 * 1. 初始化 NVS、esp-netif 与默认事件循环；
 * 2. 以 STA 模式连接 WiFi，并在断开时自动重连；
 * 3. 用嵌入的 easytier.toml 初始化 et::Instance；
 * 4. 取得 IP 后启动组网并通知 underlay 就绪，失去 IP 时通知断开；
 * 5. 打印组件上报的组网事件。
 *
 * 协议、加密、RPC、路由与虚拟网卡全部由 components/easytier-esp 负责，
 * 本文件不含任何协议实现。
 */
#include <cstdio>
#include <cstring>

#include "easytier-esp.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {

constexpr const char* kTag = "et_example";

/** @brief 单实例（示例只运行一个 EasyTier 节点）。 */
et::Instance g_instance;
/** @brief 配置诊断报告：约 4.5 KiB，放在静态区避免占用 app_main 栈。 */
et::ConfigReport g_report;
/** @brief 组网是否已启动（在事件回调中判断，避免重复 start）。 */
bool g_started = false;
/** @brief WiFi 重连计数。 */
int g_retry = 0;

/** @brief CMake EMBED_TXTFILES 生成的符号：嵌入的 TOML 配置。 */
extern const uint8_t easytier_toml_start[] asm("_binary_easytier_toml_start");
extern const uint8_t easytier_toml_end[] asm("_binary_easytier_toml_end");

/** @brief 组件事件回调：只做打印，保持快速返回。 */
void on_easytier_event(const et::Event& event, void*) noexcept {
    switch (event.type) {
        case et::EventType::Started:
            ESP_LOGI(kTag, "easytier started, peer_id=%u", static_cast<unsigned>(event.status.peer_id));
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

/** @brief 配置诊断快照：约 2.3 KiB，放静态区避免占用栈。 */
et::ConfigInfo g_config_info;

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

/**
 * @brief 打印当前组网状态。
 * @details 联调时最重要的观测手段：把状态、虚拟地址、直连数、可达路由与丢弃计数
 *          一行打出来，便于和入口节点的日志对照。
 */
void log_status() {
    et::Status status{};
    if (g_instance.get_status(status) != ESP_OK) {
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

/** @brief 打印实际生效的配置（TOML 的映射结果）。 */
void log_effective_config() {
    if (g_instance.get_config_info(g_config_info) != ESP_OK) {
        return;
    }
    ESP_LOGI(kTag, "config: instance=%s hostname=%s network=%s instance_id=%s",
             g_config_info.instance_name.data(), g_config_info.hostname.data(),
             g_config_info.network_name.data(), g_config_info.instance_id.data());
    for (size_t i = 0; i < g_config_info.peer_count; ++i) {
        ESP_LOGI(kTag, "config: peer[%u]=%s", static_cast<unsigned>(i),
                 g_config_info.peers[i].text.data());
    }
    if (g_config_info.has_static_ipv4) {
        ESP_LOGI(kTag, "config: ipv4=%u.%u.%u.%u/%u mtu=%u",
                 static_cast<unsigned>((g_config_info.static_ipv4 >> 24) & 0xFF),
                 static_cast<unsigned>((g_config_info.static_ipv4 >> 16) & 0xFF),
                 static_cast<unsigned>((g_config_info.static_ipv4 >> 8) & 0xFF),
                 static_cast<unsigned>(g_config_info.static_ipv4 & 0xFF),
                 static_cast<unsigned>(g_config_info.static_prefix),
                 static_cast<unsigned>(g_config_info.mtu));
    } else if (g_config_info.dhcp) {
        ESP_LOGI(kTag, "config: dhcp=%s mtu=%u", g_config_info.dhcp_subnet.data(),
                 static_cast<unsigned>(g_config_info.mtu));
    } else {
        ESP_LOGW(kTag, "config: 既没有静态 ipv4 也没有启用 dhcp，虚拟网卡不会创建");
    }
    // 加密开关直接影响能否与对端互通，单独打印一行便于上板核对。
    ESP_LOGI(kTag, "config: encryption=%s",
             g_config_info.enable_encryption ? "aes-gcm" : "disabled");
}

/** @brief 周期状态打印（10 秒一次，不占用任务栈）。 */
void status_timer_cb(void*) { log_status(); }

/// 组网启动任务的栈大小：解析 TOML、派生密钥都需要调用方提供足够栈。
constexpr uint32_t kStartTaskStack = 8192;

/** @brief 启动组网（定义见下方）。 */
void start_easytier();

/**
 * @brief 一次性启动任务。
 * @details WiFi 事件回调运行在系统事件任务上（栈只有几 KiB），而组网初始化要解析
 *          TOML、派生密钥并创建任务，因此必须放到独立任务里执行；事件回调本身只
 *          负责创建这个任务并立即返回。
 */
void start_task(void*) {
    start_easytier();
    if (g_started) {
        // 通知组件物理网络就绪：此后组件才会连接入口节点。
        (void)g_instance.set_underlay_ready(true);
    }
    vTaskDelete(nullptr);
}

/** @brief 启动组网（只在取得 IP 后调用一次）。 */
void start_easytier() {
    if (g_started) {
        return;
    }
    auto len = static_cast<size_t>(easytier_toml_end - easytier_toml_start);
    // EMBED_TXTFILES 会把 NUL 计入 end-start；它是字符串终止符，不是 TOML 内容。
    if (len != 0 && easytier_toml_start[len - 1] == 0) --len;
    et::RuntimeOptions runtime_options{};
    runtime_options.on_event = &on_easytier_event;
    const esp_err_t init_err = g_instance.init_from_toml(
        std::string_view(reinterpret_cast<const char*>(easytier_toml_start), len),
        runtime_options, &g_report);
    if (init_err != ESP_OK) {
        ESP_LOGE(kTag, "easytier config rejected: 0x%x", init_err);
        for (size_t i = 0; i < g_report.count; ++i) {
            const et::ConfigDiagnostic& d = g_report.items[i];
            ESP_LOGE(kTag, "config issue %d key=%s line=%u column=%u",
                     static_cast<int>(d.issue), d.key.data(), static_cast<unsigned>(d.line),
                     static_cast<unsigned>(d.column));
        }
        return;
    }
    // 被忽略但不影响使用的配置项以警告形式列出，便于排查"配置没生效"。
    for (size_t i = 0; i < g_report.count; ++i) {
        const et::ConfigDiagnostic& d = g_report.items[i];
        ESP_LOGW(kTag, "ignored config: key=%s", d.key.data());
    }

    log_effective_config();

    const esp_err_t start_err = g_instance.start();
    if (start_err != ESP_OK) {
        ESP_LOGE(kTag, "easytier start failed: 0x%x", start_err);
        return;
    }
    g_started = true;

    // 周期打印状态：第一次立即打印，之后每 10 秒一次（联调主要靠这行日志）。
    static esp_timer_handle_t status_timer = nullptr;
    if (status_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &status_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "et_status",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &status_timer) == ESP_OK) {
            (void)esp_timer_start_periodic(status_timer, 10 * 1000 * 1000);
        }
    }
    log_status();
}

/** @brief WiFi 与 IP 事件处理。 */
void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // 物理网络断开：先通知组件撤销连接与路由，再重连 WiFi。
        if (g_started) {
            (void)g_instance.set_underlay_ready(false);
        }
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
#if CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED
        // 事件回调必须快速返回：首次初始化交给独立任务（见 start_task）。
        if (!g_started) {
            (void)xTaskCreate(start_task, "et_start", kStartTaskStack, nullptr, 5, nullptr);
        } else {
            // WiFi 掉线重连后**必须重新**通知组件物理网络就绪，否则组件会一直停在
            // underlay_ready=false，永远不再连接入口（曾经的真实缺陷）。
            (void)g_instance.set_underlay_ready(true);
        }
#endif
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

/** @brief 初始化 WiFi STA 并开始连接。 */
void init_wifi() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr));

    wifi_config_t sta_config{};
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
#if CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED
    ESP_LOGI(kTag, "starting EasyTier-ESP example");
#endif
    init_wifi();
    // 之后由 WiFi/IP 事件驱动：取得 IP → 启动组网并通知 underlay 就绪。
}
