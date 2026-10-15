/**
 * @file config.cpp
 * @brief 配置解析与校验实现（IPv4/CIDR/UUID/主机名规范化）。
 */
#include "config/config.hpp"

#include <cstring>

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

/** @brief 复位为默认配置（instance_name/network_name = "default"，密钥为空串）。 */
void apply_config_defaults(StoredConfig& cfg) noexcept {
    cfg = StoredConfig{};
    (void)set_fixed_string(cfg.instance_name, "default");
    (void)set_fixed_string(cfg.network_name, "default");
    (void)set_fixed_string(cfg.hostname, "Espressif");
    cfg.mtu = kDefaultMtu;
    cfg.static_prefix = 24;
    cfg.dhcp_subnet = kDefaultDhcpSubnet;
    cfg.enable_encryption = true;  // 与上游 common.proto 的 enable_encryption 默认值一致
}

/** @brief 对齐上游 get_id() 的延迟生成与缓存，主机名由 ESP Host 提供回退值。 */
void finalize_config_identity(StoredConfig& cfg) noexcept {
    if (!cfg.has_instance_id) {
        fill_random(cfg.instance_id);
        cfg.instance_id[6] = static_cast<uint8_t>((cfg.instance_id[6] & 0x0f) | 0x40);
        cfg.instance_id[8] = static_cast<uint8_t>((cfg.instance_id[8] & 0x3f) | 0x80);
        cfg.has_instance_id = true;
    }
    if (cfg.hostname[0] == '\0') (void)set_fixed_string(cfg.hostname, "Espressif");
}

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

