/**
 * @file easytier_app.hpp
 * @brief 示例应用侧的 EasyTier 装配：加载内嵌 TOML、启动组网、上报事件与周期状态。
 *
 * @details 本模块把"组网"从 @c main.cpp 里独立出来：@c main.cpp 只负责 NVS、TCP/IP、
 *          WiFi 与模块装配，本模块负责 @c et::Inst 的全部调用（TOML 文本直接交给构造函数，
 *          组件没有独立的配置类）。对外只暴露三个函数，与 WiFi 事件一一对应：
 *
 * @code
 *   IP_EVENT_STA_GOT_IP          → easytier_app::on_ip_got()
 *   WIFI_EVENT_STA_DISCONNECTED  → easytier_app::on_ip_lost()
 * @endcode
 *
 * @note **为什么启动要单独开一个任务**：组网初始化需要较大栈 —— TOML 解析的递归下降、
 *       @c Inst 构造函数内部的 @c Settings 临时对象、密钥派生，以及构造结果对象里约
 *       4.5 KiB 的诊断报告；而 WiFi/IP 事件回调运行在系统事件任务
 *       （@c CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE ，本工程 2304 B）上，并且该任务
 *       是所有 WiFi/IP/其他组件事件共享的分发器。直接在回调里初始化必然栈溢出，且会
 *       阻塞别人的事件；再加上 @c netifapi_* 会同步阻塞到 TCPIP 线程，还有与事件循环
 *       互等的风险。因此回调只做一次 @c xTaskCreate ，重活交给 @ref kStartTaskStack 。
 *
 * @note 一次性任务执行完会自删除（@c vTaskDelete(nullptr) ），8 KiB 栈由 IDLE 任务回收，
 *       不会常驻。
 */
#pragma once

namespace easytier_app {

/**
 * @brief 通知物理网络已取得 IP。
 *
 * @details 首次调用创建一次性启动任务执行组网初始化；之后每次调用（WiFi 掉线重连后
 *          重新取得 IP）只补发一次 underlay 就绪 —— 少了这一步组件会一直停在
 *          @c underlay_ready=false ，永远不再连接入口（曾经的真实缺陷）。
 *
 * @note 可从 WiFi/IP 事件回调直接调用：只做 @c xTaskCreate 或零等待入队，立即返回。
 */
void on_ip_got() noexcept;

/**
 * @brief 通知物理网络已断开（组件会撤销连接与路由，恢复后自动重连）。
 * @note 组网尚未启动时为空操作；可从 WiFi 事件回调直接调用。
 */
void on_ip_lost() noexcept;

/** @brief 组网是否已初始化并启动（诊断用）。 */
bool is_started() noexcept;

}  // namespace easytier_app
