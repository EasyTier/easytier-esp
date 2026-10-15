/**
 * @file file.cpp
 * @brief 配置文件读取实现（stdio 接口，兼容 ESP-IDF VFS 与宿主文件系统）。
 */
#include "config/file.hpp"

#include <cstdio>
#include <cstring>

#include "support/log.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.CONFIG";

/**
 * @brief 检查路径是否可用。
 * @details 拒绝空路径、超过 255 字节的路径，以及内嵌 '\0' 的路径（调用方用
 *          string_view 传长度，不能假定是 NUL 结尾字符串）。
 */
[[nodiscard]] bool path_is_usable(std::string_view path) noexcept {
    if (path.empty() || path.size() > 255) {
        return false;
    }
    return path.find('\0') == std::string_view::npos;
}

}  // namespace

/**
 * @brief 读取配置文件内容。
 * @details 读取到缓冲区满时会再探测 1 字节，用于区分"刚好读完"与"文件过大"。
 */
esp_err_t read_config_file(std::string_view path, std::span<char> buf, size_t& len) noexcept {
    len = 0;
    if (!path_is_usable(path)) {
        ET_LOGE(kTag, "config path is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    // fopen() 需要 NUL 结尾路径；路径已限制在 255 字节以内。
    char path_buf[256];
    std::memcpy(path_buf, path.data(), path.size());
    path_buf[path.size()] = '\0';

    std::FILE* file = std::fopen(path_buf, "rb");
    if (file == nullptr) {
        ET_LOGE(kTag, "cannot open config file: %s", path_buf);
        return ESP_ERR_NOT_FOUND;
    }

    size_t total = 0;
    for (;;) {
        if (total == buf.size()) {
            // 缓冲区已满：再读 1 字节判断文件是否更大。
            char extra = 0;
            const size_t got = std::fread(&extra, 1, 1, file);
            if (got != 0) {
                std::fclose(file);
                ET_LOGE(kTag, "config file is larger than the %u byte limit",
                        static_cast<unsigned>(buf.size()));
                return ESP_ERR_INVALID_SIZE;
            }
            break;
        }
        const size_t got = std::fread(buf.data() + total, 1, buf.size() - total, file);
        total += got;
        if (got == 0) {
            break;
        }
    }

    if (std::ferror(file) != 0) {
        std::fclose(file);
        ET_LOGE(kTag, "read error on %s", path_buf);
        return ESP_FAIL;
    }
    std::fclose(file);
    len = total;
    return ESP_OK;
}

}  // namespace et
