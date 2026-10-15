/**
 * @file toml.cpp
 * @brief TOML 配置映射实现（基于 vendored tomlc17）。
 *
 * @details 关键点：
 * - 解析器的全局 allocator 被替换为"累计字节预算"分配器，超过预算立即失败，
 *   从而把语法树内存占用变成硬上限；
 * - 解析前后分别做嵌套深度预扫描与语法树结构校验，拒绝异常放大；
 * - 支持字段与上游同名同义，未实现字段一律"忽略 + 诊断"，不因未知内容拒绝配置。
 */
#include "config/toml.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "support/config_limits.hpp"
#include "support/log.hpp"

extern "C" {
#include "tomlc17.h"
}

namespace et {
namespace {

constexpr const char* kTag = "ET.TOML";

/** @brief TOML 解析器分配预算状态（解析过程串行，故用文件级状态即可）。 */
struct BudgetState {
    size_t limit = 0;      ///< 累计分配上限
    size_t used = 0;       ///< 本次解析累计分配字节数
    bool exceeded = false;  ///< 是否已超预算
    bool active = false;    ///< 是否处于一次解析中
};

BudgetState g_budget;

/**
 * @brief 预算分配器。
 * @details 以"累计申请字节数"作为上限，是峰值占用的保守上界（只多不少），
 *          并且不依赖 realloc 旧长度信息。
 */
void* budget_realloc(void* ptr, size_t size) noexcept {
    if (g_budget.active) {
        g_budget.used += size;
        if (g_budget.used > g_budget.limit) {
            g_budget.exceeded = true;
            return nullptr;
        }
    }
    return std::realloc(ptr, size);
}

/** @brief 释放回调（预算只统计申请量，不做扣减）。 */
void budget_free(void* ptr) noexcept { std::free(ptr); }

/**
 * @brief 首次加载时安装解析器全局选项。
 * @details 组件内部串行执行配置加载，且这些选项安装后不再修改，因此不存在
 *          并发改写全局状态的问题；同时启用 UTF-8 校验。
 */
void ensure_parser_options() noexcept {
    static bool installed = false;
    if (installed) {
        return;
    }
    toml_option_t opt = et_toml_default_option();
    opt.check_utf8 = true;
    opt.mem_realloc = budget_realloc;
    opt.mem_free = budget_free;
    et_toml_set_option(opt);
    installed = true;
}

/**
 * @brief 追加一条诊断（带源码位置）。
 * @note 只记录字段路径，绝不记录取值，避免密钥/私钥泄漏到日志或界面。
 */
void add_diag(ConfigReport* report, ConfigIssue issue, const toml_datum_t& at,
              std::string_view key) noexcept {
    if (report == nullptr) {
        return;
    }
    if (report->count >= kMaxDiagnostics) {
        ++report->omitted_count;
        return;
    }
    ConfigDiagnostic& d = report->items[report->count++];
    d.issue = issue;
    d.line = at.lineno > 0 ? static_cast<uint32_t>(at.lineno) : 0;
    d.column = at.colno > 0 ? static_cast<uint32_t>(at.colno) : 0;
    const size_t n = key.size() < kMaxDiagnosticKey ? key.size() : kMaxDiagnosticKey - 1;
    std::memcpy(d.key.data(), key.data(), n);
    d.key[n] = '\0';
}

/** @brief 追加一条无位置的诊断（用于整表/整体忽略）。 */
void add_diag(ConfigReport* report, ConfigIssue issue, std::string_view key) noexcept {
    toml_datum_t nowhere{};
    add_diag(report, issue, nowhere, key);
}

/** @brief 取字符串类型 datum 的视图；非字符串返回空视图。 */
[[nodiscard]] std::string_view datum_sv(const toml_datum_t& d) noexcept {
    if (d.type != TOML_STRING || d.u.str.ptr == nullptr) {
        return {};
    }
    return {d.u.str.ptr, static_cast<size_t>(d.u.str.len)};
}

/** @brief 取表中第 i 个键名。 */
[[nodiscard]] std::string_view table_key(const toml_datum_t& table, int32_t i) noexcept {
    const char* key = table.u.tab.key[i];
    const int len = table.u.tab.len[i];
    if (key == nullptr || len < 0) {
        return {};
    }
    return {key, static_cast<size_t>(len)};
}

/**
 * @brief 拼接 "prefix.name" 形式的字段路径。
 * @param buf 调用方提供的栈缓冲
 * @return 指向 buf 的视图
 */
[[nodiscard]] std::string_view join_key(char* buf, size_t cap, std::string_view prefix,
                                        std::string_view name) noexcept {
    size_t n = 0;
    auto append = [&](std::string_view s) {
        for (const char c : s) {
            if (n + 1 >= cap) {
                break;
            }
            buf[n++] = c;
        }
    };
    append(prefix);
    append(name);
    buf[n] = '\0';
    return {buf, n};
}

/** @brief 语法树规模统计结果。 */
struct TreeStats {
    size_t nodes = 0;  ///< 已统计节点数
    bool ok = true;    ///< 是否仍在限额内
};

/**
 * @brief 递归统计语法树规模并检查限额。
 * @details 检查节点总数、单数组元素数、单表键数与嵌套深度；任一项超限即置
 *          ok=false 并停止遍历。
 */
void count_tree(const toml_datum_t& node, size_t depth, TreeStats& stats) noexcept {
    if (!stats.ok) {
        return;
    }
    if (depth > kTomlMaxDepth || ++stats.nodes > kTomlMaxNodes) {
        stats.ok = false;
        return;
    }
    if (node.type == TOML_ARRAY) {
        if (node.u.arr.size < 0 || static_cast<size_t>(node.u.arr.size) > kTomlMaxArrayItems) {
            stats.ok = false;
            return;
        }
        for (int32_t i = 0; i < node.u.arr.size; ++i) {
            count_tree(node.u.arr.elem[i], depth + 1, stats);
        }
    } else if (node.type == TOML_TABLE) {
        if (node.u.tab.size < 0 || static_cast<size_t>(node.u.tab.size) > kTomlMaxTableKeys) {
            stats.ok = false;
            return;
        }
        for (int32_t i = 0; i < node.u.tab.size; ++i) {
            count_tree(node.u.tab.value[i], depth + 1, stats);
        }
    }
}

/** @brief 是否为需要按语义处理的根级键（其余键整体忽略并报告）。 */
[[nodiscard]] bool is_known_root_key(std::string_view key) noexcept {
    return key == "instance_name" || key == "hostname" || key == "instance_id" || key == "ipv4" ||
           key == "dhcp" || key == "network_identity" || key == "peer" || key == "flags";
}

/**
 * @brief 宽容读取可选字符串：类型或容量不匹配只记录忽略，绝不覆盖默认值。
 * @note 定长 C 字符串无法表示内嵌 NUL；这类合法 TOML 字符串也按不可采用处理。
 */
std::string_view optional_string(ConfigReport* report, const toml_datum_t& top,
                                  const char* key) noexcept {
    const toml_datum_t datum = et_toml_get(top, key);
    if (datum.type == TOML_UNKNOWN) return {};
    if (datum.type != TOML_STRING || datum_sv(datum).find('\0') != std::string_view::npos) {
        add_diag(report, ConfigIssue::IgnoredParameter, datum, key);
        return {};
    }
    return datum_sv(datum);
}

/** @brief 按上游字段名提取网络身份；缺失或不能采用的字段分别保留默认值。 */
void map_network_identity(ConfigReport* report, const toml_datum_t& top,
                           StoredConfig& out) noexcept {
    const toml_datum_t table = et_toml_get(top, "network_identity");
    if (table.type == TOML_UNKNOWN) return;
    if (table.type != TOML_TABLE) {
        add_diag(report, ConfigIssue::IgnoredParameter, table, "network_identity");
        return;
    }
    for (int32_t i = 0; i < table.u.tab.size; ++i) {
        const std::string_view name = table_key(table, i);
        const toml_datum_t datum = table.u.tab.value[i];
        const std::string_view value = datum_sv(datum);
        char path[192];
        const auto full = join_key(path, sizeof(path), "network_identity.", name);
        bool adopted = false;
        if (datum.type == TOML_STRING && value.find('\0') == std::string_view::npos) {
            if (name == "network_name" && !value.empty()) {
                adopted = set_fixed_string(out.network_name, value);
            } else if (name == "network_secret") {
                adopted = set_fixed_string(out.network_secret, value);
            }
        }
        if (!adopted) add_diag(report, ConfigIssue::IgnoredParameter, datum, full);
    }
}

/** @brief 严格读取 TCP 入口；错误、不支持或超容量均拒绝加载。 */
esp_err_t map_peers(ConfigReport* report, const toml_datum_t& top, StoredConfig& out) noexcept {
    const toml_datum_t array = et_toml_get(top, "peer");
    if (array.type == TOML_UNKNOWN) return ESP_OK;
    if (array.type != TOML_ARRAY) {
        add_diag(report, ConfigIssue::InvalidValue, array, "peer");
        return ESP_ERR_INVALID_ARG;
    }
    for (int32_t i = 0; i < array.u.arr.size; ++i) {
        const toml_datum_t entry = array.u.arr.elem[i];
        if (entry.type != TOML_TABLE) {
            add_diag(report, ConfigIssue::InvalidValue, entry, "peer");
            return ESP_ERR_INVALID_ARG;
        }
        for (int32_t k = 0; k < entry.u.tab.size; ++k) {
            const auto name = table_key(entry, k);
            if (name == "uri") continue;
            char path[192];
            add_diag(report, ConfigIssue::IgnoredParameter, entry.u.tab.value[k],
                     join_key(path, sizeof(path), "peer.", name));
        }
        const toml_datum_t uri = et_toml_get(entry, "uri");
        PeerEndpoint endpoint{};
        if (uri.type != TOML_STRING || datum_sv(uri).find('\0') != std::string_view::npos ||
            !parse_peer_endpoint(datum_sv(uri), endpoint)) {
            add_diag(report, uri.type == TOML_UNKNOWN ? ConfigIssue::MissingRequiredValue
                                                       : ConfigIssue::InvalidValue,
                     uri.type == TOML_UNKNOWN ? entry : uri, "peer.uri");
            return ESP_ERR_INVALID_ARG;
        }
        if (out.peer_count >= kMaxPeers) {
            add_diag(report, ConfigIssue::ResourceLimit, uri, "peer");
            return ESP_ERR_INVALID_ARG;
        }
        out.peers[out.peer_count++] = endpoint;
    }
    return ESP_OK;
}

/** @brief 仅采用有效 MTU；不支持的 flags 无论取值类型如何都只忽略。 */
void map_flags(ConfigReport* report, const toml_datum_t& top, StoredConfig& out) noexcept {
    const toml_datum_t flags = et_toml_get(top, "flags");
    if (flags.type == TOML_UNKNOWN) return;
    if (flags.type != TOML_TABLE) {
        add_diag(report, ConfigIssue::IgnoredParameter, flags, "flags");
        return;
    }
    for (int32_t i = 0; i < flags.u.tab.size; ++i) {
        const auto name = table_key(flags, i);
        const toml_datum_t datum = flags.u.tab.value[i];
        if (name == "mtu" && datum.type == TOML_INT64 &&
            datum.u.int64 >= kMinMtu && datum.u.int64 <= kMaxMtu) {
            out.mtu = static_cast<uint16_t>(datum.u.int64);
            continue;
        }
        // enable_encryption 采用实际取值（缺省 true）；enableEncryption 是上游接受的别名。
        if ((name == "enable_encryption" || name == "enableEncryption") &&
            datum.type == TOML_BOOLEAN) {
            out.enable_encryption = datum.u.boolean;
            continue;
        }
        // 与固定运行配置相同的声明无需警告；其余值（包括错误类型）均忽略。
        if (name == "disable_p2p" && datum.type == TOML_BOOLEAN && datum.u.boolean) continue;
        if (name == "encryption_algorithm" && datum.type == TOML_STRING &&
            datum_sv(datum) == "aes-gcm") continue;
        if (name == "data_compress_algo" && datum.type == TOML_STRING &&
            datum_sv(datum) == "None") continue;
        char path[192];
        add_diag(report, ConfigIssue::IgnoredParameter, datum,
                 join_key(path, sizeof(path), "flags.", name));
    }
}

/** @brief 按上游 DhcpConfig 语义提取布尔或 IPv4 CIDR；无法采用时保留默认 false。 */
void map_dhcp(ConfigReport* report, const toml_datum_t& top, StoredConfig& out) noexcept {
    const toml_datum_t datum = et_toml_get(top, "dhcp");
    if (datum.type == TOML_UNKNOWN) return;
    if (datum.type == TOML_BOOLEAN) {
        out.dhcp = datum.u.boolean;
        return;
    }
    if (datum.type == TOML_STRING) {
        uint32_t subnet = 0;
        uint8_t prefix = 0;
        bool had_prefix = false;
        if (parse_ipv4_cidr(datum_sv(datum), subnet, prefix, had_prefix) && had_prefix &&
            prefix <= 30) {
            out.dhcp = true;
            out.dhcp_has_subnet = true;
            out.dhcp_subnet = ipv4_network_of(subnet, prefix);
            out.dhcp_prefix = prefix;
            return;
        }
    }
    add_diag(report, ConfigIssue::IgnoredParameter, datum, "dhcp");
}

}  // namespace

/** @brief 清空调用方的配置报告（成功/失败路径都会先清空）。 */
void clear_config_report(ConfigReport* report) noexcept {
    if (report == nullptr) {
        return;
    }
    report->count = 0;
    report->omitted_count = 0;
    for (ConfigDiagnostic& d : report->items) {
        d = ConfigDiagnostic{};
    }
}

/**
 * @brief 深度与 dotted key 限额预扫描。
 * @details 解析状态机识别普通/基本字符串/字面字符串/多行字符串/注释，只在
 *          "代码区域"统计 [] {} 嵌套深度与当前 key 的点号分段数，用于把解析器
 *          的递归深度限制在可控范围。该扫描不承担 TOML 语法校验。
 */
bool toml_scan_limits(std::string_view text) noexcept {
    enum class State { Normal, Basic, Literal, MultilineBasic, MultilineLiteral, Comment };
    State state = State::Normal;
    size_t depth = 0;
    size_t segments = 1;
    bool in_key = true;

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        switch (state) {
            case State::Normal:
                if (c == '#') {
                    state = State::Comment;
                } else if (c == '"') {
                    if (i + 2 < text.size() && text[i + 1] == '"' && text[i + 2] == '"') {
                        state = State::MultilineBasic;
                        i += 2;
                    } else {
                        state = State::Basic;
                    }
                } else if (c == '\'') {
                    if (i + 2 < text.size() && text[i + 1] == '\'' && text[i + 2] == '\'') {
                        state = State::MultilineLiteral;
                        i += 2;
                    } else {
                        state = State::Literal;
                    }
                } else if (c == '[' || c == '{') {
                    if (++depth > kTomlMaxDepth) {
                        return false;
                    }
                } else if (c == ']' || c == '}') {
                    if (depth > 0) {
                        --depth;
                    }
                } else if (c == '\n') {
                    in_key = true;
                    segments = 1;
                } else if (c == '=') {
                    in_key = false;
                } else if (c == '.' && in_key) {
                    if (++segments > kTomlMaxKeySegments) {
                        return false;
                    }
                }
                break;
            case State::Basic:
                if (c == '\\') {
                    ++i;
                } else if (c == '"') {
                    state = State::Normal;
                }
                break;
            case State::Literal:
                if (c == '\'') {
                    state = State::Normal;
                }
                break;
            case State::MultilineBasic:
                if (c == '\\') {
                    ++i;
                } else if (c == '"' && i + 2 < text.size() && text[i + 1] == '"' &&
                           text[i + 2] == '"') {
                    state = State::Normal;
                    i += 2;
                }
                break;
            case State::MultilineLiteral:
                if (c == '\'' && i + 2 < text.size() && text[i + 1] == '\'' &&
                    text[i + 2] == '\'') {
                    state = State::Normal;
                    i += 2;
                }
                break;
            case State::Comment:
                if (c == '\n') {
                    state = State::Normal;
                    in_key = true;
                    segments = 1;
                }
                break;
        }
    }
    return true;
}

