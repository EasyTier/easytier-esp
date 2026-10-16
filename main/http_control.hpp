/**
 * @file http_control.hpp
 * @brief 示例扩展：GPIO38 电平输出 + 监听 80 端口的 HTTP 控制服务。
 *
 * @details
 * - 上电把 GPIO38 配成推挽输出并置为**高电平**；
 * - 启动 HTTP 服务，对 `GET /` 与 `GET /toggle` 的请求**翻转** GPIO38 电平，并返回一个
 *   极小的 HTML 页面（只有一个 `<h1>`，内容为 `高电平` 或 `低电平`）；其他路径返回 404，
 *   不会翻转；
 * - 服务绑定在所有网络接口上（`INADDR_ANY`），因此既可以用物理网卡的 IP 访问，也可以
 *   用 EasyTier 虚拟 IP（`http://<虚拟IP>:80`）访问。虚拟网卡地址是稍后才出现的，
 *   而监听套接字绑在通配地址上，不需要重启服务。
 *
 * @warning **这个接口用 GET 触发副作用**（翻转电平），且**物理网段同样可达**：任何能连
 *          到本设备 80 端口的程序都能翻转它，浏览器预取/代理重放也可能触发。仅适合
 *          联调演示；生产固件应加鉴权、改成 POST、或只在虚拟接口上暴露。
 * @warning GPIO38 必须是本板空闲引脚。ESP32-S3 上 GPIO22-25 不存在，GPIO33-37 在
 *          Octal PSRAM/Flash 模组上已被占用；换引脚前先核对模组手册。
 */
#pragma once

#include "esp_err.h"

namespace http_control {

/**
 * @brief 使能 GPIO38 输出（默认高电平）并启动监听 80 端口的 HTTP 服务。
 *
 * @return @c ESP_OK ；初始化 GPIO 或启动/注册路由失败时返回相应错误码
 * @note 幂等：重复调用直接返回 @c ESP_OK ，不会重复启动服务。
 * @note 调用前需已完成 @c esp_netif_init() （HTTP 服务要创建 socket）。
 */
esp_err_t start() noexcept;

}  // namespace http_control
