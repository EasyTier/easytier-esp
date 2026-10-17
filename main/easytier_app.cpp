/**
 * @file easytier_app.cpp
 * @brief EasyTier 组网的启动任务、事件回调与周期状态打印，见 @ref easytier_app.hpp 。
 */
#include "easytier_app.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "easytier-esp.h"

#if CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED

namespace easytier_app {
namespace {

constexpr const char* kTag = "et_easytier";

/// 组网是否已启动：区分"首次取得 IP，需要建任务"与"重连后重新取得 IP，只需 connect()"。
bool g_started = false;

/**
 * @brief 运行期实例。
 * @note 组件本身支持多个 `et::Inst` 并存；示例只用一个，因此放在静态区让它的生命周期
 *       覆盖整个应用（析构时自动停止并释放资源）。
 */
std::optional<et::Inst> g_inst;

/**
 * @brief CMake EMBED_TXTFILES 生成的符号：嵌入的 TOML 配置。
 * @note main/easytier.toml 含真实入口地址与网络密钥，已被 .gitignore 忽略，因此新克隆
 *       的仓库里不存在，CMake 也不会嵌入（见 main/CMakeLists.txt）。
 */
#if defined(EASYTIER_EXAMPLE_TOML_EMBEDDED)
extern const uint8_t easytier_toml_start[] asm("_binary_easytier_toml_start");
extern const uint8_t easytier_toml_end[] asm("_binary_easytier_toml_end");
#endif

/**
 * @brief 组件事件回调：只做打印，保持快速返回。
 * @note 回调在控制面任务（`et_proto`）上下文中同步执行，内部只允许调用
 *       get_status/get_config_info/connect/disconnect 这类非阻塞接口。
 */
void on_easytier_event(const et::Event& event, void*) noexcept {
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
void format_uuid(const std::array<uint8_t, 16>& id, std::array<char, 37>& out) {
    (void)snprintf(out.data(), out.size(),
                   "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                   id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10],
                   id[11], id[12], id[13], id[14], id[15]);
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

/**
 * @brief 打印当前组网状态。
 * @details 联调时最重要的观测手段：把状态、虚拟地址、直连数、可达路由与丢弃计数
 *          一行打出来，便于和入口节点的日志对照。
 */
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
        (void)std::snprintf(vip, sizeof(vip), "%u.%u.%u.%u/%u",
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

/** @brief 打印实际生效的配置（TOML 的映射结果 + 身份补齐）。 */
void log_effective_config() {
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
    // 加密开关直接影响能否与对端互通，单独打印一行便于上板核对。
    ESP_LOGI(kTag, "config: encryption=%s", cfg.enable_encryption ? "aes-gcm" : "disabled");
}

/**
 * @brief 周期状态打印（10 秒一次，不占用任务栈）。
 * @note 只有内嵌配置（EASYTIER_EXAMPLE_TOML_EMBEDDED）的构建才会注册它。
 */
[[maybe_unused]] void status_timer_cb(void*) { log_status(); }

/// 组网启动任务的栈大小：解析 TOML（递归下降）与构造 Inst 都在本任务栈上完成。
constexpr uint32_t kStartTaskStack = 8192;
/// 启动任务优先级：与组件的控制面任务同级，低于 WiFi/TCPIP。
constexpr UBaseType_t kStartTaskPriority = 5;

/** @brief 启动组网（只在取得 IP 后调用一次）。 */
void start_easytier() {
    if (g_started) {
        return;
    }
#if !defined(EASYTIER_EXAMPLE_TOML_EMBEDDED)
    // main/easytier.toml 不在版本库里（含真实地址与密钥），此构建没有内嵌配置：
    // 只保持 WiFi 连接，不启动组网，并明确提示如何补上配置。
    ESP_LOGW(kTag,
             "main/easytier.toml not found; create it and rebuild "
             "(TOML format: components/easytier-esp/README.md)");
    return;
#else
    auto len = static_cast<size_t>(easytier_toml_end - easytier_toml_start);
    // EMBED_TXTFILES 会把 NUL 计入 end-start；它是字符串终止符，不是 TOML 内容。
    if (len != 0 && easytier_toml_start[len - 1] == 0) --len;
    const std::string_view toml(reinterpret_cast<const char*>(easytier_toml_start), len);

    et::RuntimeOptions runtime_options{};
    runtime_options.on_event = &on_easytier_event;
    // 构造即初始化：TOML 解析、身份补齐、结构校验与资源分配都在构造函数里完成，
    // 析构时自动停止并释放；构造失败的具体原因由组件以 Error 日志输出。
    g_inst.emplace(toml, runtime_options);
    if (!g_inst->valid()) {
        ESP_LOGE(kTag, "easytier config rejected: 0x%x", g_inst->last_error());
        g_inst.reset();
        return;
    }

    log_effective_config();

    const esp_err_t start_err = g_inst->start();
    if (start_err != ESP_OK) {
        ESP_LOGE(kTag, "easytier start failed: 0x%x", start_err);
        g_inst.reset();
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
#endif  // EASYTIER_EXAMPLE_TOML_EMBEDDED
}

/**
 * @brief 一次性启动任务：跑完初始化后自删除。
 * @details 事件回调只负责创建它并立即返回；重活（TOML 解析、密钥派生、创建 3 个组件
 *          任务）都在本任务的 8 KiB 栈上完成，见 @ref easytier_app.hpp 的说明。
 */
void start_task(void*) {
    start_easytier();
    if (g_started) {
        // 通知组件物理网络已可用：此后组件才会连接入口节点。
        (void)g_inst->connect();
    }
    vTaskDelete(nullptr);
}

}  // namespace

void on_ip_got() noexcept {
    if (!g_started) {
        if (xTaskCreate(start_task, "et_start", kStartTaskStack, nullptr, kStartTaskPriority,
                        nullptr) != pdPASS) {
            // 建任务失败时保持 g_started=false，下一个 GOT_IP 事件会自动重试。
            ESP_LOGE(kTag, "cannot create start task (stack %u bytes)",
                     static_cast<unsigned>(kStartTaskStack));
        }
        return;
    }
    // WiFi 掉线重连后**必须重新** connect()，否则组件会一直停在
    // WaitingUnderlay，永远不再连接入口（曾经的真实缺陷）。
    if (g_inst.has_value()) {
        (void)g_inst->connect();
    }
}

void on_ip_lost() noexcept {
    if (g_started && g_inst.has_value()) {
        (void)g_inst->disconnect();
    }
}

}  // namespace easytier_app

#else  // !CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED

namespace easytier_app {

// 关闭「启动时加载内嵌的 easytier.toml 并组网」：示例只连 WiFi，便于先验证网络连通性。
void on_ip_got() noexcept {}
void on_ip_lost() noexcept {}
}  // namespace easytier_app

#endif  // CONFIG_EXAMPLE_EASYTIER_TOML_ENABLED
