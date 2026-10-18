/**
 * @file toml.cpp
 * @brief TOML 配置映射实现（基于 vendored tomlc17）。
 *
 * @details 使用 tomlc17 默认分配器；解析前后做嵌套深度与语法树结构校验。字段策略
 *          与上游一致：支持字段写错直接失败，不支持字段不解析也不报错，不缓存诊断。
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

/**
 * @brief 严格读取可选字符串字段。
 * @details 字段未提供 → 保持缺省值；字段存在但类型不是字符串（或含内嵌 NUL）→ 报错。
 *          与上游一致：**支持的字段必须写对**，不支持的字段根本不解析。
 * @return @c ESP_OK，或 @c ESP_ERR_INVALID_ARG
 */
esp_err_t read_optional_string(const toml_datum_t& top, const char* key,
                               std::string_view& out) noexcept {
    const toml_datum_t datum = et_toml_get(top, key);
    if (datum.type == TOML_UNKNOWN) return ESP_OK;  // 未提供
    if (datum.type != TOML_STRING || datum_sv(datum).find('\0') != std::string_view::npos) {
        ET_LOGE(kTag, "config field '%s' must be a string", key);
        return ESP_ERR_INVALID_ARG;
    }
    out = datum_sv(datum);
    return ESP_OK;
}

/**
 * @brief 提取网络身份（上游 `network_identity` 表）。
 * @details 与上游 `NetworkIdentity` 的 serde 定义一致：表一旦出现，`network_name` 就是
 *          必填字符串（缺失即报错）；`network_secret` 可选。表内其它键（上游字段或未知键）
 *          不解析、不报错。
 */
