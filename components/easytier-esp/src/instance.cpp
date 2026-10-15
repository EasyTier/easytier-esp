/**
 * @file instance.cpp
 * @brief et::Instance 公开 C++ API 的实现。
 *
 * @details 生命周期：
 * @code
 * init*()   → 解析/校验配置、复制字符串、派生密钥（不创建任务）
 * start()   → 创建队列与任务，状态置 WaitingUnderlay
 * stop()    → 置停止标志、等待任务确认退出（可超时）
 * deinit()  → 释放队列与密钥
 * @endcode
 * 首版只允许一个已初始化实例，避免两个实例同时占用虚拟网卡与路由归属。
 */
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <new>

#include "context.hpp"
#include "config/file.hpp"
#include "config/toml.hpp"
#include "crypto/kdf.hpp"
#include "support/config_limits.hpp"
#include "support/log.hpp"
#include "support/random.hpp"
#include "support/time.hpp"
#include "transport/tcp.hpp"

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace et {
namespace {

constexpr const char* kTag = "ET.INSTANCE";

/** @brief 队列深度（帧队列容量有限，形成背压）。 */
constexpr UBaseType_t kFrameQueueDepth = 4;
// 单个实例的常驻内存必须保持有界：Context 在堆上分配（大于 16 KiB 的分配会走
// PSRAM），但仍要有上限，避免后续加入功能时悄悄膨胀。实测组成（ESP32-S3，字节）：
//   Context 合计 93768
//     ├─ RpcReassembler（2 槽 × (8 KiB 收包 + 8 KiB 有序缓冲)）33184
//     ├─ SyncEngine（Topology 22928 + 2 个 ConnList 8968 + 快照 656 + 编码缓冲）约 33000
//     ├─ RPC 收发缓冲（body 8 KiB + service 4 KiB + 其它）约 14000
//     └─ 其它（AEAD 指针、原子量、dhcp/网卡状态等）
// 入口地址列表不再占用 Context 常驻内存：它按配置实际条数单独在堆上分配，因此配置里
// 写多少入口都不会让 Context 膨胀。
// 另外每个实例还有：帧队列 4×2008×3 ≈ 24 KiB、每个连接槽一个 TcpConn 约 12 KiB（堆，
// 数量见 Kconfig `EASYTIER_ESP_MAX_CONNECTED_PEERS`，默认 6）、
// 三个任务栈 6+10+8 = 24 KiB。因此**强烈建议启用 PSRAM**（示例 sdkconfig 已启用，
// 大于 16 KiB 的分配会落到 PSRAM）。新增缓冲时请同步更新本注释与 README 的内存表。
static_assert(sizeof(Context) < 112 * 1024, "Context 常驻内存超出预算，请检查新增缓冲");

/** @brief 事件/命令队列深度。 */
constexpr UBaseType_t kSmallQueueDepth = 4;
/** @brief TCPIP 线程 → 数据面的 pbuf 队列深度。 */
constexpr UBaseType_t kNetifQueueDepth = 8;
/** @brief I/O 任务栈（字节）。 */
constexpr uint32_t kIoTaskStack = 6144;
/** @brief 控制面任务栈（字节）。 */
constexpr uint32_t kProtoTaskStack = 10240;
/** @brief I/O 任务优先级（低于 WiFi 与 TCPIP）。 */
constexpr UBaseType_t kIoTaskPriority = 7;
/** @brief 控制面任务优先级。 */
constexpr UBaseType_t kProtoTaskPriority = 5;
/** @brief 数据面任务栈（字节）。 */
constexpr uint32_t kDataTaskStack = 8192;
/** @brief 数据面任务优先级（与 I/O 任务同级，低于 WiFi/TCPIP）。 */
constexpr UBaseType_t kDataTaskPriority = 7;

/** @brief 全局单实例标记。 */
std::atomic<bool> g_instance_in_use{false};

/** @brief 把校验后的配置（含入口列表所有权）与运行选项移入上下文。 */
void prepare_context(Context& ctx, StoredConfig&& config,
                     const RuntimeOptions& options) noexcept {
    ctx.config = std::move(config);
    finalize_config_identity(ctx.config);
    ctx.options = options;
    ctx.key = derive_key_128(ctx.config.network_secret_view());
    ctx.digest = secret_digest(ctx.config.network_name_view(), ctx.config.network_secret_view());
    uint32_t peer_id = random_u32();
    ctx.my_peer_id.store(peer_id == 0 ? 1u : peer_id);
}

#if defined(ESP_PLATFORM)
/** @brief 创建队列与任务；失败时回滚已创建的资源。 */
esp_err_t start_runtime(Context& ctx) noexcept {
    if (ctx.q_rx_ctrl == nullptr) {
        ctx.q_rx_ctrl = xQueueCreate(kFrameQueueDepth, sizeof(FrameMessage));
        ctx.q_tx = xQueueCreate(kFrameQueueDepth, sizeof(FrameMessage));
        ctx.q_evt = xQueueCreate(kSmallQueueDepth, sizeof(SmallMessage));
        ctx.q_io_cmd = xQueueCreate(kSmallQueueDepth, sizeof(SmallMessage));
        ctx.q_app_cmd = xQueueCreate(kSmallQueueDepth, sizeof(SmallMessage));
        ctx.q_rx_data = xQueueCreate(kFrameQueueDepth, sizeof(FrameMessage));
        ctx.q_netif_rx = xQueueCreate(kNetifQueueDepth, sizeof(NetifRxItem));
        ctx.q_snapshot = xQueueCreate(1, sizeof(RouteSnapshot));
        ctx.task_exit = xSemaphoreCreateCounting(kTaskCount, 0);
        if (ctx.q_rx_ctrl == nullptr || ctx.q_tx == nullptr || ctx.q_evt == nullptr ||
            ctx.q_io_cmd == nullptr || ctx.q_app_cmd == nullptr || ctx.q_rx_data == nullptr ||
            ctx.q_netif_rx == nullptr || ctx.q_snapshot == nullptr || ctx.task_exit == nullptr) {
            ET_LOGE(kTag, "queue/semaphore allocation failed (need ~24 KiB of heap)");
            return ESP_ERR_NO_MEM;
        }
    }

    ctx.stop_requested.store(false);
    ctx.underlay_ready.store(false);
    ctx.dropped.store(0);
    ctx.last_error.store(ESP_OK);
    ctx.state.store(State::WaitingUnderlay);

    // 控制面与数据面各持一个 AEAD 实例，禁止跨任务共享。关闭加密时按上游
    // NullCipher 语义不创建任何 AEAD，因此这些报文不会带 AEAD 尾或加密位。
    if (ctx.config.enable_encryption && ctx.control_cipher == nullptr) {
        ctx.control_cipher = new (std::nothrow) AesGcm128(ctx.key);
        if (ctx.control_cipher == nullptr || !ctx.control_cipher->ready()) {
            delete ctx.control_cipher;
            ctx.control_cipher = nullptr;
            ET_LOGE(kTag, "control-plane AEAD init failed (PSA/mbedTLS)");
            return ESP_ERR_NO_MEM;
        }
    }
    if (ctx.config.enable_encryption && ctx.data_cipher == nullptr) {
        ctx.data_cipher = new (std::nothrow) AesGcm128(ctx.key);
        if (ctx.data_cipher == nullptr || !ctx.data_cipher->ready()) {
            delete ctx.data_cipher;
            ctx.data_cipher = nullptr;
            ET_LOGE(kTag, "data-plane AEAD init failed (PSA/mbedTLS)");
            return ESP_ERR_NO_MEM;
        }
    }
    // 每个连接槽一个 TcpConn（各含约 12 KiB 收发缓冲）。必须显式分配：对象较大，
    // 不能放在任务栈上；缺少分配时 I/O 任务会直接报错退出（连不上任何入口）。
    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        if (ctx.slots[i].conn == nullptr) {
            ctx.slots[i].conn = new (std::nothrow) TcpConn();
            if (ctx.slots[i].conn == nullptr) {
                ET_LOGE(kTag, "TcpConn slot %u allocation failed (needs ~12 KiB each)",
                        static_cast<unsigned>(i));
                return ESP_ERR_NO_MEM;
            }
        }
    }

    // 路由同步引擎：拓扑、最短路、自身信息与 dhcp 自分配都在其中。
    ctx.sync.configure(ctx.config, ctx.my_peer_id.load());
    ctx.my_ipv4.store(ctx.config.has_static_ipv4 ? ctx.config.static_ipv4 : 0);
    ctx.my_prefix.store(ctx.config.has_static_ipv4 ? ctx.config.static_prefix : 24);
    ctx.reachable_routes.store(0);
    // 打印常驻内存规模与并发连接槽数，便于真机 bring-up 时核对 Kconfig 是否生效。
    ET_LOGI(kTag, "runtime context: %u bytes; slots: %u; frame queues: ctrl=%u data=%u tx=%u",
            static_cast<unsigned>(sizeof(Context)), static_cast<unsigned>(kMaxConnSlots),
            static_cast<unsigned>(kFrameQueueDepth), static_cast<unsigned>(kFrameQueueDepth),
            static_cast<unsigned>(kFrameQueueDepth));
    // 静态地址时立即可用；dhcp 地址在控制面算出后再创建网卡。
    // 与物理网段重叠的虚拟网段会破坏正常路由，这里显式拒绝（见 proto_task 的同名检查）。
    if (ctx.config.has_static_ipv4) {
        const esp_err_t netif_err = ctx.virtual_netif.create(
            ctx.config.static_ipv4, ctx.config.static_prefix, ctx.config.mtu,
            static_cast<void*>(ctx.q_netif_rx), &ctx.generation);
        ctx.netif_created = netif_err == ESP_OK;
        ctx.netif_ip = ctx.config.static_ipv4;
        if (netif_err != ESP_OK) {
            ctx.set_error(netif_err);
        }
    }

    if (xTaskCreate(et_io_task_entry, "et_io", kIoTaskStack, &ctx, kIoTaskPriority,
                    &ctx.io_task_handle) != pdPASS) {
        ET_LOGE(kTag, "cannot create et_io task (stack %u bytes)", kIoTaskStack);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(et_proto_task_entry, "et_proto", kProtoTaskStack, &ctx, kProtoTaskPriority,
                    &ctx.proto_task_handle) != pdPASS) {
        ET_LOGE(kTag, "cannot create et_proto task (stack %u bytes)", kProtoTaskStack);
        ctx.stop_requested.store(true);
        (void)xSemaphoreTake(ctx.task_exit, pdMS_TO_TICKS(1000));
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(et_data_task_entry, "et_data", kDataTaskStack, &ctx, kDataTaskPriority,
                    &ctx.data_task_handle) != pdPASS) {
        ET_LOGE(kTag, "cannot create et_data task (stack %u bytes)", kDataTaskStack);
        ctx.stop_requested.store(true);
        for (int i = 0; i < 2; ++i) {
            (void)xSemaphoreTake(ctx.task_exit, pdMS_TO_TICKS(1000));
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** @brief 等待两个任务确认退出。 */
bool wait_tasks(Context& ctx, uint32_t timeout_ms) noexcept {
    if (timeout_ms == UINT32_MAX) {
        // 析构路径：无限等待，绝不释放仍被任务访问的资源。
        for (size_t i = 0; i < kTaskCount; ++i) {
            if (xSemaphoreTake(ctx.task_exit, portMAX_DELAY) != pdTRUE) {
                return false;
            }
        }
        return true;
    }
    const TickType_t total = pdMS_TO_TICKS(timeout_ms);
    const TickType_t start = xTaskGetTickCount();
    for (size_t i = 0; i < kTaskCount; ++i) {
        const TickType_t used = xTaskGetTickCount() - start;
        const TickType_t remain = (used >= total) ? 0 : (total - used);
        if (xSemaphoreTake(ctx.task_exit, remain) != pdTRUE) {
            return false;
        }
    }
    return true;
}

/** @brief 删除队列与信号量。 */
/**
 * @brief 释放运行期资源。
 * @details **顺序很重要**：必须先注销虚拟网卡再删除队列。网卡的 output 回调会把
 *          pbuf 入队到 `q_netif_rx`，而 netifapi_* 是同步的（返回时 TCPIP 线程已处理
 *          完该请求），所以先注销网卡能保证之后不会再有人往队列里写；反过来先删队列
 *          则存在"回调在队列已释放后 xQueueSend"的 use-after-free 窗口。
 */
void destroy_queues(Context& ctx) noexcept {
    ctx.virtual_netif.destroy();
    ctx.netif_created = false;
    if (ctx.q_rx_ctrl != nullptr) {
        vQueueDelete(ctx.q_rx_ctrl);
        ctx.q_rx_ctrl = nullptr;
    }
    if (ctx.q_tx != nullptr) {
        vQueueDelete(ctx.q_tx);
        ctx.q_tx = nullptr;
    }
    if (ctx.q_evt != nullptr) {
        vQueueDelete(ctx.q_evt);
        ctx.q_evt = nullptr;
    }
    if (ctx.q_io_cmd != nullptr) {
        vQueueDelete(ctx.q_io_cmd);
        ctx.q_io_cmd = nullptr;
    }
    if (ctx.q_app_cmd != nullptr) {
        vQueueDelete(ctx.q_app_cmd);
        ctx.q_app_cmd = nullptr;
    }
    if (ctx.q_rx_data != nullptr) {
        vQueueDelete(ctx.q_rx_data);
        ctx.q_rx_data = nullptr;
    }
    if (ctx.q_netif_rx != nullptr) {
        vQueueDelete(ctx.q_netif_rx);
        ctx.q_netif_rx = nullptr;
    }
    if (ctx.q_snapshot != nullptr) {
        vQueueDelete(ctx.q_snapshot);
        ctx.q_snapshot = nullptr;
    }
    if (ctx.task_exit != nullptr) {
        vSemaphoreDelete(ctx.task_exit);
        ctx.task_exit = nullptr;
    }
    delete ctx.control_cipher;
    ctx.control_cipher = nullptr;
    delete ctx.data_cipher;
    ctx.data_cipher = nullptr;
    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        delete ctx.slots[i].conn;
        ctx.slots[i].conn = nullptr;
        ctx.slots[i].alive.store(false);
        ctx.slots[i].peer_id.store(0);
        ctx.slots[i].generation.store(0);
    }
}
#endif  // ESP_PLATFORM

}  // namespace

/** @brief 私有实现：上下文 + 运行标记。 */
struct Instance::Impl {
    Context ctx{};            ///< 运行上下文
    bool started = false;     ///< 是否已 start()
    bool resources = false;   ///< 队列是否已创建
};

Instance::Instance() noexcept = default;

Instance::~Instance() noexcept {
    if (impl_ == nullptr) {
        return;
    }
#if defined(ESP_PLATFORM)
    if (impl_->started) {
        // 析构没有超时：必须等到任务确认退出后才能释放内存。
        (void)stop(UINT32_MAX);
    }
#endif
    (void)deinit();
}

/**
 * @brief 公共初始化路径。
 * @details 依次：单实例检查 → 运行选项检查 → 上下文准备 → 分配 Impl。
 */
esp_err_t Instance::init(const Config& config, const RuntimeOptions& options) noexcept {
    // StoredConfig（约 3.5 KiB）与 ConfigReport（约 4.5 KiB）放在静态区：
    // 公共 init 路径可能被 2~4 KiB 栈的事件任务调用（例如 WiFi 拿到 IP 的回调），
    // 把这两个对象放在栈上会直接压垮调用方。整块缓冲复用是安全的——组件本身是
    // 单实例设计（见 g_instance_in_use），且初始化只发生一次。
    static StoredConfig stored;
    static ConfigReport report;
    // stored_config_from_config() 内部会先复位为默认值，这里无需重复复位。
    const esp_err_t err = stored_config_from_config(config, stored, &report);
    if (err != ESP_OK) {
        return err;
    }
    return init_from_internal(&stored, options);
}

esp_err_t Instance::init_from_toml(std::string_view text, const RuntimeOptions& options,
                                  ConfigReport* report) noexcept {
    // 同上：解析缓冲放在静态区，保证调用方无论栈多大都安全。
    // parse_toml_config() 内部会先复位为默认值，这里无需重复复位。
    static StoredConfig stored;
    const esp_err_t err = parse_toml_config(text, stored, report);
    if (err != ESP_OK) {
        return err;
    }
    return init_from_internal(&stored, options);
}

esp_err_t Instance::init_from_file(std::string_view path, const RuntimeOptions& options,
                                   ConfigReport* report) noexcept {
    clear_config_report(report);
    if (impl_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ESP_PLATFORM)
    // 配置文件最大 16 KiB，按上限读取到栈外的静态缓冲，避免占用调用方栈。
    static std::array<char, kTomlMaxBytes> file_buffer;
    size_t len = 0;
    const esp_err_t io_err = read_config_file(path, file_buffer, len);
    if (io_err != ESP_OK) {
        if (report != nullptr && report->count < kMaxDiagnostics) {
            ConfigDiagnostic& d = report->items[report->count++];
            d.issue = ConfigIssue::IoError;
            (void)set_fixed_string(d.key, "config_file");
        }
        return io_err;
    }
    return init_from_toml(std::string_view(file_buffer.data(), len), options, report);
#else
    (void)path;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/** @brief 由校验后的配置完成初始化（三个 init* 入口共用）。 */
esp_err_t Instance::init_from_internal(void* stored_config,
                                       const RuntimeOptions& options) noexcept {
    if (stored_config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // 入口列表的所有权在这里移交给 Context，因此 stored 是可变引用。
    auto& stored = *static_cast<StoredConfig*>(stored_config);
    if (impl_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!g_instance_in_use.compare_exchange_strong(expected, true)) {
        ET_LOGE(kTag, "another instance is already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    // Impl 内含 Context（当前约 99 KiB：RPC 重组缓冲、拓扑、邻接表、快照等）。
    // 未启用 PSRAM 时可能分配失败，这里给出明确提示而不是只返回 0x101。
    Impl* impl = new (std::nothrow) Impl();
    if (impl == nullptr) {
        // 最可能的原因：未启用 PSRAM 时内部 RAM 不足以容纳约 99 KiB 的 Context。
        ET_LOGE(kTag, "context allocation failed (%u bytes); enable PSRAM or shrink buffers",
                static_cast<unsigned>(sizeof(Impl)));
        g_instance_in_use.store(false);
        return ESP_ERR_NO_MEM;
    }
    prepare_context(impl->ctx, std::move(stored), options);
    impl->ctx.state.store(State::Stopped);
    impl_ = impl;
    return ESP_OK;
}

esp_err_t Instance::start() noexcept {
    if (impl_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ESP_PLATFORM)
    if (impl_->started) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = start_runtime(impl_->ctx);
    if (err != ESP_OK) {
        destroy_queues(impl_->ctx);
        impl_->resources = false;
        impl_->ctx.set_error(err);
        impl_->ctx.state.store(State::Error);
        return err;
    }
    impl_->resources = true;
    impl_->started = true;
    // Started 事件由控制面任务在进入主循环时上报，保证回调上下文一致。
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t Instance::set_underlay_ready(bool ready) noexcept {
    if (impl_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ESP_PLATFORM)
    if (!impl_->started) {
        return ESP_ERR_INVALID_STATE;
    }
    SmallMessage msg{};
    msg.kind = static_cast<uint8_t>(ready ? AppCommand::UnderlayUp : AppCommand::UnderlayDown);
    // 非阻塞：队列满时返回错误，由调用方重试，绝不静默丢弃物理网络状态。
    if (xQueueSend(impl_->ctx.q_app_cmd, &msg, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    (void)ready;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t Instance::get_status(Status& out) const noexcept {
    if (impl_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const Context& ctx = impl_->ctx;
    out.state = ctx.state.load();
    out.peer_id = ctx.my_peer_id.load();
    out.virtual_ipv4 = ctx.my_ipv4.load();
    out.prefix_len = ctx.my_prefix.load();
    out.direct_peer_count = ctx.direct_peers.load();
    out.reachable_route_count = ctx.reachable_routes.load();
    out.last_error = ctx.last_error.load();
    out.dropped_packets = ctx.dropped.load();
    return ESP_OK;
}

/**
 * @brief 取回当前生效配置的诊断快照。
 * @details 数据来源是组件内部已校验的 StoredConfig（即真正生效的配置），
 *          因此可以据此确认 TOML 的映射结果。
 */
esp_err_t Instance::get_config_info(ConfigInfo& out) const noexcept {
    if (impl_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const StoredConfig& cfg = impl_->ctx.config;
    out = ConfigInfo{};
    std::memcpy(out.instance_name.data(), cfg.instance_name.data(),
                std::min(out.instance_name.size() - 1, cfg.instance_name.size()));
    std::memcpy(out.hostname.data(), cfg.hostname.data(),
                std::min(out.hostname.size() - 1, cfg.hostname.size()));
    std::memcpy(out.network_name.data(), cfg.network_name.data(),
                std::min(out.network_name.size() - 1, cfg.network_name.size()));
    format_uuid_text(cfg.instance_id.data(), out.instance_id.data());
    out.peer_total_count = cfg.peers.size();
    out.peer_count = cfg.peers.size() > out.peers.size() ? out.peers.size() : cfg.peers.size();
    out.peers_truncated = cfg.peers.size() > out.peer_count;
    for (size_t i = 0; i < out.peer_count; ++i) {
        // 主机名最长 255 字节而缓冲 200：用精度限长，避免编译期截断告警。
        (void)snprintf(out.peers[i].text.data(), out.peers[i].text.size(), "%.180s:%u",
                       cfg.peers[i].host.data(), static_cast<unsigned>(cfg.peers[i].port));
    }
    out.has_static_ipv4 = cfg.has_static_ipv4;
    out.static_ipv4 = cfg.static_ipv4;
    out.static_prefix = cfg.static_prefix;
    out.dhcp = cfg.dhcp;
    if (cfg.dhcp) {
        const uint32_t subnet = cfg.dhcp_subnet_or_default();
        (void)snprintf(out.dhcp_subnet.data(), out.dhcp_subnet.size(), "%u.%u.%u.%u/%u",
                       static_cast<unsigned>((subnet >> 24) & 0xFF),
                       static_cast<unsigned>((subnet >> 16) & 0xFF),
                       static_cast<unsigned>((subnet >> 8) & 0xFF),
                       static_cast<unsigned>(subnet & 0xFF),
                       static_cast<unsigned>(cfg.dhcp_has_subnet ? cfg.dhcp_prefix
                                                                 : kDefaultDhcpPrefix));
    }
    out.enable_encryption = cfg.enable_encryption;
    out.mtu = cfg.mtu;
    return ESP_OK;
}

esp_err_t Instance::stop(uint32_t timeout_ms) noexcept {
    if (impl_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ESP_PLATFORM)
    if (!impl_->started) {
        return ESP_OK;  // 幂等
    }
    impl_->ctx.state.store(State::Stopping);
    impl_->ctx.underlay_ready.store(false);
    impl_->ctx.stop_requested.store(true);

    if (!wait_tasks(impl_->ctx, timeout_ms)) {
        // 任务尚未退出：保留全部资源，允许再次 stop()。
        return ESP_ERR_TIMEOUT;
    }
    impl_->started = false;
    if (impl_->ctx.netif_created) {
        (void)impl_->ctx.virtual_netif.set_link(false);
    }
    impl_->ctx.state.store(State::Stopped);
    impl_->ctx.io_task_handle = nullptr;
    impl_->ctx.proto_task_handle = nullptr;
    impl_->ctx.data_task_handle = nullptr;
    return ESP_OK;
#else
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t Instance::deinit() noexcept {
    if (impl_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ESP_PLATFORM)
    if (impl_->started) {
        return ESP_ERR_INVALID_STATE;  // 必须先 stop()
    }
    destroy_queues(impl_->ctx);
#endif
    // 清除密钥与网络密钥，避免残留在堆中。
    impl_->ctx.key.fill(0);
    impl_->ctx.digest.fill(0);
    impl_->ctx.config.network_secret.fill(0);
    delete impl_;
    impl_ = nullptr;
    g_instance_in_use.store(false);
    return ESP_OK;
}

}  // namespace et
