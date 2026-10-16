/**
 * @file config.cpp
 * @brief @ref et::Settings 的补齐逻辑与解析助手
 *        （IPv4/CIDR/UUID/主机名/入口 URI）。
 */
#include "config/config.hpp"

#include <cstring>
#include <string>

#include "support/log.hpp"
#include "support/random.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.CONFIG";

/** @brief 是否为需要从主机名中剔除的控制字符。 */
[[nodiscard]] bool is_control(uint32_t cp) noexcept {
    return cp < 0x20 || cp == 0x7f;
}

/**
 * @brief 解码一个 UTF-8 码点。
 * @param s 输入
 * @param[in,out] i 当前偏移，成功时前进到下一个码点
 * @param cp 输出码点
 * @return false 表示 UTF-8 非法
 */
[[nodiscard]] bool utf8_next(std::string_view s, size_t& i, uint32_t& cp) noexcept {
    if (i >= s.size()) {
        return false;
    }
    const uint8_t b0 = static_cast<uint8_t>(s[i]);
    size_t len = 1;
    uint32_t value = 0;
    if (b0 < 0x80) {
        cp = b0;
        ++i;
        return true;
    } else if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        value = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        value = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        value = b0 & 0x07;
    } else {
        return false;
    }
    if (i + len > s.size()) {
        return false;
    }
    for (size_t k = 1; k < len; ++k) {
        const uint8_t b = static_cast<uint8_t>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            return false;
        }
        value = (value << 6) | (b & 0x3F);
    }
    i += len;
    cp = value;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Settings：身份补齐与敏感文本清除
// ---------------------------------------------------------------------------

/**
 * @brief 补齐实例身份。
 * @details 与用户约定一致：hostname 缺省回退 Espressif；instance_name 未显式配置时取
 *          hostname；instance_id 缺省生成随机 UUID v4（显式 UUID 含全零都保留）。
 */
void Settings::finalize_identity() noexcept {
    if (hostname.empty()) {
        hostname = std::string(kDefaultHostname);
    }
    if (!instance_name_explicit) {
        instance_name = hostname;
    }
    if (!has_instance_id) {
        fill_random(instance_id);
        // RFC 4122：版本位固定为 4，变体位固定为 10xx。
        instance_id[6] = static_cast<uint8_t>((instance_id[6] & 0x0f) | 0x40);
        instance_id[8] = static_cast<uint8_t>((instance_id[8] & 0x3f) | 0x80);
        has_instance_id = true;
    }
}

/** @brief 用 0 覆写内容并释放容量，避免密钥残留在堆中。 */
void zeroize_string(std::string& text) noexcept {
    for (char& c : text) {
        c = '\0';
    }
    text.clear();
    std::string().swap(text);
}

// ---------------------------------------------------------------------------
// 解析助手
// ---------------------------------------------------------------------------

/** @brief 严格解析点分十进制 IPv4（每段 0..255，不接受前导 '+' 或空白）。 */
bool parse_ipv4_numeric(std::string_view text, uint32_t& out) noexcept {
    uint32_t parts[4] = {0, 0, 0, 0};
    size_t index = 0;
    size_t i = 0;
    while (index < 4) {
        if (i >= text.size()) {
            return false;
        }
        uint32_t value = 0;
        size_t digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + static_cast<uint32_t>(text[i] - '0');
            ++digits;
            ++i;
            if (digits > 3 || value > 255) {
                return false;
            }
        }
        if (digits == 0) {
            return false;
        }
        parts[index++] = value;
        if (index < 4) {
            if (i >= text.size() || text[i] != '.') {
                return false;
            }
            ++i;
        }
    }
    if (i != text.size()) {
        return false;
    }
    out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

/**
 * @brief 解析 "IP" 或 "IP/前缀"。
 * @note 不带 "/" 时前缀默认为 24（与上游取裸地址的默认行为一致）。
 */
bool parse_ipv4_cidr(std::string_view text, uint32_t& addr, uint8_t& prefix,
                     bool& had_prefix) noexcept {
    had_prefix = false;
    const size_t slash = text.find('/');
    const std::string_view addr_part = (slash == std::string_view::npos) ? text : text.substr(0, slash);
    if (!parse_ipv4_numeric(addr_part, addr)) {
        return false;
    }
    if (slash == std::string_view::npos) {
        prefix = 24;
        return true;
    }
    const std::string_view prefix_part = text.substr(slash + 1);
    if (prefix_part.empty() || prefix_part.size() > 2) {
        return false;
    }
    uint32_t value = 0;
    for (const char c : prefix_part) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (value > 32) {
        return false;
    }
    prefix = static_cast<uint8_t>(value);
    had_prefix = true;
    return true;
}

/** @brief 解析 8-4-4-4-12 UUID 文本；连字符可省略但不能少于 32 个十六进制位。 */
bool parse_uuid_text(std::string_view text, std::array<uint8_t, 16>& out) noexcept {
    size_t out_index = 0;
    int high = -1;
    for (const char c : text) {
        if (c == '-') {
            continue;
        }
        int value = -1;
        if (c >= '0' && c <= '9') {
            value = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            value = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            value = c - 'A' + 10;
        } else {
            return false;
        }
        if (high < 0) {
            high = value;
        } else {
            if (out_index >= out.size()) {
                return false;
            }
            out[out_index++] = static_cast<uint8_t>((high << 4) | value);
            high = -1;
        }
    }
    return high < 0 && out_index == out.size();
}