esp_err_t map_network_identity(const toml_datum_t& top, Settings& out) noexcept {
    const toml_datum_t table = et_toml_get(top, "network_identity");
    if (table.type == TOML_UNKNOWN) return ESP_OK;
    if (table.type != TOML_TABLE) {
        ET_LOGE(kTag, "config field 'network_identity' must be a table");
        return ESP_ERR_INVALID_ARG;
    }
    bool has_name = false;
    for (int32_t i = 0; i < table.u.tab.size; ++i) {
        const std::string_view name = table_key(table, i);
        const toml_datum_t datum = table.u.tab.value[i];
        const std::string_view value = datum_sv(datum);
        if (name != "network_name" && name != "network_secret") {
            continue;  // 不支持的字段：不解析、不报错
        }
        if (datum.type != TOML_STRING || value.find('\0') != std::string_view::npos) {
            ET_LOGE(kTag, "config field 'network_identity.%.*s' must be a string",
                    static_cast<int>(name.size()), name.data());
            return ESP_ERR_INVALID_ARG;
        }
        if (name == "network_name") {
            if (value.empty()) {
                ET_LOGE(kTag, "config field 'network_identity.network_name' must not be empty");
                return ESP_ERR_INVALID_ARG;
            }
            out.network_name = value;
            has_name = true;
        } else {
            out.network_secret = value;
        }
    }
    if (!has_name) {
        // 上游 NetworkIdentity::network_name 是必填字段（serde 会报 missing field）。
        ET_LOGE(kTag, "config field 'network_identity.network_name' is required");
        return ESP_ERR_INVALID_ARG;
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
        // 入口表里除 uri 之外的键（上游字段或未知键）不解析、不报错。
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

/**
 * @brief 提取 `flags` 表里组件支持的字段。
 * @details 只解析 `mtu`（576..1380 整数）与 `enable_encryption`（布尔，含上游别名
 *          `enableEncryption`）；两者若给出但类型/取值不对即报错。表内其它标志
 *          （`disable_p2p`、`encryption_algorithm`、`data_compress_algo` 等）不解析、不报错。
 */
esp_err_t map_flags(const toml_datum_t& top, Settings& out) noexcept {
    const toml_datum_t flags = et_toml_get(top, "flags");
    if (flags.type == TOML_UNKNOWN) return ESP_OK;
    if (flags.type != TOML_TABLE) {
        ET_LOGE(kTag, "config field 'flags' must be a table");
        return ESP_ERR_INVALID_ARG;
    }
    for (int32_t i = 0; i < flags.u.tab.size; ++i) {
        const auto name = table_key(flags, i);
        const toml_datum_t datum = flags.u.tab.value[i];
        if (name == "mtu") {
            if (datum.type != TOML_INT64 || datum.u.int64 < kMinMtu ||
                datum.u.int64 > kDefaultMtu) {
                ET_LOGE(kTag, "config field 'flags.mtu' must be an integer in [%u,%u]",
                        static_cast<unsigned>(kMinMtu), static_cast<unsigned>(kDefaultMtu));
                return ESP_ERR_INVALID_ARG;
            }
            out.mtu = static_cast<uint16_t>(datum.u.int64);
            continue;
        }
        // enable_encryption 采用实际取值（缺省 true）；enableEncryption 是上游接受的别名。
        if (name == "enable_encryption" || name == "enableEncryption") {
            if (datum.type != TOML_BOOLEAN) {
                ET_LOGE(kTag, "config field 'flags.%.*s' must be a boolean",
                        static_cast<int>(name.size()), name.data());
                return ESP_ERR_INVALID_ARG;
            }
            out.enable_encryption = datum.u.boolean;
            continue;
        }
        // 其余标志（上游字段或未知键）不解析、不报错。
    }
    return ESP_OK;
}

/**
 * @brief 按上游 `DhcpConfig` 语义提取布尔或 IPv4 CIDR 字符串。
 * @details 给出但类型不是布尔/字符串，或 CIDR 非法、前缀不在 0..30，均视为错误。
 */
esp_err_t map_dhcp(const toml_datum_t& top, Settings& out) noexcept {
    const toml_datum_t datum = et_toml_get(top, "dhcp");
    if (datum.type == TOML_UNKNOWN) return ESP_OK;
    if (datum.type == TOML_BOOLEAN) {
        out.dhcp = datum.u.boolean;
        return ESP_OK;
    }
    if (datum.type == TOML_STRING) {
        uint32_t subnet = 0;
        uint8_t prefix = 0;
        bool had_prefix = false;
        if (!parse_ipv4_cidr(datum_sv(datum), subnet, prefix, had_prefix) || !had_prefix) {
            ET_LOGE(kTag, "config field 'dhcp' must be a boolean or an IPv4 CIDR string");
            return ESP_ERR_INVALID_ARG;
        }
        if (prefix > 30) {
            ET_LOGE(kTag, "config field 'dhcp' prefix must be in [0,30]");
            return ESP_ERR_INVALID_ARG;
        }
        out.dhcp = true;
        out.dhcp_has_subnet = true;
        out.dhcp_subnet = ipv4_network_of(subnet, prefix);
        out.dhcp_prefix = prefix;
        return ESP_OK;
    }
    ET_LOGE(kTag, "config field 'dhcp' must be a boolean or an IPv4 CIDR string");
    return ESP_ERR_INVALID_ARG;
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
 * @details 处理顺序：深度限额 → 解析 → 语法树结构校验 → 逐字段提取（根级未知键与未实现
 *          的字段直接跳过）→ dhcp/ipv4 优先级收敛 → 释放语法树。调用方（@ref et::Inst 的
 *          构造函数）总是从缺省构造的 @ref Settings 开始，因此这里等价于"缺省值 + TOML"。
 *          **不缓存诊断**：真正失败（语法错误、入口非法、资源超限）用 Error 日志记录并
 *          返回错误码；不支持的字段不解析也不报错。
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

    // 根级未知键（以及组件不支持的上游字段，如 listeners/routes/proxy_network 等）
    // 一律不解析、不报错——与上游 serde 忽略未知字段的行为一致。

    if (err == ESP_OK) {
        std::string_view value{};
        err = read_optional_string(top, "instance_name", value);
        if (err == ESP_OK && !value.empty()) {
            out.instance_name = value;
            // 显式给出的实例名优先于"取 hostname"的默认推导。
            out.instance_name_explicit = true;
        }
        value = {};
        if (err == ESP_OK) err = read_optional_string(top, "hostname", value);
        if (err == ESP_OK && !value.empty()) {
            normalize_hostname(value, out.hostname);
            // 与上游一致：规范化后为空（例如全是控制字符）视为"未提供"，回退缺省 Espressif。
            if (out.hostname.empty()) {
                ET_LOGD(kTag, "config field 'hostname' has no usable characters; using default");
            }
        }
        value = {};
        if (err == ESP_OK) err = read_optional_string(top, "instance_id", value);
        if (err == ESP_OK && !value.empty()) {
            // 解析失败可能已经写入部分字节；只有完整有效时才提交。
            std::array<uint8_t, 16> parsed{};
            if (!parse_uuid_text(value, parsed)) {
                ET_LOGE(kTag, "config field 'instance_id' must be a UUID string");
                err = ESP_ERR_INVALID_ARG;
            } else {
                out.instance_id = parsed;
                out.has_instance_id = true;
            }
        }
        value = {};
        if (err == ESP_OK) err = read_optional_string(top, "ipv4", value);
        if (err == ESP_OK && !value.empty()) {
            uint32_t addr = 0;
            uint8_t prefix = 24;
            bool had_prefix = false;
            if (parse_ipv4_cidr(value, addr, prefix, had_prefix)) {
                out.has_static_ipv4 = true;
                out.static_ipv4 = addr;
                // 与上游一致：裸地址或 /32 归一化为 /24。
                out.static_prefix = (had_prefix && prefix != 32) ? prefix : 24;
            }
            // 与上游 `get_ipv4()` 的 `.parse().ok()` 一致：字符串类型对但内容无法解析时
            // **静默忽略**（不报错、不设置静态地址）。
        }
        if (err == ESP_OK) {
            err = map_network_identity(top, out);
        }
        if (err == ESP_OK) {
            err = map_flags(top, out);
        }
        if (err == ESP_OK) {
            err = map_dhcp(top, out);
        }
        if (err == ESP_OK) {
            err = map_peers(top, out);
        }
        if (out.has_static_ipv4 && out.dhcp) {
            // 静态地址优先：上游同样是配置了 ipv4 就忽略 dhcp。
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