/** @brief 数值 IPv4 转点分文本（不分配，写入调用方缓冲区）。 */
std::string_view ipv4_to_text(uint32_t value, std::array<char, 16>& buf) noexcept {
    size_t pos = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const uint32_t octet = (value >> shift) & 0xFF;
        char digits[3];
        size_t n = 0;
        uint32_t v = octet;
        do {
            digits[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        while (n > 0) {
            if (pos + 1 < buf.size()) {
                buf[pos++] = digits[--n];
            } else {
                --n;
            }
        }
        if (shift != 0 && pos + 1 < buf.size()) {
            buf[pos++] = '.';
        }
    }
    buf[pos] = '\0';
    return {buf.data(), pos};
}

/**
 * @brief 按上游规则规范化主机名。
 * @details 剔除控制字符、最多保留 32 个 Unicode 码点、UTF-8 结果不超过 127 字节；
 *          遇到非法 UTF-8 即停止（与上游逐字符过滤的语义一致）。
 */
std::string_view normalize_hostname(std::string_view in,
                                    std::array<char, kMaxHostnameBytes>& out) noexcept {
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
        if (written + len >= out.size()) {
            break;
        }
        std::memcpy(out.data() + written, in.data() + start, len);
        written += len;
        ++code_points;
    }
    out[written] = '\0';
    return {out.data(), written};
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
bool parse_peer_endpoint(std::string_view uri, PeerEndpoint& out) noexcept {
    constexpr std::string_view scheme = "tcp://";
    if (!uri.starts_with(scheme)) return false;
    const auto rest = uri.substr(scheme.size());
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const auto host = rest.substr(0, colon);
    const auto port_text = rest.substr(colon + 1);
    if (host.size() > kMaxPeerHostBytes || port_text.empty() || port_text.size() > 5) return false;
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
    PeerEndpoint parsed{};
    if (!set_fixed_string(parsed.host, host)) return false;
    parsed.port = static_cast<uint16_t>(port);
    out = parsed;
    return true;
}

/**
 * @brief 结构完整性校验。
 * @details 要求：至少 1 个入口（且不超过 kMaxPeers）、必须配置静态 IPv4 或 dhcp、
 *          MTU 在范围内、网络名非空、每个入口都有主机名与端口。
 */
esp_err_t validate_stored_config(const StoredConfig& cfg) noexcept {
    if (cfg.peer_count == 0) {
        ET_LOGE(kTag, "no usable entry peer configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.peer_count > kMaxPeers) {
        ET_LOGE(kTag, "too many entry peers: %u", static_cast<unsigned>(cfg.peer_count));
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg.has_static_ipv4 && !cfg.dhcp) {
        ET_LOGE(kTag, "neither ipv4 nor dhcp is configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.mtu < kMinMtu || cfg.mtu > kMaxMtu) {
        ET_LOGE(kTag, "mtu %u out of range", static_cast<unsigned>(cfg.mtu));
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.network_name[0] == '\0') {
        ET_LOGE(kTag, "network_name must not be empty");
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < cfg.peer_count; ++i) {
        if (cfg.peers[i].host[0] == '\0' || cfg.peers[i].port == 0) {
            ET_LOGE(kTag, "entry peer %u is incomplete", static_cast<unsigned>(i));
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

/**
 * @brief 由编程式 Config 构造 StoredConfig。
 * @details 仅接受受支持的 tcp:// 入口；其它 scheme、IPv6 或错误 URI 返回错误。
 *          静态 ipv4 存在时 dhcp 被忽略（与上游优先级一致）。
 */
esp_err_t stored_config_from_config(const Config& config, StoredConfig& out,
                                    ConfigReport* report) noexcept {
    apply_config_defaults(out);
    auto ignore = [report](ConfigIssue issue, const char* key) {
        if (report == nullptr) {
            return;
        }
        if (report->count < kMaxDiagnostics) {
            ConfigDiagnostic& d = report->items[report->count++];
            d.issue = issue;
            (void)set_fixed_string(d.key, key);
        } else {
            ++report->omitted_count;
        }
    };

    if (!set_fixed_string(out.instance_name, config.instance_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    std::array<char, kMaxHostnameBytes> normalized{};
    const auto hostname = normalize_hostname(config.hostname, normalized);
    if (!hostname.empty()) (void)set_fixed_string(out.hostname, hostname);
    if (!set_fixed_string(out.network_name, config.network_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!set_fixed_string(out.network_secret, config.network_secret)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config.instance_id.empty() && !parse_uuid_text(config.instance_id, out.instance_id)) {
        ET_LOGE(kTag, "instance_id is not a valid UUID");
        return ESP_ERR_INVALID_ARG;
    }
    out.has_instance_id = !config.instance_id.empty();
    if (config.peers.size() > kMaxPeers) {
        ET_LOGE(kTag, "too many entry peers: %u", static_cast<unsigned>(config.peers.size()));
        return ESP_ERR_INVALID_ARG;
    }
    for (const std::string_view peer : config.peers) {
        PeerEndpoint endpoint{};
        if (!parse_peer_endpoint(peer, endpoint)) {
            ignore(ConfigIssue::InvalidValue, "peer.uri");
            ET_LOGE(kTag, "invalid or unsupported entry peer URI");
            return ESP_ERR_INVALID_ARG;
        }
        out.peers[out.peer_count++] = endpoint;
    }

    if (!config.virtual_ipv4.empty()) {
        bool had_prefix = false;
        uint8_t prefix = 24;
        uint32_t addr = 0;
        if (!parse_ipv4_cidr(config.virtual_ipv4, addr, prefix, had_prefix)) {
            ET_LOGE(kTag, "ipv4 is not a valid IPv4 address or CIDR");
            return ESP_ERR_INVALID_ARG;
        }
        out.has_static_ipv4 = true;
        out.static_ipv4 = addr;
        // Upstream normalizes a bare address and /32 to /24.
        out.static_prefix = (had_prefix && prefix != 32) ? prefix : 24;
        if (had_prefix && prefix == 32) {
            ignore(ConfigIssue::IgnoredParameter, "ipv4.prefix");
        }
    } else if (config.dhcp) {
        out.dhcp = true;
        if (!config.dhcp_subnet.empty()) {
            uint32_t subnet = 0;
            uint8_t prefix = 0;
            bool had_prefix = false;
            if (!parse_ipv4_cidr(config.dhcp_subnet, subnet, prefix, had_prefix) || !had_prefix ||
                prefix > 30) {
                ET_LOGE(kTag, "dhcp subnet must be an IPv4 CIDR with prefix 0..=30");
                return ESP_ERR_INVALID_ARG;
            }
            out.dhcp_has_subnet = true;
            out.dhcp_subnet = ipv4_network_of(subnet, prefix);
            out.dhcp_prefix = prefix;
        }
    }
    out.mtu = config.mtu;
    out.enable_encryption = config.enable_encryption;
    return validate_stored_config(out);
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