/**
 * @brief 按上游规则规范化主机名。
 * @details 剔除控制字符、最多保留 32 个 Unicode 码点；遇到非法 UTF-8 即停止（与上游
 *          逐字符过滤的语义一致）。32 码点 × 最多 4 字节 = 128 字节，因此这里用一个
 *          有界的栈缓冲暂存后一次性写入 @p out 。
 */
void normalize_hostname(std::string_view in, std::string& out) noexcept {
    // 32 个码点在 UTF-8 下最多 128 字节；暂存缓冲不需要结尾 NUL（用长度赋值）。
    std::array<char, 32 * 4> staging{};
    size_t written = 0;
    size_t code_points = 0;
    size_t i = 0;
    while (i < in.size()) {
        const size_t start = i;
        uint32_t cp = 0;
        if (!utf8_next(in, i, cp)) {
            break;  // stop at malformed UTF-8, like upstream's char filter
        }
        if (is_control(cp)) {
            continue;
        }
        if (code_points >= 32) {
            break;
        }
        const size_t len = i - start;
        if (written + len > staging.size()) {
            break;
        }
        std::memcpy(staging.data() + written, in.data() + start, len);
        written += len;
        ++code_points;
    }
    out.assign(staging.data(), written);
}

/** @brief 取 IPv4 网络号（prefix 为 0 时返回 0）。 */
uint32_t ipv4_network_of(uint32_t addr, uint8_t prefix) noexcept {
    if (prefix == 0) {
        return 0;
    }
    if (prefix >= 32) {
        return addr;
    }
    const uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
    return addr & mask;
}

/** @brief 判断两个 IPv4 网段是否重叠（取较短前缀比较网络号）。 */
bool ipv4_prefixes_overlap(uint32_t a, uint8_t prefix_a, uint32_t b, uint8_t prefix_b) noexcept {
    const uint8_t common = prefix_a < prefix_b ? prefix_a : prefix_b;
    return ipv4_network_of(a, common) == ipv4_network_of(b, common);
}

/** @brief 严格校验协议、主机与端口，绝不把错误入口留到运行期 DNS/连接阶段。 */
bool parse_peer_endpoint(std::string_view uri, std::string_view& host_out,
                         uint16_t& port_out) noexcept {
    constexpr std::string_view scheme = "tcp://";
    if (!uri.starts_with(scheme)) return false;
    const auto rest = uri.substr(scheme.size());
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const auto host = rest.substr(0, colon);
    const auto port_text = rest.substr(colon + 1);
    if (host.empty() || port_text.empty() || port_text.size() > 5) return false;
    bool numeric = true;
    for (const char c : host) {
        const bool digit = c >= '0' && c <= '9';
        numeric = numeric && (digit || c == '.');
        if (!(digit || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '-' || c == '_' || c == '.')) return false;
    }
    if (numeric) {
        uint32_t ip = 0;
        if (!parse_ipv4_numeric(host, ip)) return false;
    } else {
        // DNS 标签不可为空或超长；允许完整域名末尾的根点。
        size_t start = 0;
        while (start < host.size()) {
            const auto dot = host.find('.', start);
            const size_t end = dot == std::string_view::npos ? host.size() : dot;
            if (end == start || end - start > 63 || host[start] == '-' || host[end - 1] == '-')
                return false;
            start = end + 1;
        }
    }
    uint32_t port = 0;
    for (char c : port_text) {
        if (c < '0' || c > '9') return false;
        port = port * 10 + static_cast<uint32_t>(c - '0');
    }
    if (port == 0 || port > 65535) return false;
    host_out = host;
    port_out = static_cast<uint16_t>(port);
    return true;
}

/**
 * @brief 结构完整性校验。
 * @details 要求：至少 1 个入口、必须配置静态 IPv4 或 dhcp、MTU 在范围内、网络名非空、
 *          每个入口都有主机名与端口。入口数量本身没有上限，只有可同时连接的数量由
 *          Kconfig 决定（见 kMaxConnectedPeers）。
 */
esp_err_t validate_config(const Settings& cfg) noexcept {
    if (cfg.peers.size() == 0) {
        ET_LOGE(kTag, "no usable entry peer configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg.has_static_ipv4 && !cfg.dhcp) {
        ET_LOGE(kTag, "neither ipv4 nor dhcp is configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.mtu < kMinMtu || cfg.mtu > kDefaultMtu) {
        ET_LOGE(kTag, "mtu %u out of range", static_cast<unsigned>(cfg.mtu));
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.network_name.empty()) {
        ET_LOGE(kTag, "network_name must not be empty");
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < cfg.peers.size(); ++i) {
        if (cfg.peers[i].host.empty() || cfg.peers[i].port == 0) {
            ET_LOGE(kTag, "entry peer %u is incomplete", static_cast<unsigned>(i));
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

/** @brief 把 16 字节 UUID 格式化为标准文本（8-4-4-4-12）。 */
void format_uuid_text(const uint8_t* id, char* out) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    size_t pos = 0;
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[pos++] = '-';
        }
        out[pos++] = kHex[(id[i] >> 4) & 0x0F];
        out[pos++] = kHex[id[i] & 0x0F];
    }
    out[pos] = '\0';
}

}  // namespace et
