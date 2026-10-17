/**
 * @file easytier_app.hpp
 * @brief 示例应用侧的 EasyTier 装配：加载内嵌 TOML、启动组网、上报事件与周期状态。
 *
 * @details 本模块把"组网"从 @c main.cpp 里独立出来：@c main.cpp 只负责 NVS、TCP/IP、
 *          WiFi 与模块装配，本模块负责 @c et::Inst 的全部调用（TOML 文本直接交给构造函数，
 *          组件没有独立的配置类）。对外只暴露两个事件入口，与 WiFi/IP 事件对应：
 *
 * @code
 *   IP_EVENT_STA_GOT_IP          → easytier_app::on_ip_got()
 *   WIFI_EVENT_STA_DISCONNECTED  → easytier_app::on_ip_lost()
 * @endcode
 *
 * @note **为什么启动要单独开一个任务**：组网初始化需要较大栈 —— TOML 解析的递归下降、
 *       @c Inst 构造函数内部的 @c Settings 临时对象与密钥派生。WiFi/IP 事件回调运行在
 *       系统事件任务（@c CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE ，本工程 2304 B）上，
 *       而该任务还是所有 WiFi/IP/组件事件共享的分发器：在回调里直接初始化必然栈溢出，
 *       且会阻塞别人的事件；再加上 @c netifapi_* 会同步阻塞到 TCPIP 线程，还有与事件
 *       循环互等的风险。因此回调只做一次 @c xTaskCreate ，重活交给 8 KiB 的启动任务。
 *
 * @note 一次性任务执行完会自删除（@c vTaskDelete(nullptr) ），8 KiB 栈由 IDLE 任务回收，
 *       不会常驻。
 */
#pragma once

namespace easytier_app {

/**
 * @brief 通知组件可以连接（WiFi 已取得 IP）。
 *
 * @details 首次调用创建一次性启动任务执行组网初始化；之后每次调用（WiFi 掉线重连后
 *          重新取得 IP）只再调用一次 @c et::Inst::connect() —— 少了这一步组件会一直
 *          停在 @c WaitingUnderlay ，永远不再连接入口（曾经的真实缺陷）。
 *
 * @note 可从 WiFi/IP 事件回调直接调用：只做 @c xTaskCreate 或零等待入队，立即返回。
 */
void on_ip_got() noexcept;

/**
 * @brief 通知组件断开（组件会撤销连接与路由，恢复后重新 connect）。
 * @note 组网尚未启动时为空操作；可从 WiFi 事件回调直接调用。
 */
void on_ip_lost() noexcept;

}  // namespace easytier_app