/**
 * @brief 解析并映射 TOML 文本。
 *
 * @details 处理顺序：清空报告 → 长度/深度限额 → 预算内解析 → 语法树结构校验 →
 *          根级未知键诊断 → 宽容逐字段提取 → dhcp/ipv4 优先级收敛 → 释放语法树。
 *          字段不能采用时只记录忽略；任何失败路径都会释放语法树并关闭预算标记。
 */
esp_err_t parse_toml_config(std::string_view text, StoredConfig& out,
                            ConfigReport* report) noexcept {
    clear_config_report(report);
    apply_config_defaults(out);

    // ESP-IDF EMBED_TXTFILES 的结束符属于容器而非 TOML；只移除末尾一个 NUL，
    // 文档内部 NUL 仍交给语法解析器拒绝，绝不按 strlen 截断隐藏后续内容。
    if (!text.empty() && text.back() == '\0') text.remove_suffix(1);
    if (text.empty()) return ESP_OK;  // 上游空文档使用默认配置
    if (text.size() > kTomlMaxBytes) {
        ET_LOGE(kTag, "config too large: %u bytes", static_cast<unsigned>(text.size()));
        add_diag(report, ConfigIssue::ResourceLimit, "toml");
        return ESP_ERR_NO_MEM;
    }
    if (!toml_scan_limits(text)) {
        add_diag(report, ConfigIssue::ResourceLimit, "toml");
        return ESP_ERR_NO_MEM;
    }

    ensure_parser_options();
    g_budget.limit = kTomlHeapBudget;
    g_budget.used = 0;
    g_budget.exceeded = false;
    g_budget.active = true;

    const toml_result_t result =
        et_toml_parse_named(text.data(), static_cast<int>(text.size()), "easytier.toml");

    g_budget.active = false;
    const bool budget_exceeded = g_budget.exceeded;
    const size_t budget_used = g_budget.used;

    if (!result.ok) {
        // 只提取位置，不回显解析器原始错误文本（其中可能带配置值或密钥）。
        int error_line = 0;
        (void)std::sscanf(result.errmsg, "(line %d)", &error_line);
        toml_datum_t location{};
        location.lineno = error_line;
        const bool resource_error = budget_exceeded ||
            std::strstr(result.errmsg, "out of memory") != nullptr ||
            std::strstr(result.errmsg, "too many key parts") != nullptr ||
            std::strstr(result.errmsg, "stack overflow") != nullptr;
        et_toml_free(result);
        add_diag(report, resource_error ? ConfigIssue::ResourceLimit : ConfigIssue::SyntaxError,
                 location, "toml");
        if (resource_error) {
            ET_LOGE(kTag, "TOML parser resource limit (heap budget %u bytes)",
                    static_cast<unsigned>(kTomlHeapBudget));
            return ESP_ERR_NO_MEM;
        }
        ET_LOGE(kTag, "TOML syntax error at line %u", static_cast<unsigned>(error_line));
        return ESP_ERR_INVALID_ARG;
    }

    TreeStats stats;
    count_tree(result.toptab, 0, stats);

    esp_err_t err = ESP_OK;
    if (!stats.ok) {
        add_diag(report, ConfigIssue::ResourceLimit, "toml");
        err = ESP_ERR_NO_MEM;
    }

    const toml_datum_t top = result.toptab;

    if (err == ESP_OK) {
        for (int32_t i = 0; i < top.u.tab.size && err == ESP_OK; ++i) {
            const std::string_view key = table_key(top, i);
            if (is_known_root_key(key)) {
                continue;
            }
            add_diag(report, ConfigIssue::IgnoredParameter, top.u.tab.value[i], key);
        }
    }

    if (err == ESP_OK) {
        auto value = optional_string(report, top, "instance_name");
        if (!value.empty() && !set_fixed_string(out.instance_name, value)) {
            add_diag(report, ConfigIssue::IgnoredParameter, "instance_name");
        }
        value = optional_string(report, top, "hostname");
        if (!value.empty()) {
            std::array<char, kMaxHostnameBytes> normalized{};
            const auto hostname = normalize_hostname(value, normalized);
            if (!hostname.empty()) (void)set_fixed_string(out.hostname, hostname);
        }
        value = optional_string(report, top, "instance_id");
        if (!value.empty()) {
            // 解析失败可能已经写入部分字节；只有完整有效时才提交。
            std::array<uint8_t, 16> parsed{};
            if (parse_uuid_text(value, parsed)) {
                out.instance_id = parsed;
                out.has_instance_id = true;
            } else add_diag(report, ConfigIssue::IgnoredParameter, "instance_id");
        }
        value = optional_string(report, top, "ipv4");
        if (!value.empty()) {
            uint32_t addr = 0;
            uint8_t prefix = 24;
            bool had_prefix = false;
            if (!parse_ipv4_cidr(value, addr, prefix, had_prefix)) {
                add_diag(report, ConfigIssue::IgnoredParameter, "ipv4");
            } else {
                out.has_static_ipv4 = true;
                out.static_ipv4 = addr;
                out.static_prefix = (had_prefix && prefix != 32) ? prefix : 24;
                if (had_prefix && prefix == 32) {
                    add_diag(report, ConfigIssue::IgnoredParameter, "ipv4.prefix");
                }
            }
        }
        map_network_identity(report, top, out);
        map_flags(report, top, out);
        map_dhcp(report, top, out);
        err = map_peers(report, top, out);
        if (out.has_static_ipv4 && out.dhcp) {
            add_diag(report, ConfigIssue::IgnoredParameter, "dhcp");
            out.dhcp = false;
            out.dhcp_has_subnet = false;
        }
    }

    et_toml_free(result);

    if (err != ESP_OK) {
        return err;
    }
    (void)budget_used;
    ET_LOGD(kTag, "TOML parsed, heap %u bytes", static_cast<unsigned>(budget_used));
    // 与上游空配置/空 peer 列表语义一致：解析成功不代表网络已具备可用路由。
    // 编程式 Config 入口仍保留严格校验；TOML 入口按逐字段提取契约返回成功。
    return ESP_OK;
}

}  // namespace et
