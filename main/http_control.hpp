/**
 * @file http_control.hpp
 * @brief 示例扩展：GPIO38 电平输出 + HTTP 控制服务。
 *
 * @details `GET /` 与 `/toggle` 翻转 GPIO38 并返回极简 HTML；监听 INADDR_ANY，
 *          物理网卡和 EasyTier 虚拟 IP 均可访问。
 * @warning GET 触发副作用且物理网段可达，仅用于联调；生产固件应加鉴权、
 *          改成 POST 或只在虚拟接口上暴露。
 * @warning GPIO38 必须为本板空闲引脚；换脚前核对模组手册。
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
