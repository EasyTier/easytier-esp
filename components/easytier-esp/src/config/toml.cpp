/**
 * @file toml.cpp
 * @brief TOML 配置映射实现（基于 vendored tomlc17）。
 *
 * @details 关键点：
 * - 使用 tomlc17 默认分配器（不做堆预算），分配失败由解析器以 "out of memory" 报错；
 * - 解析前后分别做嵌套深度预扫描与语法树结构校验，拒绝异常放大；
 * - 支持字段与上游同名同义，未实现字段用 Warn 日志"忽略"说明，不因未知内容拒绝配置；
 *   真正的失败（语法错误、入口非法、资源超限）用 Error 日志 + 返回错误码，**不缓存诊断**。
 */
#include "config/toml.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "support/config_limits.hpp"
#include "support/log.hpp"

extern "C" {
#include "tomlc17.h"
}

namespace et {
namespace {

constexpr const char* kTag = "ET.TOML";

/**
 * @brief 解析串行化标记。
 * @note tomlc17 的 `toml_option`（UTF-8 校验 / 分配器）是**进程级**全局状态，首次
 *       安装必须串行；组件也按"Inst 构造串行"的约定，同一时刻只允许一次解析，
 *       有解析进行中时直接拒绝第二次调用。
 */
std::atomic_flag g_parse_busy;

/** @brief 离开作用域时释放解析串行化标记（覆盖所有返回路径）。 */
struct ParseBusyGuard {
    ~ParseBusyGuard() noexcept { g_parse_busy.clear(); }
};

/**
 * @brief 首次加载时安装解析器全局选项。
 * @details 只启用 UTF-8 校验，分配器沿用 tomlc17 默认的 `realloc`/`free`；分配失败由
 *          解析器自身以 "out of memory" 报错，再由调用方映射为 `ESP_ERR_NO_MEM`。
 */
void ensure_parser_options() noexcept {
    static bool installed = false;
    if (installed) {
        return;
    }
    toml_option_t opt = et_toml_default_option();
    opt.check_utf8 = true;
    et_toml_set_option(opt);
    installed = true;
}

/**
 * @brief 记录一条被忽略的配置项（Warn 级，只记录字段路径，绝不记录取值）。
 * @param prefix 字段路径前缀（如 "flags."、"network_identity."，可为空）
 * @param name 字段名
 * @param at 该字段在 TOML 中的位置（用于行号；0 表示未知）
 */
void log_ignored(std::string_view prefix, std::string_view name,
                 const toml_datum_t& at) noexcept {
    if (at.lineno > 0) {
        ET_LOGW(kTag, "ignored config: %.*s%.*s (line %d)",
                static_cast<int>(prefix.size()), prefix.data(),
                static_cast<int>(name.size()), name.data(), at.lineno);
    } else {
        ET_LOGW(kTag, "ignored config: %.*s%.*s", static_cast<int>(prefix.size()),
                prefix.data(), static_cast<int>(name.size()), name.data());
    }
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
 * @brief 宽容读取可选字符串：类型不匹配只记日志忽略，绝不覆盖默认值。
 * @note 字段路径无法表示内嵌 NUL；这类合法 TOML 字符串也按不可采用处理。
 */
std::string_view optional_string(const toml_datum_t& top, const char* key) noexcept {
    const toml_datum_t datum = et_toml_get(top, key);
    if (datum.type == TOML_UNKNOWN) return {};
    if (datum.type != TOML_STRING || datum_sv(datum).find('\0') != std::string_view::npos) {
        log_ignored({}, key, datum);
        return {};
    }
    return datum_sv(datum);
}

/** @brief 按上游字段名提取网络身份；缺失或不能采用的字段分别保留默认值。 */
esp_err_t map_network_identity(const toml_datum_t& top, Settings& out) noexcept {
    const toml_datum_t table = et_toml_get(top, "network_identity");
    if (table.type == TOML_UNKNOWN) return ESP_OK;
    if (table.type != TOML_TABLE) {
        log_ignored({}, "network_identity", table);
        return ESP_OK;
    }
    for (int32_t i = 0; i < table.u.tab.size; ++i) {
        const std::string_view name = table_key(table, i);
        const toml_datum_t datum = table.u.tab.value[i];
        const std::string_view value = datum_sv(datum);
        bool adopted = false;
        if (datum.type == TOML_STRING && value.find('\0') == std::string_view::npos) {
            if (name == "network_name" && !value.empty()) {
                out.network_name = value;
                adopted = true;
            } else if (name == "network_secret") {
                out.network_secret = value;
                adopted = true;
            }
        }
        if (!adopted) log_ignored("network_identity.", name, datum);
    }
    return ESP_OK;
}

/** @brief 严格读取 TCP 入口；条目数量无固定上限，错误或不支持的取值仍拒绝加载。 */
esp_err_t map_peers(const toml_datum_t& top, Settings& out) noexcept {
    const toml_datum_t array = et_toml_get(top, "peer");
    if (array.type == TOML_UNKNOWN) return ESP_OK;
    if (array.type != TOML_ARRAY) {
        ET_LOGE(kTag, "peer must be an array of tables");
        return ESP_ERR_INVALID_ARG;
    }
    // 入口数量不设固定上限：按数组实际长度一次预留到堆上（std::vector 分配失败即终止）。
    if (array.u.arr.size > 0) {
        out.peers.reserve(static_cast<size_t>(array.u.arr.size));
    }
    for (int32_t i = 0; i < array.u.arr.size; ++i) {
        const toml_datum_t entry = array.u.arr.elem[i];
        if (entry.type != TOML_TABLE) {
            ET_LOGE(kTag, "peer[%d] must be a table", static_cast<int>(i));
            return ESP_ERR_INVALID_ARG;
        }
        for (int32_t k = 0; k < entry.u.tab.size; ++k) {
            const auto name = table_key(entry, k);
            if (name == "uri") continue;
            log_ignored("peer.", name, entry.u.tab.value[k]);
        }
        const toml_datum_t uri = et_toml_get(entry, "uri");
        std::string_view host{};
        uint16_t port = 0;
        if (uri.type != TOML_STRING || datum_sv(uri).find('\0') != std::string_view::npos ||
            !parse_peer_endpoint(datum_sv(uri), host, port)) {
            ET_LOGE(kTag, "peer[%d].uri is missing or not a supported tcp:// endpoint",
                    static_cast<int>(i));
            return ESP_ERR_INVALID_ARG;
        }
        PeerEndpoint endpoint{};
        endpoint.host = host;
        endpoint.port = port;
        out.peers.push_back(std::move(endpoint));
    }
    return ESP_OK;
}

/** @brief 仅采用有效 MTU；不支持的 flags 无论取值类型如何都只忽略。 */
void map_flags(const toml_datum_t& top, Settings& out) noexcept {
    const toml_datum_t flags = et_toml_get(top, "flags");
    if (flags.type == TOML_UNKNOWN) return;
    if (flags.type != TOML_TABLE) {
        log_ignored({}, "flags", flags);
        return;
    }
    for (int32_t i = 0; i < flags.u.tab.size; ++i) {
        const auto name = table_key(flags, i);
        const toml_datum_t datum = flags.u.tab.value[i];
        if (name == "mtu" && datum.type == TOML_INT64 &&
            datum.u.int64 >= kMinMtu && datum.u.int64 <= kDefaultMtu) {
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
        log_ignored("flags.", name, datum);
    }
}

/** @brief 按上游 DhcpConfig 语义提取布尔或 IPv4 CIDR；无法采用时保留默认 false。 */
void map_dhcp(const toml_datum_t& top, Settings& out) noexcept {
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
    log_ignored({}, "dhcp", datum);
}

}  // namespace

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
 * @details 处理顺序：深度限额 → 解析 → 语法树结构校验 → 根级未知键日志 → 宽容逐字段提取
 *          → dhcp/ipv4 优先级收敛 → 释放语法树。调用方（@ref et::Inst 的构造函数）总是从
 *          缺省构造的 @ref Settings 开始，因此这里等价于"缺省值 + TOML"。
 *          **不缓存诊断**：不能采用的非 peer 字段用 Warn 日志记录，真正失败（语法错误、
 *          入口非法、资源超限）用 Error 日志记录并返回错误码。
 */
esp_err_t parse_toml_into(Settings& out, std::string_view text) noexcept {
    // ESP-IDF EMBED_TXTFILES 的结束符属于容器而非 TOML；只移除末尾一个 NUL，
    // 文档内部 NUL 仍交给语法解析器拒绝，绝不按 strlen 截断隐藏后续内容。
    if (!text.empty() && text.back() == '\0') text.remove_suffix(1);
    if (text.empty()) return ESP_OK;  // 上游空文档使用默认配置
    if (!toml_scan_limits(text)) {
        ET_LOGE(kTag, "TOML nesting or dotted-key depth exceeds the limit");
        return ESP_ERR_NO_MEM;
    }

    // tomlc17 的全局选项是进程级状态：已有解析进行中时直接拒绝，保证首次安装串行，
    // 也符合"Inst 构造串行"的约定（多实例场景下不应从两个任务并发构造）。
    if (g_parse_busy.test_and_set()) {
        ET_LOGE(kTag, "another TOML parse is in progress; Inst construction must be serialized");
        return ESP_ERR_INVALID_STATE;
    }
    const ParseBusyGuard busy_guard{};
    ensure_parser_options();

    const toml_result_t result =
        et_toml_parse_named(text.data(), static_cast<int>(text.size()), "easytier.toml");

    if (!result.ok) {
        // 只提取位置，不回显解析器原始错误文本（其中可能带配置值或密钥）。
        int error_line = 0;
        (void)std::sscanf(result.errmsg, "(line %d)", &error_line);
        const bool resource_error =
            std::strstr(result.errmsg, "out of memory") != nullptr ||
            std::strstr(result.errmsg, "too many key parts") != nullptr ||
            std::strstr(result.errmsg, "stack overflow") != nullptr;
        et_toml_free(result);
        if (resource_error) {
            ET_LOGE(kTag, "TOML parser out of resource (allocation or nesting limits)");
            return ESP_ERR_NO_MEM;
        }
        ET_LOGE(kTag, "TOML syntax error at line %u", static_cast<unsigned>(error_line));
        return ESP_ERR_INVALID_ARG;
    }

    TreeStats stats;
    count_tree(result.toptab, 0, stats);

    esp_err_t err = ESP_OK;
    if (!stats.ok) {
        ET_LOGE(kTag, "TOML syntax tree exceeds the size limits");
        err = ESP_ERR_NO_MEM;
    }

    const toml_datum_t top = result.toptab;

    if (err == ESP_OK) {
        for (int32_t i = 0; i < top.u.tab.size && err == ESP_OK; ++i) {
            const std::string_view key = table_key(top, i);
            if (is_known_root_key(key)) {
                continue;
            }
            log_ignored({}, key, top.u.tab.value[i]);
        }
    }

    if (err == ESP_OK) {
        auto value = optional_string(top, "instance_name");
        if (!value.empty()) {
            out.instance_name = value;
            // 显式给出的实例名优先于"取 hostname"的默认推导。
            out.instance_name_explicit = true;
        }
        value = optional_string(top, "hostname");
        if (!value.empty()) {
            normalize_hostname(value, out.hostname);
            if (out.hostname.empty()) {
                ET_LOGW(kTag, "ignored config: hostname (empty after normalization)");
            }
        }
        value = optional_string(top, "instance_id");
        if (!value.empty()) {
            // 解析失败可能已经写入部分字节；只有完整有效时才提交。
            std::array<uint8_t, 16> parsed{};
            if (parse_uuid_text(value, parsed)) {
                out.instance_id = parsed;
                out.has_instance_id = true;
            } else {
                ET_LOGW(kTag, "ignored config: instance_id (not a valid UUID)");
            }
        }
        value = optional_string(top, "ipv4");
        if (!value.empty()) {
            uint32_t addr = 0;
            uint8_t prefix = 24;
            bool had_prefix = false;
            if (!parse_ipv4_cidr(value, addr, prefix, had_prefix)) {
                ET_LOGW(kTag, "ignored config: ipv4 (not a valid IPv4/CIDR)");
            } else {
                out.has_static_ipv4 = true;
                out.static_ipv4 = addr;
                out.static_prefix = (had_prefix && prefix != 32) ? prefix : 24;
                if (had_prefix && prefix == 32) {
                    ET_LOGW(kTag, "ignored config: ipv4.prefix (/32 normalized to /24)");
                }
            }
        }
        if (err == ESP_OK) {
            err = map_network_identity(top, out);
        }
        if (err == ESP_OK) {
            map_flags(top, out);
            map_dhcp(top, out);
            err = map_peers(top, out);
        }
        if (out.has_static_ipv4 && out.dhcp) {
            ET_LOGW(kTag, "ignored config: dhcp (ipv4 is configured, static address wins)");
            out.dhcp = false;
            out.dhcp_has_subnet = false;
        }
    }

    et_toml_free(result);

    if (err != ESP_OK) {
        return err;
    }
    ET_LOGD(kTag, "TOML parsed");
    // 与上游空配置/空 peer 列表语义一致：解析成功不代表网络已具备可用路由。
    // TOML 入口按逐字段提取契约返回成功；结构完整性由 et::Inst 构造时校验。
    return ESP_OK;
}

}  // namespace et
