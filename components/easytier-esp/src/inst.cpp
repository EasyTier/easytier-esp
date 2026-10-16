/**
 * @file inst.cpp
 * @brief @c et::Inst RAII 实例的实现。
 *
 * @details 生命周期：
 * @code
 * Inst(toml, options) → 解析 TOML、补齐身份、校验结构、分配上下文、派生密钥（不创建任务）
 * start()             → 创建队列与任务，状态置 WaitingUnderlay
 * stop()              → 置停止标志、等待任务确认退出（可超时）
 * ~Inst()             → 若仍 start 则无限等待任务退出，再释放队列/连接/密钥
 * @endcode
 * 组件内部**没有**全局单实例状态：每个 Inst 对象独占自己的设置（@ref Settings）、Context、
 * 队列、任务、socket、虚拟网卡与事件回调，因此可以同时存在多个实例（内存与任务数叠加）。
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "context.hpp"
#include "config/config.hpp"
#include "config/settings.hpp"
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
//   Context 合计 92800（不再包含 4.5 KiB 的解析诊断报告，报告由 Inst 自己持有）
//     ├─ RpcReassembler（2 槽 × (8 KiB 收包 + 8 KiB 有序缓冲)）33184
//     ├─ SyncEngine（Topology 22928 + 2 个 ConnList 8968 + 快照 524 + 编码缓冲）35240
//     ├─ RPC 收发缓冲（body 8 KiB + service 4 KiB + 其它）约 14000
//     ├─ Settings 外壳（std::string / std::vector 的小对象，文本与入口在堆上）
//     └─ 其它（AEAD 指针、原子量、dhcp/网卡状态等）
// 入口地址列表与实例名/主机名/网络名/网络密钥都由 std::string/vector 按实际长度在堆上分配，
// 因此这些文本写多长都不会让 Context 膨胀。
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

/** @brief 把文本视图拷进定长诊断缓冲（截断但保证 NUL 结尾）。 */
template <size_t N>
void copy_text(std::array<char, N>& dst, std::string_view src) noexcept {
    const size_t n = src.size() < N - 1 ? src.size() : N - 1;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

/** @brief 把生效设置与运行选项拷入上下文，并派生密钥。 */
void prepare_context(Context& ctx, const Settings& settings,
                     const RuntimeOptions& options) noexcept {
    // 设置已经在构造时补齐身份；这里做一次深拷贝并派生密钥。
    ctx.settings = settings;
    ctx.options = options;
    ctx.key = derive_key_128(ctx.settings.network_secret);
    ctx.digest = secret_digest(ctx.settings.network_name, ctx.settings.network_secret);
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
    if (ctx.settings.enable_encryption && ctx.control_cipher == nullptr) {
        ctx.control_cipher = new (std::nothrow) AesGcm128(ctx.key);
        if (ctx.control_cipher == nullptr || !ctx.control_cipher->ready()) {
            delete ctx.control_cipher;
            ctx.control_cipher = nullptr;
            ET_LOGE(kTag, "control-plane AEAD init failed (PSA/mbedTLS)");
            return ESP_ERR_NO_MEM;
        }
    }
    if (ctx.settings.enable_encryption && ctx.data_cipher == nullptr) {
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
    ctx.sync.configure(ctx.settings, ctx.my_peer_id.load());
    ctx.my_ipv4.store(ctx.settings.has_static_ipv4 ? ctx.settings.static_ipv4 : 0);
    ctx.my_prefix.store(ctx.settings.has_static_ipv4 ? ctx.settings.static_prefix : 24);
    ctx.reachable_routes.store(0);
    // 打印常驻内存规模与并发连接槽数，便于真机 bring-up 时核对 Kconfig 是否生效。
    ET_LOGI(kTag, "runtime context: %u bytes; slots: %u; frame queues: ctrl=%u data=%u tx=%u",
            static_cast<unsigned>(sizeof(Context)), static_cast<unsigned>(kMaxConnSlots),
            static_cast<unsigned>(kFrameQueueDepth), static_cast<unsigned>(kFrameQueueDepth),
            static_cast<unsigned>(kFrameQueueDepth));
    // 静态地址时立即可用；dhcp 地址在控制面算出后再创建网卡。
    // 与物理网段重叠的虚拟网段会破坏正常路由，这里显式拒绝（见 proto_task 的同名检查）。
    if (ctx.settings.has_static_ipv4) {
        const esp_err_t netif_err = ctx.virtual_netif.create(
            ctx.settings.static_ipv4, ctx.settings.static_prefix, ctx.settings.mtu,
            static_cast<void*>(ctx.q_netif_rx), &ctx.generation);
        ctx.netif_created = netif_err == ESP_OK;
        ctx.netif_ip = ctx.settings.static_ipv4;
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

/**
 * @brief 等待三个任务确认退出。
 * @param timeout_ms 等待上限；@c UINT32_MAX 表示无限等待（析构路径使用）。
 */
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

/**
 * @brief 释放运行期资源。
 * @details **顺序很重要**：必须先注销虚拟网卡再删除队列。网卡的 output 回调会把
 *          pbuf 入队到 `q_netif_rx`，而 netifapi_* 是同步的（返回时 TCPIP 线程已处理
 *          完该请求），所以先注销网卡能保证之后不会再有人往队列里写；反过来先删队列
 *          则存在"回调在队列已释放后 xQueueSend"的 use-after-free 窗口。
 * @note 可重复调用（句柄置空后再次调用即刻返回），因此析构与 start 失败回滚共用。
 */
void destroy_queues(Context& ctx) noexcept {
    ctx.virtual_netif.destroy();
    ctx.netif_created = false;
    // 队列句柄集中销毁：新增队列只需加进这张表，不会再漏一处。
    QueueHandle_t* const handles[] = {&ctx.q_rx_ctrl, &ctx.q_tx,      &ctx.q_evt,
                                      &ctx.q_io_cmd, &ctx.q_app_cmd,  &ctx.q_rx_data,
                                      &ctx.q_netif_rx, &ctx.q_snapshot};
    for (QueueHandle_t* handle : handles) {
        if (*handle != nullptr) {
            vQueueDelete(*handle);
            *handle = nullptr;
        }
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
        ctx.slots[i].peer_id.store(0);
        ctx.slots[i].generation.store(0);
    }
}
#else
/** @brief 宿主构建没有 FreeRTOS 队列与 AEAD；start/stop 返回不支持。 */
void destroy_queues(Context& ctx) noexcept { (void)ctx; }
#endif  // ESP_PLATFORM

}  // namespace

/**
 * @brief 构造实例：解析 TOML → 补齐身份 → 校验结构 → 堆上分配上下文并派生密钥。
 * @note 组件禁用异常，构造不能返回错误码；失败时对象 valid()==false，
 *       错误码见 last_error()、诊断见 report()，析构对失败对象安全。
 */
Inst::Inst(std::string_view toml, const RuntimeOptions& options) noexcept {
    report_.clear();
    // 从缺省构造的设置开始：缺省值由 Settings 的成员初始化器给出，因此这里天然是
    // "全量替换"语义，不需要额外的复位步骤。
    Settings settings{};
    last_error_ = parse_toml_into(settings, report_, toml);
    // 即使解析失败也补齐身份，保证错误信息里能看到实例名/hostname。
    settings.finalize_identity();
    if (last_error_ != ESP_OK) {
        ET_LOGE(kTag, "TOML parse failed: 0x%x", last_error_);
        return;
    }
    last_error_ = validate_config(settings);
    if (last_error_ != ESP_OK) {
        return;
    }
    // 运行上下文 92,800 B ≈ 91 KiB：RPC 重组缓冲、拓扑、邻接表、快照等。
    // 未启用 PSRAM 时可能分配失败，这里给出明确提示。
    ctx_ = new (std::nothrow) Context();
    if (ctx_ == nullptr) {
        ET_LOGE(kTag, "context allocation failed (%u bytes); enable PSRAM or shrink buffers",
                static_cast<unsigned>(sizeof(Context)));
        last_error_ = ESP_ERR_NO_MEM;
        return;
    }
    prepare_context(*ctx_, settings, options);
    ctx_->state.store(State::Stopped);
    started_ = false;
    last_error_ = ESP_OK;
}

/**
 * @brief 析构：停止任务并释放全部资源。
 * @warning 析构没有超时，会一直等到任务确认退出。事件回调运行在控制面任务里，
 *          因此在回调中析构会永久阻塞；请只在普通应用任务中销毁实例。
 */
Inst::~Inst() noexcept {
    if (ctx_ == nullptr) {
        return;
    }
#if defined(ESP_PLATFORM)
    if (started_) {
        (void)stop(UINT32_MAX);
    }
#endif
    destroy_queues(*ctx_);
    // 清除密钥与网络密钥，避免残留在堆中。
    ctx_->key.fill(0);
    ctx_->digest.fill(0);
    zeroize_string(ctx_->settings.network_secret);
    delete ctx_;
    ctx_ = nullptr;
    started_ = false;
}

esp_err_t Inst::start() noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ESP_PLATFORM)
    if (started_) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = start_runtime(*ctx_);
    if (err != ESP_OK) {
        destroy_queues(*ctx_);
        ctx_->set_error(err);
        ctx_->state.store(State::Error);
        return err;
    }
    started_ = true;
    // Started 事件由控制面任务在进入主循环时上报，保证回调上下文一致。
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t Inst::set_underlay_ready(bool ready) noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ESP_PLATFORM)
    if (!started_) {
        return ESP_ERR_INVALID_STATE;
    }
    SmallMessage msg{};
    msg.kind = static_cast<uint8_t>(ready ? AppCommand::UnderlayUp : AppCommand::UnderlayDown);
    // 非阻塞：队列满时返回错误，由调用方重试，绝不静默丢弃物理网络状态。
    if (xQueueSend(ctx_->q_app_cmd, &msg, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    (void)ready;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t Inst::get_status(Status& out) const noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const Context& ctx = *ctx_;
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
 * @details 数据来源是运行期持有的 @ref Settings（即真正生效的设置），
 *          因此可以据此确认 TOML 的映射结果。
 */
esp_err_t Inst::get_config_info(ConfigInfo& out) const noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const Settings& cfg = ctx_->settings;
    out = ConfigInfo{};
    copy_text(out.instance_name, cfg.instance_name);
    copy_text(out.hostname, cfg.hostname);
    copy_text(out.network_name, cfg.network_name);
    format_uuid_text(cfg.instance_id.data(), out.instance_id.data());
    out.peer_total_count = cfg.peers.size();
    out.peer_count = cfg.peers.size() > out.peers.size() ? out.peers.size() : cfg.peers.size();
    out.peers_truncated = cfg.peers.size() > out.peer_count;
    for (size_t i = 0; i < out.peer_count; ++i) {
        // 主机名最长 255 字节而缓冲 200：用精度限长，避免编译期截断告警。
        (void)snprintf(out.peers[i].text.data(), out.peers[i].text.size(), "%.180s:%u",
                       cfg.peers[i].host.c_str(), static_cast<unsigned>(cfg.peers[i].port));
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

esp_err_t Inst::stop(uint32_t timeout_ms) noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ESP_PLATFORM)
    if (!started_) {
        return ESP_OK;  // 幂等
    }
    ctx_->state.store(State::Stopping);
    ctx_->underlay_ready.store(false);
    ctx_->stop_requested.store(true);

    if (!wait_tasks(*ctx_, timeout_ms)) {
        // 任务尚未退出：保留全部资源，允许再次 stop()。
        return ESP_ERR_TIMEOUT;
    }
    started_ = false;
    if (ctx_->netif_created) {
        (void)ctx_->virtual_netif.set_link(false);
    }
    ctx_->state.store(State::Stopped);
    ctx_->io_task_handle = nullptr;
    ctx_->proto_task_handle = nullptr;
    ctx_->data_task_handle = nullptr;
    return ESP_OK;
#else
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

}  // namespace et
