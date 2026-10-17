/**
 * @file context.cpp
 * @brief @ref et::Context 的生命周期与运行资源管理（RAII）。
 *
 * @details 生命周期：
 * @code
 * prepare(settings, options) → 拷贝设置、派生密钥、随机 peer id（不创建任务）
 * start()                    → 建队列 / AEAD / TcpConn / 网卡 / 任务
 * stop(timeout)              → 置停止标志、等待任务确认退出（可超时）
 * ~Context()                 → 若仍在运行则无限等待停止，再释放资源并清零密钥
 * @endcode
 * 组件内**没有**全局单实例状态：每个 Context 独占自己的队列、任务、socket、网卡与
 * 加解密实例，因此多个 @ref et::Inst 可以并存（内存与任务数按实例叠加）。
 */
#include "context.h"

#include <array>
#include <cstdio>
#include <new>
#include <string_view>

#include "config/config.hpp"
#include "config/settings.hpp"
#include "crypto/kdf.hpp"
#include "support/config_limits.hpp"
#include "support/log.hpp"
#include "support/random.hpp"
#include "transport/tcp.hpp"

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace et {
namespace {

constexpr const char* kTag = "ET.CONTEXT";

// 单个实例的常驻内存必须保持有界：Context 在堆上分配（大于 16 KiB 的分配会走
// PSRAM），但仍要有上限，避免后续加入功能时悄悄膨胀。实测组成（ESP32-S3，字节）：
//   Context 合计 92808（不含解析诊断：诊断已改为直接写日志，不再缓存报告）
//     ├─ RpcReassembler（2 槽 × (8 KiB 收包 + 8 KiB 有序缓冲)）33184
//     ├─ SyncEngine（Topology 22928 + 2 个 ConnList 8968 + 快照 524 + 编码缓冲）35240
//     ├─ RPC 收发缓冲（body 8 KiB + service 4 KiB + 其它）约 14000
//     ├─ Settings 外壳（std::string / std::vector 的小对象，文本与入口在堆上）
//     └─ 其它（AEAD 指针、原子量、dhcp/网卡状态等）
// 入口地址列表与实例名/主机名/网络名/网络密钥都由 std::string/vector 按实际长度在堆上分配，
// 因此这些文本写多长都不会让 Context 膨胀。
// 另外每个实例还有：帧队列（q_rx_ctrl/q_tx/q_rx_data，深度见 Kconfig
// `EASYTIER_ESP_QUEUE_*`，默认各 8，每元素约 2 KiB）、每个连接槽一个 TcpConn 约 12 KiB
// （堆，数量见 Kconfig `EASYTIER_ESP_MAX_CONNECTED_PEERS`，默认 6）、
// 三个任务栈 6+10+8 = 24 KiB。因此**强烈建议启用 PSRAM**（示例 sdkconfig 已启用，
// 大于 16 KiB 的分配会落到 PSRAM）。新增缓冲时请同步更新本注释与 README 的内存表。
static_assert(sizeof(Context) < 112 * 1024, "Context 常驻内存超出预算，请检查新增缓冲");

#if defined(ESP_PLATFORM)
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
#endif

/** @brief 把文本视图拷进定长诊断缓冲（截断但保证 NUL 结尾）。 */
template <size_t N>
void copy_text(std::array<char, N>& dst, std::string_view src) noexcept {
    const size_t n = src.size() < N - 1 ? src.size() : N - 1;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

}  // namespace

/** @brief 准备上下文：拷贝设置/运行选项、派生密钥与摘要、生成随机 peer id。 */
void Context::prepare(const Settings& new_settings, const RuntimeOptions& new_options) noexcept {
    settings = new_settings;
    options = new_options;
    key = derive_key_128(settings.network_secret);
    digest = secret_digest(settings.network_name, settings.network_secret);
    const uint32_t peer_id = random_u32();
    my_peer_id.store(peer_id == 0 ? 1u : peer_id);
    state.store(State::Stopped);
    started_ = false;
}

/** @brief 启动：创建运行资源与任务；失败时回滚并置 Error 状态。 */
esp_err_t Context::start() noexcept {
#if defined(ESP_PLATFORM)
    if (started_) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = create_runtime();
    if (err != ESP_OK) {
        release_runtime();
        set_error(err);
        state.store(State::Error);
        return err;
    }
    started_ = true;
    // Started 事件由控制面任务在进入主循环时上报，保证回调上下文一致。
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/** @brief 停止（幂等）：等待任务退出；超时保留资源以便重试。 */
esp_err_t Context::stop(uint32_t timeout_ms) noexcept {
#if defined(ESP_PLATFORM)
    if (!started_) {
        return ESP_OK;  // 幂等
    }
    state.store(State::Stopping);
    underlay_ready.store(false);
    stop_requested.store(true);

    if (!wait_tasks(timeout_ms)) {
        // 任务尚未退出：保留全部资源，允许再次 stop()。
        return ESP_ERR_TIMEOUT;
    }
    started_ = false;
    if (netif_created) {
        (void)virtual_netif.set_link(false);
    }
    state.store(State::Stopped);
    io_task_handle = nullptr;
    proto_task_handle = nullptr;
    data_task_handle = nullptr;
    return ESP_OK;
#else
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/** @brief 通知物理网络就绪/断开（零等待入队）。 */
esp_err_t Context::set_underlay_ready(bool ready) noexcept {
#if defined(ESP_PLATFORM)
    if (!started_) {
        return ESP_ERR_INVALID_STATE;
    }
    SmallMessage msg{};
    msg.kind = static_cast<uint8_t>(ready ? AppCommand::UnderlayUp : AppCommand::UnderlayDown);
    // 非阻塞：队列满时返回错误，由调用方重试，绝不静默丢弃物理网络状态。
    if (xQueueSend(q_app_cmd, &msg, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    (void)ready;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/** @brief 拷贝状态快照。 */
void Context::get_status(Status& out) const noexcept {
    out.state = state.load();
    out.peer_id = my_peer_id.load();
    out.virtual_ipv4 = my_ipv4.load();
    out.prefix_len = my_prefix.load();
    out.direct_peer_count = direct_peers.load();
    out.reachable_route_count = reachable_routes.load();
    out.last_error = last_error.load();
    out.dropped_packets = dropped.load();
}

/** @brief 拷贝生效配置快照（不含网络密钥）。 */
void Context::get_config_info(ConfigInfo& out) const noexcept {
    const Settings& cfg = settings;
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
}

/** @brief 析构：停止仍在运行的任务（无限等待），释放资源并清零密钥。 */
Context::~Context() noexcept {
#if defined(ESP_PLATFORM)
    if (started_) {
        (void)stop(UINT32_MAX);
    }
#endif
    release_runtime();
    // 清除密钥与网络密钥，避免残留在堆中。
    key.fill(0);
    digest.fill(0);
    zeroize_string(settings.network_secret);
}

#if defined(ESP_PLATFORM)
/** @brief 创建队列与任务；失败时返回错误码，由 @ref start 统一回滚。 */
esp_err_t Context::create_runtime() noexcept {
    if (q_rx_ctrl == nullptr) {
        // 队列深度来自 Kconfig menu "队列深度"（宿主默认值同 config_limits.hpp）。
        // 注意 q_snapshot 固定深度 1：它是"只保留最新路由快照"的覆盖语义。
        q_rx_ctrl = xQueueCreate(static_cast<UBaseType_t>(kQueueRxCtrlDepth),
                                 sizeof(FrameMessage));
        q_tx = xQueueCreate(static_cast<UBaseType_t>(kQueueTxDepth), sizeof(FrameMessage));
        q_evt = xQueueCreate(static_cast<UBaseType_t>(kQueueSmallDepth), sizeof(SmallMessage));
        q_io_cmd = xQueueCreate(static_cast<UBaseType_t>(kQueueSmallDepth),
                                sizeof(SmallMessage));
        q_app_cmd = xQueueCreate(static_cast<UBaseType_t>(kQueueSmallDepth),
                                 sizeof(SmallMessage));
        q_rx_data = xQueueCreate(static_cast<UBaseType_t>(kQueueRxDataDepth),
                                 sizeof(FrameMessage));
        q_netif_rx = xQueueCreate(static_cast<UBaseType_t>(kQueueNetifDepth),
                                  sizeof(NetifRxItem));
        q_snapshot = xQueueCreate(static_cast<UBaseType_t>(kQueueSnapshotDepth),
                                  sizeof(RouteSnapshot));
        task_exit = xSemaphoreCreateCounting(kTaskCount, 0);
        if (q_rx_ctrl == nullptr || q_tx == nullptr || q_evt == nullptr ||
            q_io_cmd == nullptr || q_app_cmd == nullptr || q_rx_data == nullptr ||
            q_netif_rx == nullptr || q_snapshot == nullptr || task_exit == nullptr) {
            ET_LOGE(kTag,
                    "queue/semaphore allocation failed; lower the depths in Kconfig "
                    "menu \"队列深度\" or enable PSRAM");
            return ESP_ERR_NO_MEM;
        }
    }

    stop_requested.store(false);
    underlay_ready.store(false);
    dropped.store(0);
    last_error.store(ESP_OK);
    state.store(State::WaitingUnderlay);

    // 控制面与数据面各持一个 AEAD 实例，禁止跨任务共享。关闭加密时按上游
    // NullCipher 语义不创建任何 AEAD，因此这些报文不会带 AEAD 尾或加密位。
    if (settings.enable_encryption && control_cipher == nullptr) {
        control_cipher = new (std::nothrow) AesGcm128(key);
        if (control_cipher == nullptr || !control_cipher->ready()) {
            delete control_cipher;
            control_cipher = nullptr;
            ET_LOGE(kTag, "control-plane AEAD init failed (PSA/mbedTLS)");
            return ESP_ERR_NO_MEM;
        }
    }
    if (settings.enable_encryption && data_cipher == nullptr) {
        data_cipher = new (std::nothrow) AesGcm128(key);
        if (data_cipher == nullptr || !data_cipher->ready()) {
            delete data_cipher;
            data_cipher = nullptr;
            ET_LOGE(kTag, "data-plane AEAD init failed (PSA/mbedTLS)");
            return ESP_ERR_NO_MEM;
        }
    }
    // 每个连接槽一个 TcpConn（各含约 12 KiB 收发缓冲）。必须显式分配：对象较大，
    // 不能放在任务栈上；缺少分配时 I/O 任务会直接报错退出（连不上任何入口）。
    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        if (slots[i].conn == nullptr) {
            slots[i].conn = new (std::nothrow) TcpConn();
            if (slots[i].conn == nullptr) {
                ET_LOGE(kTag, "TcpConn slot %u allocation failed (needs ~12 KiB each)",
                        static_cast<unsigned>(i));
                return ESP_ERR_NO_MEM;
            }
        }
    }

    // 路由同步引擎：拓扑、最短路、自身信息与 dhcp 自分配都在其中。
    sync.configure(settings, my_peer_id.load());
    my_ipv4.store(settings.has_static_ipv4 ? settings.static_ipv4 : 0);
    my_prefix.store(settings.has_static_ipv4 ? settings.static_prefix : 24);
    reachable_routes.store(0);
    // 打印常驻内存规模、连接槽数与各队列深度，便于真机 bring-up 时核对 Kconfig 是否生效。
    ET_LOGI(kTag,
            "runtime context: %u bytes; slots: %u; queues: rx_ctrl=%u tx=%u rx_data=%u "
            "small=%u netif=%u",
            static_cast<unsigned>(sizeof(Context)), static_cast<unsigned>(kMaxConnSlots),
            static_cast<unsigned>(kQueueRxCtrlDepth), static_cast<unsigned>(kQueueTxDepth),
            static_cast<unsigned>(kQueueRxDataDepth), static_cast<unsigned>(kQueueSmallDepth),
            static_cast<unsigned>(kQueueNetifDepth));
    // 静态地址时立即可用；dhcp 地址在控制面算出后再创建网卡。
    // 与物理网段重叠的虚拟网段会破坏正常路由，这里显式拒绝（见 proto_task 的同名检查）。
    if (settings.has_static_ipv4) {
        const esp_err_t netif_err = virtual_netif.create(
            settings.static_ipv4, settings.static_prefix, settings.mtu,
            static_cast<void*>(q_netif_rx), &generation);
        netif_created = netif_err == ESP_OK;
        netif_ip = settings.static_ipv4;
        if (netif_err != ESP_OK) {
            set_error(netif_err);
        }
    }

    if (xTaskCreate(et_io_task_entry, "et_io", kIoTaskStack, this, kIoTaskPriority,
                    &io_task_handle) != pdPASS) {
        ET_LOGE(kTag, "cannot create et_io task (stack %u bytes)", kIoTaskStack);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(et_proto_task_entry, "et_proto", kProtoTaskStack, this, kProtoTaskPriority,
                    &proto_task_handle) != pdPASS) {
        ET_LOGE(kTag, "cannot create et_proto task (stack %u bytes)", kProtoTaskStack);
        stop_requested.store(true);
        (void)xSemaphoreTake(task_exit, pdMS_TO_TICKS(1000));
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(et_data_task_entry, "et_data", kDataTaskStack, this, kDataTaskPriority,
                    &data_task_handle) != pdPASS) {
        ET_LOGE(kTag, "cannot create et_data task (stack %u bytes)", kDataTaskStack);
        stop_requested.store(true);
        for (int i = 0; i < 2; ++i) {
            (void)xSemaphoreTake(task_exit, pdMS_TO_TICKS(1000));
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** @brief 等待三个任务确认退出。 */
bool Context::wait_tasks(uint32_t timeout_ms) noexcept {
    if (timeout_ms == UINT32_MAX) {
        // 析构路径：无限等待，绝不释放仍被任务访问的资源。
        for (size_t i = 0; i < kTaskCount; ++i) {
            if (xSemaphoreTake(task_exit, portMAX_DELAY) != pdTRUE) {
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
        if (xSemaphoreTake(task_exit, remain) != pdTRUE) {
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
void Context::release_runtime() noexcept {
    virtual_netif.destroy();
    netif_created = false;
    // 队列句柄集中销毁：新增队列只需加进这张表，不会再漏一处。
    QueueHandle_t* const handles[] = {&q_rx_ctrl, &q_tx,      &q_evt,
                                      &q_io_cmd, &q_app_cmd,  &q_rx_data,
                                      &q_netif_rx, &q_snapshot};
    for (QueueHandle_t* handle : handles) {
        if (*handle != nullptr) {
            vQueueDelete(*handle);
            *handle = nullptr;
        }
    }
    if (task_exit != nullptr) {
        vSemaphoreDelete(task_exit);
        task_exit = nullptr;
    }
    delete control_cipher;
    control_cipher = nullptr;
    delete data_cipher;
    data_cipher = nullptr;
    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        delete slots[i].conn;
        slots[i].conn = nullptr;
        slots[i].peer_id.store(0);
        slots[i].generation.store(0);
    }
}
#else
/** @brief 宿主构建没有 FreeRTOS 队列与任务；start/stop 返回不支持。 */
esp_err_t Context::create_runtime() noexcept { return ESP_ERR_NOT_SUPPORTED; }
/** @brief 宿主构建没有运行期资源可释放。 */
void Context::release_runtime() noexcept {}
/** @brief 宿主构建不会启动任务。 */
bool Context::wait_tasks(uint32_t) noexcept { return true; }
#endif  // ESP_PLATFORM

}  // namespace et
