/**
 * @file file.hpp
 * @brief 从已挂载的 VFS 路径有限额地读取配置文件。
 */
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "easytier-esp.h"

namespace et {

/**
 * @brief 读取配置文件。
 * @param path VFS 路径，例如 "/spiffs/easytier.toml"；不允许内嵌 '\0'
 * @param buf 输出缓冲区
 * @param len 输出实际读取长度
 * @return
 *  - @c ESP_OK 读取成功
 *  - @c ESP_ERR_INVALID_ARG 路径非法（空、过长、含 '\0'）
 *  - @c ESP_ERR_NOT_FOUND 文件不存在
 *  - @c ESP_ERR_INVALID_SIZE 文件大于 @p buf
 *  - @c ESP_FAIL 读取出错
 */
esp_err_t read_config_file(std::string_view path, std::span<char> buf, size_t& len) noexcept;

}  // namespace et
