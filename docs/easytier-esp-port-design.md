# EasyTier-ESP 移植设计

状态：设计已批准，实施进行中（M0 完成，M1 传输与握手已实现并通过宿主测试；详见第 14 节）。

## 1. 目标与已确定约束

EasyTier-ESP 封装为独立 ESP-IDF 组件 `components/easytier-esp`，公开接口与组件自身实现统一采用 C++20，命名空间为 `et`，实例类名为 **`Instance`**。公开接口不提供 C 包装层。`main` 只演示初始化物理网络、连接 WiFi、加载配置和启动组网。

首版仅适配 **ESP-IDF 6.1.x**，组件和示例依赖范围为 `>=6.1,<6.2`，硬件验收使用当前 ESP32-S3。旧版本 IDF 兼容与其他芯片验证后续单独推进，不加入预先兼容分支。

必须能够直接读取与 EasyTier 上游核心相同格式的 **TOML 配置文件**，也支持读取 TOML 文本。不兼容参数忽略并报告，不能因为配置包含未实现的功能而整体拒绝读取。配置格式兼容不意味着实现所有上游能力；支持字段的语义、忽略项和最终生效设置必须明确。

**TOML 加载策略修订（以本条为准）**：按上游 `easytier-core/src/config/toml.rs`
的字段名、层级与缺省值提取本组件支持的内容；按用户要求进一步宽容：未知/无关字段、
非 peer 的不支持功能、错误类型、无效值、字段超容量均忽略，不阻断其他有效字段。
**peer 只能写对**：无效/不支持的 URI、缺 uri、错误类型或超过 10 个入口均报错；
peer 内无关附加字段可忽略。TOML 与编程式配置共用严格入口校验。空文档、空文件、无入口/无地址都允许加载，但不保证组网成功。
语法错误、peer 配置错误、读取失败、解析资源耗尽会返回错误。编程式 `init(Config)` 保留严格校验。
修复 `EMBED_TXTFILES` 自动追加末尾 NUL 被计入长度而误报语法错的问题；文档内部
NUL 仍非法。新增嵌入文本、逐字段容错、空配置和混合入口回归测试，示例测试不再写死
用户的网络身份和入口。旧章节关于 TOML 字段错误/缺项拒绝的描述由本条替代。

**真机反馈修订**：虚拟地址分配后 `netif_add` 触发 IDF 全局 DHCP 回调崩溃，
根因是 raw 网卡把 `VirtualNetif*` 存进 IDF 保留语义的 `netif->state`。现将其保持
为空，通过首成员布局恢复对象；设备生命周期/改址测试断言该关系。单入口不再填满
两个连接槽；同一 host:port 不重复连接；重试退避改为每槽截止时间，避免全 I/O 任务等待。
缺省 hostname 为用户指定的 ESP Host 回退名 `Espressif`，缺省 instance_id 对齐上游
`get_id()`，在初始化时生成并缓存随机 UUID v4，显式 nil UUID 不被覆盖。其他支持字段
默认值见组件 README 的默认值表；host/default 配置与真机验收状态分开记录。

首版完成 TCP 组网：网络入口、明文握手、SipHash 派生、AES-128-GCM、Ping/Pong、双向 RPC 和分片、OSPF 路由同步、IPv4 虚拟网卡(支持DHCP和静态设置)、经入口中继的双向通信。UDP mux 与 NAT 打洞分阶段加入。

不纳入首版：共享公共中继/foreign network、Noise/credential、IPv6 及公网地址租约/集中式地址分配服务、子网代理、出口节点、Magic DNS、KCP/QUIC、出站广播/组播、管理界面。虚拟 IPv4 支持静态配置与 dhcp 自分配（见 4.3），但不实现对外提供地址分配服务。公网入口必须是同网络、相同密钥的种子节点，不能泛指任意 EasyTier 公共中继。

## 2. 依据与工程现状

已读取用户附件 [esp32-port-design.md](</home/myqfeng/.dsh/attachments/v1/files/e3/e3d490dff1e1fb1b80d755e402d6b910529faec91480fa75a30bf6cfbe6896e6/esp32-port-design.md>)，并核对本地 EasyTier 源码。协议基线锁定提交 `32dc7f1f406e5fe43ba095a918a7a1c4b28f2335`，版本描述 `v2.7.0-24-g32dc7f1f`。

当前 [公开头文件](../components/easytier-esp/include/easytier-esp.h) 和 [组件实现](../components/easytier-esp/easytier-esp.cpp) 只有占位内容，[main.cpp](../main/main.cpp) 是 Hello World；[构建记录](../build/project_description.json) 显示 IDF 6.1、ESP32-S3。工作区已有修改在实施时保留，不以清理或覆盖工程作为前提。

核心上游依据：

| 内容 | 源码 |
|---|---|
| TOML 字段与读取行为 | [config/toml.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/config/toml.rs:396>) |
| 网络身份默认值与摘要 | [config/mod.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/config/mod.rs:237>) |
| 报文类型和布局 | [packet/mod.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/packet/mod.rs>) |
| TCP 分帧 | [tunnel/framed.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/tunnel/framed.rs:61>) |
| 握手 | [peer_conn.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/peers/conn/peer_conn.rs:1191>) |
| Ping | [peer_conn_ping.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/peers/conn/peer_conn_ping.rs:179>) |
| AES | [aes_gcm.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/tunnel/encrypt/aes_gcm.rs>) |
| RPC 分片 | [rpc/packet.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/rpc/packet.rs:252>) |
| protobuf | [peer_rpc.proto](</home/myqfeng/Desktop/EasyTier/easytier-proto/proto/peer_rpc.proto>)、[common.proto](</home/myqfeng/Desktop/EasyTier/easytier-proto/proto/common.proto>) |
| 路由会话 | [peer_ospf_route.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/peers/route/peer_ospf_route.rs:2203>) |
| 转发 | [peer_manager.rs](</home/myqfeng/Desktop/EasyTier/easytier-core/src/peers/peer_manager.rs:3033>) |

## 3. 组件边界与架构选择

组件负责协议、连接、加密、RPC、路由、虚拟网卡、任务、内存池、配置解析、诊断及停止/恢复。应用负责 NVS、esp-netif、默认事件循环、WiFi/以太网、文件系统挂载、凭据来源和业务逻辑。

组件不初始化 WiFi，不挂载文件系统，不引用 `main` 的头文件或全局变量，不改变物理默认网卡。TOML 配置不包含 ESP WiFi 密码；WiFi 示例配置与 overlay 配置分开。

| 方案 | 收益 | 代价与决定 |
|---|---|---|
| C++20 子集组件，I/O/控制/数据三个任务 | 依赖较少、资源可界定、socket 生命周期单一所有者 | 推荐；用黄金样本和互操作验证维护兼容 |
| 附件 RX/TX/PROTO/DATA 四任务 | 收发任务分离 | fd 关闭与复用需要额外握手，首版不采用 |
| 直接移植 Rust 核心 | 更多实现可复用 | Tokio、系统接口与依赖裁剪范围大，首版不采用 |

内部自有模块为 C++20；若采用 C 实现的第三方 TOML 库，仅在私有包装层使用，不改变公开 C++ API。`app_main` 使用 `extern "C"`，SDK 回调/适配入口遵循实际 ABI；普通类方法和内部接口不使用 C 链接。

## 4. 上游 TOML 配置兼容

### 4.1 输入示例

采用上游核心的根字段、`[network_identity]`、`[[peer]]` 和 `[flags]`，不发明 `peers = [...]`、根级 `network = ...` 等替代格式。以下内容用于后续新增示例配置 `main/easytier.toml`：

```toml
instance_name = "esp32-demo"
hostname = "esp32-s3"
ipv4 = "10.144.144.10/24"
dhcp = false

# 上游参数可以保留；首版不创建监听端口。
listeners = ["tcp://0.0.0.0:11010", "udp://0.0.0.0:11010"]

[network_identity]
network_name = "esp-demo"
network_secret = "replace-with-your-network-secret"

[[peer]]
uri = "tcp://192.168.1.20:11010"

[flags]
mtu = 1380
enable_encryption = true
encryption_algorithm = "aes-gcm"
data_compress_algo = "None"
disable_p2p = true
```

入口地址仅为示例。实际入口与本节点必须有相同网络名、密钥，并使用首版支持的非 Noise、AES-128-GCM、无数据压缩配置。虚拟前缀不得与物理 LAN 重叠。

### 4.2 三种配置处理结果

1. **支持并应用**：字段类型和值合法时采用该设置。
2. **未知/不兼容并忽略**：合法 TOML 中未实现的字段、整张表、协议或取值忽略，产生不含敏感值的诊断；保留组件支持的默认行为。
3. **无法形成有效配置**：TOML 语法错误、已支持字段类型错误或无效值、容量超限，或者忽略后没有静态 IPv4/可用入口，初始化失败并回滚。

例如 `flags.enable_ipv6=true`、`secure_mode.enabled=true`、`flags.encryption_algorithm="chacha20"`、`flags.enable_encryption=false`、`flags.data_compress_algo="Zstd"` 在首版均属于不兼容设置，忽略并报告，生效 profile 仍为 IPv4、非 Noise、AES-128-GCM、加密开启、无数据压缩。**不会仅因这些参数出现而拒绝解析。** 如果其他节点实际要求被忽略的能力，后续握手或通信可能失败，状态中给出原因；解析成功不等于整个配置对应的网络可互通。

未知表内部不做业务类型验证，合法 TOML 就可忽略。例如未来新增的嵌套配置和数组表，不应因为组件不认识而失败。重复 TOML key、未闭合字符串等语法问题仍然失败。

### 4.3 首版字段映射

| 上游字段 | 首版处理及默认 |
|---|---|
| `instance_name` | 支持；缺省 `default`，用于日志/状态识别 |
| `hostname` | 支持；缺省空串，按上游去除控制字符并最多32个Unicode字符，UTF-8输出最多128字节 |
| `instance_id` | 支持合法UUID；缺省随机UUID，供路由实例身份使用，不等同于peer ID |
| `ipv4` | 支持IPv4字符串及CIDR；裸地址按上游使用/24；上游该提交对/32也归一化为/24，首版保持这一行为 |
| `network_identity` 整表缺省 | 与上游相同：网络名`default`、密钥为空字符串 |
| `network_identity.network_name` | 表存在时必须是字符串且存在；长度上限128 UTF-8字节 |
| `network_identity.network_secret` | 支持字符串含空串，上限256 UTF-8字节；缺省按上游普通模式归一化为空字符串，与Config默认值一致 |
| `[[peer]].uri` | 首版采用`tcp://IPv4-or-hostname:port`，过滤合法但未支持的URL scheme和IPv6入口并报告；支持项最多10个，超限失败，不静默丢弃支持项 |
| `[[peer]].peer_public_key` | 忽略，首版不支持Noise/credential |
| `flags.mtu` | 支持整数576～1380，缺省1380；非整数/越界失败 |
| `flags.enable_encryption` | true支持；false不兼容，忽略并继续使用true，报告忽略项 |
| `flags.encryption_algorithm` | 空串或`aes-gcm`支持；其他取值忽略，使用`aes-gcm` |
| `flags.data_compress_algo` | `None`支持；其他算法忽略，使用None |
| `flags.disable_p2p` | true支持；false忽略，首版实际禁用自动P2P |
| `dhcp` | 需要实现；接受布尔或IPv4 CIDR字符串（前缀0..=30）。提供有效`ipv4`时忽略该项；两者都没有时初始化失败 |
| `listeners`、`mapped_listeners`、`routes`、`proxy_network`、`port_forward`、`exit_nodes` | 整体忽略并报告 |
| `secure_mode`、`credential_file`、`managed_credentials`、`acl`、`vpn_portal_config` | 整体忽略并报告；缺省密钥使用空字符串初始化普通身份，不获得credential能力，原网络若要求credential将在连接阶段失败 |
| `ipv6`、STUN服务器、日志文件、网络命名空间、RPC portal、其余flags及未知字段 | 忽略；日志使用ESP-IDF自身配置 |

已支持的schema字段若类型错误，不使用隐式字符串/整数转换。合法但未知的算法字符串属于忽略项；例如MTU为`"large"`则属于支持字段类型错误。`[[peer]]`必须能读取合法字符串URI，明显格式错误报告字段错误；已识别但未支持的scheme过滤。

`flags.no_tun`、`disable_relay_data`、`latency_first`等未实现设置同样忽略，不宣称其语义已生效。资源限制属于设备能力约束，不作为可以忽略的配置参数。

### 4.4 解析库与资源

不使用逐行正则/手工切字符串解析TOML，避免字符串中的`#`、多行字符串、数组表、inline table、dotted/quoted key和转义被误读。

推荐私有引入MIT许可的 **tomlc17**，固定提交 `fba134d7d1f9b58414f908600b2869f7e55ff2c4`，保留源文件与LICENSE，构建时不自动下载。其[项目说明](https://github.com/cktan/tomlc17/blob/fba134d7d1f9b58414f908600b2869f7e55ff2c4/README.md)提供C/C++调用和标准TOML解析；[分配器选项](https://github.com/cktan/tomlc17/blob/fba134d7d1f9b58414f908600b2869f7e55ff2c4/OPTIONS.md)允许自定义realloc/free。C库仅为私有依赖，解析/类型转换和公开接口仍由C++封装。

选择理由：完整解析上游配置语法、无需C++异常，并可对解析临时分配设置预算。toml++也支持关闭异常，但默认STL分配在`-fno-exceptions`下的OOM回收更难保证，不作为首版首选。

初始限制：文本/文件最多16KiB、语法嵌套深度最多32、解析临时堆预算256KiB、语法树总节点最多1024、单数组最多256项、单表最多256键、输入peer数组最多64项（筛选后支持入口仍最多10个）。通过有字符串/注释状态的扫描限制嵌套深度，包括数组/inline table与dotted/table key路径；此扫描仅用于限额，不承担TOML解析。总节点和数组/表数量必须在私有解析器创建节点时检查，必要时在固定第三方源码上维护小型限额补丁，不能等整棵树建立后才检查。预算分配器在超过限制时返回null，并设置明确的资源错误标记；失败和成功都释放语法树。UTF-8验证开启。

256KiB是强制分配上限，不是所有16KiB输入的最坏开销保证。即使TOML合法且未超过文本长度，只要短字段、数组或其他复杂结构耗尽预算/节点限额，也返回ResourceLimit；上游格式兼容承诺适用于设备资源限制内的配置。容量测试包含大量短key/value、未知数组和嵌套表，验证拒绝时无泄漏，未支持字段的忽略不绕过资源限额。

解析库的全局options在组件首次加载时串行设置一次；组件私有库符号隔离，避免影响应用的其他TOML解析器。解析过程和单实例初始化串行，预算状态不暴露为公共全局配置；不能在实例运行时再次初始化或重置库options。

初始化配置流程：

```text
文本/文件 → 长度及深度限额 → TOML语法树 → 支持字段映射
          → 忽略项诊断 → 有效配置验证 → 自有配置副本 → Instance资源
```

解析仅在初始化阶段执行，运行任务不持有语法树或输入视图。初始化中断/错误保持Instance未初始化。必须通过故障注入验证第三方解析器遇到每个分配失败点均能清理，而不能仅凭文档承诺OOM行为。

### 4.5 文件加载

`init_from_file`使用已挂载ESP-IDF VFS路径，例如`/spiffs/easytier.toml`；组件不挂载SPIFFS、LittleFS、FAT或SD卡，不要求某一种存储介质。文件路径复制后使用，拒绝嵌入NUL；有限额读取，不能调用库默认允许巨大文件的无界parse_file。

文件打开失败返回`ESP_ERR_NOT_FOUND`或`ESP_FAIL`及诊断；读取到超过16KiB失败；I/O失败关闭文件并释放缓冲。文件和文本最终进入同一个转换/验证函数，不维护两套语义。

默认main用CMake `EMBED_TXTFILES "easytier.toml"`嵌入示例TOML，再调用`init_from_toml`，无需额外文件系统教学代码。README另演示已挂载文件系统下的`init_from_file`；文本和VFS文件两种入口均为首版交付。

## 5. 公开C++ API

### 5.1 接口形状

以下为后续实现的接口定义，需包含`<array>`、`<cstddef>`、`<cstdint>`、`<span>`、`<string_view>`和`esp_err.h`。头文件保留现有名称`include/easytier-esp.h`，内容是C++专用。

```cpp
namespace et {

struct Config {
    std::string_view instance_name = "default";
    std::string_view hostname{};
    std::string_view instance_id{}; // 空表示生成UUID
    std::string_view network_name = "default";
    std::string_view network_secret{};
    std::span<const std::string_view> peers{};
    std::string_view virtual_ipv4{}; // 与TOML ipv4相同的地址/CIDR表示；非空时dhcp被忽略
    bool dhcp = false;               // 无静态地址时按上游语义自分配虚拟IPv4
    std::string_view dhcp_subnet{};  // 空表示使用上游默认子网10.126.126.0/24
    uint16_t mtu = 1380;
};

enum class State : uint8_t {
    Uninitialized, Stopped, WaitingUnderlay, Connecting,
    Handshaking, Syncing, Ready, Stopping, Error
};

enum class EventType : uint8_t {
    Started, PeerUp, NetworkReady, PeerDown, Error, Stopped
};

struct Status {
    State state = State::Uninitialized;
    uint32_t peer_id = 0;
    uint32_t virtual_ipv4 = 0; // IPv4 octets按大端组成的数值
    uint8_t prefix_len = 24;
    size_t direct_peer_count = 0;
    size_t reachable_route_count = 0;
    esp_err_t last_error = ESP_OK;
    uint64_t dropped_packets = 0;
};

struct Event {
    EventType type;
    Status status;
    uint32_t peer_id = 0;
};

using EventCallback = void (*)(const Event&, void*) noexcept;

struct RuntimeOptions {
    EventCallback on_event = nullptr;
    void* user_data = nullptr;
};

enum class ConfigIssue : uint8_t {
    IgnoredParameter, IgnoredPeer, SyntaxError, InvalidValue,
    MissingRequiredValue, ResourceLimit, IoError
};

struct ConfigDiagnostic {
    ConfigIssue issue{};
    uint32_t line = 0;   // 未知位置为0
    uint32_t column = 0;
    std::array<char, 128> key{}; // 只记录字段路径，不保存敏感值
};

struct ConfigReport {
    std::array<ConfigDiagnostic, 32> items{};
    size_t count = 0;
    size_t omitted_count = 0; // 诊断多于容量时累计
};

class Instance final {
public:
    Instance() noexcept;
    ~Instance() noexcept;
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) = delete;
    Instance& operator=(Instance&&) = delete;

    esp_err_t init(const Config& config,
                   const RuntimeOptions& options = {}) noexcept;
    esp_err_t init_from_toml(std::string_view text,
                             const RuntimeOptions& options = {},
                             ConfigReport* report = nullptr) noexcept;
    esp_err_t init_from_file(std::string_view path,
                             const RuntimeOptions& options = {},
                             ConfigReport* report = nullptr) noexcept;
    esp_err_t start() noexcept;
    esp_err_t set_underlay_ready(bool ready) noexcept;
    esp_err_t get_status(Status& out) const noexcept;
    esp_err_t stop(uint32_t timeout_ms = 5000) noexcept;
    esp_err_t deinit() noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace et
```

### 5.2 行为与生命周期

三种初始化入口互斥地使用同一个有效配置验证器，不能先TOML初始化后叠加Config覆盖。不提供隐含的字段优先级规则。Config为便利入口，TOML为主要示例入口。

初始化复制全部配置字符串、入口列表和回调设置；返回后输入TOML、语法树、Config视图及RuntimeOptions临时对象均可释放。用户上下文指针指向的对象须保持到停止完成；如果停止后还要再次start，须继续保持该上下文有效直到deinit或析构完成。ConfigReport为调用方值对象，调用开始清空，失败也写入诊断。忽略诊断超过32项时累计omitted_count，不因诊断容量不足拒绝配置。ConfigReport约4.5KiB，示例使用静态存储；解析包装层直接写调用方报告，不能在app_main栈上再创建完整报告或大型配置临时副本。

日志最多记录字段路径、类别、位置及忽略数量，不打印TOML原文、secret、私钥或可能包含秘密的语法错误行。UI/应用可用ConfigReport展示忽略项；没有report时仍输出脱敏摘要。

首版只允许一个已初始化Instance；并发初始化通过实例槽保护，第二实例返回`ESP_ERR_INVALID_STATE`。peer ID随机非零；UUID来源于配置或随机生成，两者不能混淆。

`start`创建运行资源并异步连接，返回成功不等于网络Ready。`set_underlay_ready`非阻塞提交命令，可从WiFi/IP回调调用；队列满返回错误，不丢弃通知。应用应先`start`再通知物理就绪；示例只在WiFi取得IP后启动组件任务，确保随机源和underlay可用。

事件在控制面任务执行，参数只在回调期间有效。回调须快速返回，可读取状态，不得调用阻塞stop/deinit或析构Instance。生命周期API由应用串行调用，状态查询及underlay通知允许跨任务。

`get_status`返回线程安全副本；不返回内部连接、路由或span引用。状态流为：

```text
Uninitialized → Stopped → WaitingUnderlay → Connecting
              → Handshaking → Syncing → Ready
Ready/连接中 → WaitingUnderlay或重试
运行状态 → Stopping → Stopped → Uninitialized
```

`stop`对已停止Instance幂等；超时返回`ESP_ERR_TIMEOUT`并保留被任务使用的资源，允许重试。`deinit`仅释放已停止资源并清除密钥。析构请求停止后**持续等待全部任务确认退出，没有析构超时**，确认后才释放资源；若内部故障导致任务不退出，析构保持等待，不能提前返回、强制删除任务或释放Impl。所有正常阻塞点须可唤醒或有截止时间，但这不构成故障路径下析构有限时完成的保证。应用建议显式stop检查超时，不在组件任务内析构，不并发销毁仍被调用的实例。测试覆盖stop超时后任务恢复并确认退出，验证析构在确认前不释放资源。

## 6. 文件结构与构建

以下为拟创建结构；不表示这些功能已经实现：

```text
components/easytier-esp/
├── CMakeLists.txt
├── idf_component.yml          # >=6.1,<6.2
├── Kconfig                    # 资源上限，非网络凭据
├── README.md
├── include/easytier-esp.h      # Config/Instance/Status/Event/ConfigReport
├── src/
│   ├── instance.cpp           # 公开API和生命周期
│   ├── context.hpp/.cpp       # Impl及资源
│   ├── config/
│   │   ├── config.hpp/.cpp    # 有效配置验证与自有副本
│   │   ├── toml.hpp/.cpp      # TOML映射和忽略策略
│   │   ├── file.hpp/.cpp      # 有限额VFS读取
│   │   └── allocator.hpp/.cpp # 解析预算分配器
│   ├── io_task.cpp
│   ├── proto_task.cpp
│   ├── data_task.cpp
│   ├── transport/tcp.hpp/.cpp
│   ├── protocol/wire.hpp/.cpp
│   ├── protocol/protobuf.hpp/.cpp
│   ├── protocol/messages.hpp/.cpp
│   ├── crypto/siphash.hpp/.cpp
│   ├── crypto/aes_gcm.hpp/.cpp
│   ├── peer/session.hpp/.cpp
│   ├── rpc/rpc.hpp/.cpp
│   ├── route/topology.hpp/.cpp
│   ├── route/sync.hpp/.cpp
│   ├── route/table.hpp/.cpp
│   ├── netif/virtual_netif.hpp/.cpp
│   └── memory/frame_pool.hpp/.cpp
├── third_party/tomlc17/       # 固定提交、私有符号、LICENSE
└── test/
main/
├── main.cpp                   # WiFi与Instance调用
├── easytier.toml              # 上游格式示例，CMake嵌入
├── Kconfig.projbuild          # WiFi示例参数
├── CMakeLists.txt
└── idf_component.yml
tests/host/
tests/fixtures/
tools/
docs/easytier-esp-port-design.md
sdkconfig.defaults
```

组件公开传播`cxx_std_20`，私有关闭异常/RTTI；第三方C库使用所需C标准编译。源码显式注册，公开include不暴露私有头/第三方类型。公开依赖仅为头文件所需ESP错误类型，FreeRTOS/lwIP/mbedTLS/esp-netif/esp_timer/随机源为私有依赖；VFS读取使用IDF libc接口，组件不依赖某个文件系统。

main显式依赖easytier-esp、esp_wifi、esp_event、esp_netif、nvs_flash，嵌入TOML文件。组件README同时说明嵌入配置和VFS文件用法。

## 7. 任务与所有权

| 任务 | 独占职责 | 初始栈/优先级 |
|---|---|---|
| et_io | 所有socket、connect/select、TCP重组、发送游标、关闭 | 6KiB / 7 |
| et_proto | 握手、Ping、RPC、拓扑、路由、状态事件 | 10KiB / 5 |
| et_data | IPv4、数据AES、overlay转发、网卡投递 | 8KiB / 7 |

默认不绑核，优先级低于WiFi和TCPIP，栈按水位调整。

```text
socket → et_io → 控制队列 → et_proto
               → 数据队列 → et_data → tcpip_input → 应用socket
应用socket → netif output → pbuf队列 → et_data
et_proto/et_data → 控制/数据TX队列 → et_io → socket
et_proto → 长度1快照邮箱 → et_data私有路由副本
```

socket非阻塞，select同时监测读写，处理部分写/EAGAIN/EOF/截止时间。每轮限定字节和帧预算，控制优先且数据有服务配额。TCP帧开始发送后完整写完才切换优先级，不在半帧中插入其他帧。

连接引用为slot+generation，旧代际队列项释放，fd复用不会造成误发送。统一帧池由短临界区保护，队列传索引；成功入队转移所有权，失败由生产者释放。控制帧保留额度。

路由通过长度1队列`xQueueOverwrite`按值发布，数据任务持私有副本；其他需要快照的读者使用自己的邮箱，不竞争消费同一队列。替代附件无读者保护的双缓冲：仅用atomic索引不能阻止连续发布覆盖读者使用的旧缓冲。

初始化可动态分配，运行期组件热路径不进行无界分配。lwIP自身pbuf/socket分配单列。TOML语法树在任务启动前释放。

## 8. 协议实现及附件修正

### 8.1 线格式、握手与AES

多字节字段显式LE读写，packed结构仅用于大小/布局校验，不直接解引用未对齐字段。

TCP头4B，body=PM头16B+实际payload，长度16～2000，实际payload包括AES尾。PM.len保存原始明文长度，无压缩AES profile下实际payload=PM.len+28，最大明文payload1956B。拆TCP帧不能使用PM.len。

AES尾为tag16||nonce12，AAD为空；不得擅自认证PM头，否则无法互通。随机nonce使用ESP网络就绪条件下的随机源；控制面和数据面各持AES上下文。认证成功清Encrypted并截28B；失败丢弃整包，不投递可能已被改写的解密缓冲。

IDF 6.1携带的是mbedTLS 4.x，公开头文件不再提供`mbedtls_gcm_context`（`gcm.h`仅存在于private目录）。组件改用PSA Crypto的`psa_aead_encrypt`/`psa_aead_decrypt`配`PSA_ALG_GCM`实现AES-128-GCM，tag由PSA追加在密文尾部，组件再拼接自身nonce；初始化阶段调用一次`psa_crypto_init()`。宿主测试用OpenSSL EVP作同一接口的独立实现，并交叉比对NIST向量。

SipHash-1-3(k0=k1=0)与上游锁定实现一致；`derive_key_128("secret")`应为`565a19db4ef0c121a8ac580edaf84ea6`。digest按network_name+secret及累计digest前缀逐轮派生，finish输出BE64。DefaultHasher实现不是Rust永久协议保证，因此以提交和黄金向量锁定。

握手发送type=2/to=0，校验network_name、digest32B、self peer冲突。首版不宣告liveness扩展，reserved=0；flags中未实现扩展不产生。

附件Ping8B有误：主动Ping使用u32 LE 4B。Pong按上游直连处理仅改type，payload和PM from/to原样。响应超时2秒；保活1/2/4/8/16/32秒变化，连续失败关闭。UDP打洞连接不能笼统使用附件1秒周期，该短周期仅适用于上游特定TCP打洞路径。

### 8.2 protobuf与RPC

有界protobuf codec支持varint、fixed32/64、length-delimited、未知字段跳过、packed/unpacked repeated、负int32/64的10B编码、optional存在位、oneof、重复scalar最后取值。长度/嵌套/计数先校验，失败不越界。

UUID编码为四个uint32字段，不是bytes16；IPv4 protobuf数值为octets按大端组成的整数，与lwIP网络序存储显式转换。

RPC从首期支持分片，TCP同样使用上游1300B UDP payload预算构造。新单包1/0、历史0/0均接受。片0提供权威descriptor和compression信息，后续片可以缺这些字段。重组键包含from/to、事务、请求方向及连接代际；支持乱序与一致重复，冲突重复失败。完整body后才解析RpcRequest/Response，超时、缺片、超限归还资源。

服务为network_name域/OspfRouteRpc/SyncRouteInfo/method_index=1，同时实现客户端和服务端。非本地目标RPC按overlay路由转发，不当作本地请求解密或invoke。

RPC压缩与数据压缩独立。发送algo=None、accepted_algo=None；缺省历史compression信息为未压缩。上游初次请求的缺省能力回落到无压缩，后续学习对端accepted能力；必须互操作验证。实际收到Zstd报告不支持，取消事务并撤销Ready，不按明文解析。

RPC加密遵循目标public feature；未知feature的直连目标默认非public（加密），未知非直连目标按上游public回退（明文）。Handshake/Ping/Pong明文；数据源加密，中继不重复加密，目的解密。

### 8.3 路由会话与合并

必须双向主动Sync：响应中没有RoutePeerInfo，仅等待对端请求无法宣告ESP的IP/邻接。实现session ID、initiator角色、代际准入、旧响应拒绝、重连全量同步；不能用“等待5秒再作为发起方”的简化规则替代会话语义。

发conn list，收list和bitmap；support_conn_list_sync=true不保证对端只发list。bitmap为LSB-first，位索引row*N+col，校验N*N溢出与字节长度。

peer与conn版本分别维护，版本0占位不进入图，缺资料通过missing IDs补发。仅成功响应且session仍有效才提交已发送版本；error缺省与present=0 DuplicatePeerId必须区分。

大于本地版本才合并与更新时间，接收时间采用本地时钟。不可达IP/next-hop立即撤销；90秒是不可达元数据清理，3600秒刷新与3660秒过期分开处理。

路由使用跳数最短路、avoid_relay_data惩罚及peer ID稳定择优，不承诺与上游所有延迟择优结果完全一致。自身宣告list支持、disable_p2p=true、NAT Unknown，无未实现能力。

保留有界原始RoutePeerInfo以传播未知字段，不把未理解扩展删掉后反射出去。拓扑更新先验证容量再提交，不截断后返回成功，不静默淘汰仍用于活跃路径的节点。

### 8.4 数据与UDP阶段

出站IPv4精确映射目标peer再选next-hop，源forward_counter=1。转发保留最终to_peer/from_peer及AES尾，只更新计数和传输连接；counter>7丢弃，否则递增。只有目标本机才解密。首版出站单播，广播/组播丢弃计数；远端已扇出的合法入站IPv4可交付。

UDP后续阶段使用8B mux头、Syn/Sack 8B magic，匹配来源地址+conn_id+magic，重复Syn重发Sack，len精确等于实际报文减8B。不依赖枚举中有值但上游未实现接收分类的Fin关闭。UDP的外网MTU和数据分片另行验收，不能将TCP的2000B预算直接套用。

## 9. IDF 6.1 lwIP集成

私有raw L3 netif，不模拟Ethernet/ARP，不设default。注册、状态改变、地址配置及删除通过netifapi线程安全执行；入站用tcpip_input，不能直接从数据任务调用ip_input。

output在TCPIP上下文只pbuf_ref+零等待入队；满时释放额外引用并返回ERR_MEM。数据任务正确复制pbuf链，校验IPv4 version/IHL/total_length/MTU。tcpip_input成功转交pbuf所有权，失败自行释放。

首版单虚拟前缀由lwIP网卡匹配选路，物理default继续用于入口和普通上网。启动及物理IP变化时拒绝虚拟/物理前缀重叠；入口解析IP位于虚拟前缀也拒绝，防止递归隧道。跨前缀/子网代理延期。

`dhcp`自分配按上游`DhcpIpv4Allocator`语义实现：从已发布的路由信息收集已用虚拟地址，优先沿用当前子网，否则选择通告的子网（取`first_address`、前缀最小者），都没有时先等待两轮再回落到默认子网`10.126.126.0/24`；候选地址从子网主机段随机起点线性探测，避开已用地址。同代价选择规则与上游一致，地址变化时更新netif地址并重新发布自身`RoutePeerInfo`。分配的地址必须校验与物理网段不重叠。本节点不实现为其他节点分配地址的服务端。

underlay丢失或最后有效peer断开时虚拟link down；恢复重新握手同步。固定虚拟IP冲突通过拓扑检测报错，不宣称支持上游自动地址分配。

## 10. 资源、错误和关闭

| 资源 | 初始S3容量 |
|---|---|
| 拓扑节点 / 直连 / 支持入口 | 32 / 10 / 10 |
| 帧池 | 24×2048B，其中8额度保留控制流量 |
| RPC事务 / 重组槽 | 4 / 2 |
| 每槽重组大小 / 片数 / 超时 | 16KiB / 32 / 3秒 |
| 原始RoutePeerInfo | 每节点512B |
| TOML文本 / 深度 / 临时堆 | 16KiB / 32 / 256KiB |
| TOML节点 / 单数组项 / 单表键 / 输入peer项 | 1024 / 256 / 256 / 64 |
| 配置诊断 | 32条，多余累计数量 |
| Config字符串 | 网络名128B、密钥256B、入口URL每项256B、instance_name128B |

组件运行资源估算约150～200KiB量级，WiFi/lwIP额外开销单列。TOML解析是任务启动前的临时峰值，最大预算256KiB，不与满额运行帧/RPC资源同时分配；实例初始化先解析/复制/释放树，再创建固定运行资源。不得沿用附件总计小于50KiB估算。

记录当前/最小堆、最大连续空闲块、任务栈水位、帧池峰值、RPC重组峰值和解析临时峰值。PSRAM可承载大缓冲，任务栈与关键状态优先内部RAM；不得假设PSRAM总能满足内部RAM或连续块要求。

连接/握手各5秒截止；网络失败按1/2/4/8/16/30秒加随机抖动退避。鉴权/能力错误低频重试。明确区分配置、容量、认证、协议、无路由和物理断网错误，不输出秘密。

停止顺序：禁止新网卡输出；I/O关闭连接并作废generation；取消事务；归还所有帧/pbuf；等待各任务确认退出；线程安全移除netif；释放队列和资源。超时保留资源。初始化/启动中途失败反序回滚，析构不释放仍被任务访问的对象。

## 11. main示例设计

main只执行：NVS初始化、esp-netif/事件循环、WiFi STA连接和重连、等待取得IP、读取嵌入TOML、初始化静态`et::Instance`、启动及通知underlay、打印组件事件。

默认示例不挂载文件系统，不增加ping/echo服务。独立README片段演示VFS配置：

```cpp
static et::Instance instance;
static et::ConfigReport report;

// 应用已初始化esp-netif/事件循环、挂载文件系统并连接物理网络。
esp_err_t err = instance.init_from_file("/spiffs/easytier.toml", {}, &report);
if (err == ESP_OK) {
    err = instance.start();
}
if (err == ESP_OK) {
    err = instance.set_underlay_ready(true);
}
// 应用处理err和report；停止时显式调用stop/deinit。
```

示例不直接创建socket隧道，不引用AES/RPC/路由/netif私有头。业务通信仍通过普通lwIP socket。真实WiFi密码和网络密钥不提交；示例配置只保留占位密钥与示意入口。

## 12. 实施阶段与验收

| 阶段 | 实现单元 | 验收 |
|---|---|---|
| M0 | IDF6.1构建、Instance API、Config/TOML/VFS、资源模型、wire/SipHash/AES | 上游配置文本和文件有效字段一致；不兼容参数忽略；配置视图释放安全；失败回滚与黄金向量通过 |
| M1 | 非阻塞TCP、握手、代际、underlay恢复 | 与同网络Linux入口握手；拆包/粘包/短写/超时/断WiFi恢复正确 |
| M2 | 4B Ping、双向RPC、分片、压缩协商 | 5分钟稳定；乱序/重复/缺片/超限正确；默认含zstd能力上游完成None协商 |
| M3 | 双向Sync、list/bitmap、session、拓扑和快照 | Linux学到ESP IP/邻接；三节点下一跳正确；旧代际拒绝；断链立即撤销路径 |
| M4 | L3 netif（静态与dhcp自分配）、AES单播、中转、最小TOML WiFi示例 | ESP A—中继B—Linux C双向ICMP/TCP/UDP；密文转发不变；地址自分配与冲突回避正确；断网恢复与资源回收 |
| M5 | UDP mux及配置支持扩展 | UDP peer从忽略项升级为支持项；达到TCP同等互通；magic/长度/重复Syn正确 |
| M6 | STUN/地址发现RPC/ConeToCone独立设计 | 核对上游线格式后实现，失败稳定保留中继 |

宿主纯协议/配置测试使用独立CMake/CTest，不依赖IDF任务。上游Rust生成golden fixtures，不只依靠自编码自解码。

配置测试至少覆盖：

- 同一配置从文本和临时文件初始化后字段一致；使用上游核心测试里的完整TOML字段布局。
- `[network_identity]`、`[[peer]]`、inline table、dotted/quoted key、注释、CRLF、多行/转义字符串、数组表、合法UTF-8。
- 上游默认identity、缺省MTU、裸IP和/32归一化、UUID、hostname规范化。
- 未知嵌套表/数组、listeners/proxy/secure/IPv6、未知flags全部忽略，已支持项仍生效。
- unsupported算法和加密false忽略后使用固定profile；不把其出现当成初始化失败。
- TCP与不支持scheme混合过滤、仅不支持入口、超过支持入口上限（10）、缺IPv4且未启用DHCP、dhcp非法CIDR（前缀>30或非CIDR字符串）、dhcp与ipv4并存时以ipv4为准；缺省密钥与显式空串通过文本/文件/Config入口得到相同普通身份。
- 支持字段类型/值错误、重复key/坏转义、文件不存在/短读/超限、NUL路径、深度/预算/节点限额，以及大量短key/value造成的高开销合法输入。
- 解析每个分配失败点、资源回滚、源文本释放、诊断截断计数、不泄漏secret到日志。

协议测试覆盖SipHash/digest、固定nonce AES/tag损坏、TCP边界、UUID/IPv4、protobuf未知/packed/负数/optional error=0、bitmap、RPC片序与缺片。

IDF测试覆盖Instance不可复制/移动、单实例占用、初始化失败后重试、stop幂等/超时、析构回收、回调限制、pbuf链、队列满、旧generation、快照连续更新。集成测试固定A—B—C，检查C学习A地址、双向普通socket通信、断B后立即失效以及重连重同步，对照easytier-cli route。

首版成功标准：ESP-IDF6.1/当前ESP32-S3通过M0～M4；组件复制到另一个6.1工程可独立使用；公开类名Instance；TOML文件/文本均可初始化，忽略项有诊断；main仅负责WiFi和组件调用；三节点通信、恢复、容量错误和安全回收有实测证据。

## 13. 实施注意与文档维护

本文是设计交付，不表示接口、解析器或固件已经实现，也未宣称通过构建或硬件测试。实施时先完成M0，再按协议依赖顺序推进；每阶段保留可独立检验结果。

向后兼容另立任务，首版不承诺5.x或其他版本。TOML新增支持字段时同步更新映射表、有效profile、诊断和文本/文件一致性测试。协议升级记录上游提交变化与golden差异。

从EasyTier改编代码保留来源和适用许可证说明；第三方TOML解析器保留MIT LICENSE与固定提交。组件README记录真实RAM/Flash数据、已验证设备和配置兼容限制，不以设计估算代替实测。

## 14. 实施进度与实测记录

> 本节记录**实际**代码状态与本文设计的差异，避免把设计目标误当成已完成功能。

### 14.1 已完成并有可复现证据

| 里程碑 | 状态 | 证据 |
|---|---|---|
| M0 组件底座 | 完成 | 宿主测试 57 个用例全部通过（`ctest`）；ESP-IDF 6.1 构建通过 |
| M0 配置/TOML | 完成 | 文本与 VFS 文件两条路径、忽略并报告、深度/字节预算、失败回滚均有用例 |
| M0 crypto/wire/protobuf | 完成 | 独立生成的 SipHash/KDF 向量、NIST GCM 向量、Google protobuf 黄金样本 |
| M1 传输/握手 | 完成（缺少真实对端互操作） | 回环 socket 覆盖拆包/粘包/部分写/背压/非法分帧/对端关闭；握手校验含黄金样本 |
| M1 任务与生命周期 | 完成 | I/O 与控制面任务、队列、代际、退避重连、start/stop/deinit 已编译进固件 |
| M2 控制面 RPC | 完成（缺少真实对端互操作） | 分片/重组/事务/注册表与压缩协商共 16 个用例；每片编码 ≤ 1248 字节（上游 UDP 预算） |
| M3 双向路由同步 | 完成（缺少真实对端互操作） | `RoutePeerInfo`/邻接表/位图/请求响应编解码与上游黄金样本逐字节一致；拓扑合并、版本、过期、有向图 Dijkstra 与快照共 19 个用例 |
| M4 数据面 | 完成（缺少真实对端与真机验证） | lwIP raw L3 网卡（netifapi + tcpip_input）、数据面任务、dhcp 自分配与转发决策共 14 个新用例；固件链接通过 |
| 多入口并发连接 | 完成（缺少真机验证） | I/O 任务改为 `kMaxConnSlots = 2` 个连接槽：每槽独立连接一个入口、独立代际/退避/握手/Ping；配置里其余入口作为轮换备用。帧选路规则：控制帧按"代际 → 槽"（代际由全局计数器分配，唯一标识一个槽），数据面/转发的帧按 `to_peer`（下一跳）选槽。`SyncEngine` 改为多直连集合（`on_peer_up/on_peer_down(peer_id)`），邻接行同时宣告全部直连；新增 2 个多对端用例（含"断开一个不影响另一个、断开全部才撤销路由"） |
| 设备侧静态审查（第三轮：生命周期与网卡注销） | 完成 | **释放顺序存在 use-after-free 窗口**：`destroy_queues` 先删除了所有队列（含网卡的 `q_netif_rx`），之后才注销虚拟网卡；这期间网卡仍处于 up 状态，TCPIP 线程的 output 回调可能向**已释放的队列句柄** `xQueueSend`。现改为**先注销网卡**（`netifapi_*` 同步，返回后不会再回调）再删队列，并去掉一处重复的 `destroy()`。同时复核了 `start/stop/deinit` 的失败路径：部分任务创建失败时会先等已启动任务退出再释放（不会边跑边删），`deinit` 在 started 时拒绝执行，析构走无限等待 |
| 配置解析的敌意输入（有界拒绝） | 完成 | 新增 `tests/host/test_config_hostile.cpp`（8 个用例）。**实证结论：解析器对所有敌意输入都是有界拒绝**（返回错误 + 诊断，不崩溃、不无限递归）。覆盖面：① 深层**点分嵌套**（`[x.a.b.c…]` 与 `a.b.c… = 1` 两种写法）——实测浅层（≤8 段）可用、10~15 段以 `InvalidValue` 拒绝、≥16 段以 `ResourceLimit`（TOML 堆预算）拒绝，已把"深层必须拒绝、浅层必须可用"写进断言而不绑定具体阈值；② 大量并列表/键（宽度攻击）被节点数/表键数上限兜住；③ 超长单行/超上限文档被文本长度上限拒绝；④ 内嵌 NUL 字节、未闭合字符串、重复键、非法数组均被拒。顺带纠正了两个我最初的错误预期：`[a]\n[b]` 是**并列**表而非嵌套（真嵌套只能靠点分名），以及"超长但合法的键名"应当被接受（内存上界由 16 KiB 文本上限保证） |
| 解析器健壮性（面向不可信输入） | 完成 | 新增 `tests/host/test_parser_robustness.cpp`（5 个用例）：① 2000 组确定性随机字节灌入全部解码器（PM 头/TCP 长度前缀/节点信息/邻接表与位图/同步请求响应/RPC 分片与请求响应体），只要求"全函数"——不崩溃、不挂死、返回的视图不越界；② 对 7 个上游黄金样本**逐长度截断**（完整样本必须解析成功，截断输入只要求不越界）；③ 对 4 个样本做**逐字节逐位翻转**；④ 明确断言截断 varint 与超长长度字段被拒绝。过程中厘清并钉住了一条**分层契约**：解码器是"忠实报告线缆取值"的纯函数（字节翻转可让 `total_pieces` 变成 254），范围/一致性校验属于**重组器**——已补用例证明 `feed()` 会拒绝越界片数（我最初的断言写成"解码器必须自行限幅"，属测试预期错误，代码本身正确） |
| 公开 API 编译检查 | 已提供 | 宿主构建中的 `et_api_compilecheck` OBJECT 库编译公开 API 用法，只编译不链接；验证接口签名可用。文档、生命周期约束和真机结果仍需人工核对 |
| 线程与回调约束成文 | 完成 | 在公开头文件里补齐 **@section thread_safety** 表格与规则：哪些接口可从任意任务调用、哪些允许在事件回调中调用、哪些必须串行；并明确"回调运行在控制面任务里且是**同步**回调，因此在回调中调用 `stop()/deinit()/析构` 会等待该任务自身退出而**死锁**"、"`stop()` 返回后不再有任何回调，此时才可释放 `user_data`"。README 增加同内容的约束小节。同时复核示例：WiFi 事件里只调用非阻塞的 `set_underlay_ready()`，`init_from_toml()`+`start()` 放在独立 8 KiB 任务中执行，与约束一致 |
| Kconfig 与代码/文档一致性核对 | 完成 | 逐个核对 7 个 Kconfig 选项的**实际作用**，发现 3 处漂移：① `EASYTIER_ESP_LOG_LEVEL` 号称控制组件日志级别，但 `log.hpp` 完全没用它——设置它毫无效果；② `EASYTIER_ESP_FRAME_POOL_SIZE`（共享帧池）在改用队列后已无引用；③ `EASYTIER_ESP_MAX_CIDRS`（代理网段）因"子网代理延期"而从未使用。处理：**日志级别真正接入** `log.hpp`（编译期裁剪，高一级的语句完全不编译进固件，实测 level=0 使固件从 907,040 降到 900,864 字节，−6,176 B），另两个**死配置直接删除**（避免留下"设置了却不生效"的旋钮）。`config_limits.hpp` 另加 `static_assert` 范围守卫：直接编辑 sdkconfig 绕过 Kconfig range 时会在**编译期**失败而不是运行期越界。新增 3 个宿主用例把"宿主默认值 == Kconfig 默认值"钉死（改任一侧都必须同步另一侧） |
| 数据面整链验证（源加密/中继不变/目的解密） | 完成 | 此前数据面只有"决策函数级"的用例（`plan_inbound`/`prepare_forward`/`build_data_frame`），没有把三节点串起来。新增 `tests/host/test_data_relay.cpp`：用**生产代码的同一批函数**走完整链路——A `build_data_frame` + `encrypt_random_nonce` 打帧 → B `decode_peer_manager_header` + `plan_inbound` + `prepare_forward`（转发）→ C `decode_peer_manager_header` + `plan_inbound` + `decrypt_in_place`（投递）。断言：① 整帧只有 `forward_counter` 一个字节变化（密文与 AEAD 尾逐字节不变）——这正是设计要求的"中继不重复加密、密文转发不变"；② 目的端用同一网络密钥还原出原始 IPv4/UDP 报文；③ 错误密钥解密失败；④ **明文绝不出现在整帧里**（用 `std::search` 在整帧中查找原始报文必须找不到） |
| 上游未知字段的保留与转发（子网代理等） | 完成 | 上游 `RoutePeerInfo` 的字段远多于本组件建模的数量（`proxy_cidrs`、`easytier_version`、`cost`、IPv6、凭据证明……）。设计上我们"不解析但必须原样转发"，此前**只用自己编码的字节验证过 raw 保留**（且只比对长度），无法证明上游生成的富信息能穿过我们。现新增由上游 `.proto` 生成的"富信息"黄金样本（含两个 `proxy_cidrs`、`easytier_version`、`cost`、`is_public_server` 等），并加 3 个用例：① 解码后已知字段正确且 `raw` 与上游字节**逐字节相同**；② 再叠加一个手写的"未来新增字段"(tag 31) 仍能跳过并保留（顺带验证被截断的 varint 会被拒绝）；③ **端到端转发**：A 从 B 处学到 C 的富信息后，A 发给 D 的同步请求里必须逐字节包含 C 的原始字节，同时 C 的地址确实进入路由表、下一跳为 B。"既能用、又能原样转发"因此被固定下来（子网代理等延期功能的信息不会因经过 ESP 节点而丢失） |
| 路由会话与重复 id 语义对齐上游 | 完成 | 与上游 `PeerRoute`/`check_duplicate_peer_id` 逐条对照后修正三处：① **会话 id 改为每连接一份**（上游 `my_session_id` 属于 PeerRoute 实例）——此前引擎只存一个值，第二个入口握手时覆盖它，会让第一个对端认为"我们重建了会话"而清空它保存的信息；② **会话 id 变化即清空该对端已保存的信息**（上游 `session id mismatch, clear saved info`）并递增自身信息版本重新宣告，新增 `Topology::reset_peer()`（version 归 0 占位、退出建图）；③ **补齐 `DuplicatePeerId` 检测**：对端上报"它自己"的路由 id 与我们已知的不同且版本更旧时，按上游语义回一个 `error` 字段存在且为 0 的响应，且**不覆盖**已保存的较新信息。新增 3 个宿主用例（每对端会话 id 互不覆盖、会话变化清空信息并在重新宣告后恢复、重复 id 触发错误而路由 id 相同/版本更新不触发） |
| 保活/断链检测对齐上游 | 完成 | **补齐设计文档第 378 行要求但此前缺失的行为**：原实现只有"间隔 1/2/4/8/16/32 秒翻倍"，没有"响应超时 2 秒"，也**没有"连续失败关闭"**——对端静默死亡（断电/NAT 超时且无 FIN）时只能等 TCP 重传，可能几分钟甚至永不发现。现抽出 `peer/keepalive.hpp/.cpp` 状态机并与上游 `PeerConnPinger` 对齐：每次 Ping 固定 2 秒响应超时、连续 5 次失败关闭连接、失败后立刻回到最小间隔快速探测、**只有匹配的 Pong 才清零失败计数**（入站数据流量不算存活，上游专门有用例覆盖该点）。控制面按槽持有状态机，关闭动作走 `request_reconnect` 复用既有的断开/重连与拓扑撤销路径。新增 6 个宿主用例钉住全部时序边界（首次立即发送、间隔翻倍与封顶、1.999s 不超时/2s 整超时、第 4 次仍重试而第 5 次关闭、成功后清零、抖动后恢复） |
| 示例配置纳入宿主管控 | 完成 | 新增 `tests/host/test_example_config.cpp`（3 个用例，宿主专用）：直接解析随固件发布的 `main/easytier.toml`，断言 (1) 解析成功且网络名/密钥/实例名/主机名/入口 host+port/静态地址/前缀/MTU 全部映射正确；(2) 诊断里**不允许有错误**，只允许"已忽略参数"，且忽略项必须恰好是预期的那几类（`listeners` 与 `flags.*`），新增未支持键会立刻失败；(3) 与联调指南里示例的网络名一致。此前该文件写错只有烧录后才会以"配置被拒绝"暴露 |
| Zstd 不兼容时撤销 Ready | 完成 | **修正与设计文档的偏差**：此前收到 zstd 压缩的 RPC 只 `set_error(ESP_ERR_NOT_SUPPORTED)`，节点会一直停在 `state=ready`、`network_ready_reported=true`，但每次同步都失败——接入方看到"就绪"却永远学不到路由。现新增 `revoke_ready()`：置错误、计数丢弃，并在**曾经上报过 Ready** 时把状态切到 `State::Error` 并补发一次错误事件（避免每个 zstd 报文刷事件）。同时新增宿主用例把"zstd 必须被如实解码出来"的编解码契约固定下来（含首片带压缩信息、后续片不带） |
| 配置初始化边界审查 | 完成 | 复核"重复 init / 失败 init / 反复 init-deinit"路径，**未发现缺陷**：`init_from_internal` 用 CAS 抢占单实例标志，竞争失败者不会清掉持有者的标志；只有 Impl 分配失败才回滚标志；`impl_` 在全部失败路径之后才赋值；`prepare_context` 值拷贝 `StoredConfig` 并从副本派生密钥，不保留调用方视图；`init_from_file` 的 16 KiB 文件缓冲与 `init`/`init_from_toml` 的解析缓冲都在静态区，调用方栈不受影响 |
| 分片链路集成用例 | 完成 | 新增 `sync_engine_request_survives_rpc_fragmentation`：用**真实的同步请求**（32 节点拓扑、长主机名 → 请求必然超过 RPC 单片 1248 字节预算）走完整链路 `build_request → encode_rpc_request → plan_rpc_slices → encode_rpc_piece →（乱序）decode_rpc_packet → RpcReassembler → handle_request`，逐字节校验重组结果并用对端引擎验证学到了发送方的虚拟地址与下一跳。这补上了"分片/重组此前只用通用内容测试"的缺口，是宿主机上最接近两节点真实互操作的验证（仅差 socket 传输） |
| 内存分配失败路径可诊断 | 完成 | 为每一处可能失败的内存分配补上明确日志：Context/Impl（约 99 KiB，提示"启用 PSRAM 或减小缓冲"）、队列/信号量（约 24 KiB）、控制面与数据面 AEAD（PSA/mbedTLS）、每个连接槽的 `TcpConn`（约 12 KiB）、三个任务（含所需栈大小）。此前这些路径只返回 `0x101`（ESP_ERR_NO_MEM），真机上难以定位 |
| 公共 API 契约设备侧用例 | 完成（待烧录执行） | 新增 3 个设备专用用例：未初始化时各接口的错误码、三类非法配置必须被拒绝、`stop/start/deinit` 的幂等与顺序约束。**这些契约先在宿主侧用同一 API（`stored_config_from_config`）固定下来**（新增宿主用例，118 个），确保设备侧断言的错误码与实际一致，不会在烧录后才发现期望写错 |
| 示例断网重连行为复核 | 完成 | **修复真实缺陷**：示例在首次连上 WiFi 后把初始化交给独立任务，但后续 WiFi 掉线重连时既不再创建任务、也没有重新调用 `set_underlay_ready(true)`，导致组件永久停在"物理网络未就绪"，**再也不会连接入口**。现在每次取得 IP 都会重新通知就绪。同时 I/O 任务在"物理网络由断到通"时复位全部槽的退避，按 1 秒节奏快速恢复（而不是等最长 30 秒） |
| 设备侧测试诊断增强 | 完成 | 设备侧测试逐用例输出**任务栈高水位**与**堆增量**（并汇总"疑似泄漏"数、打印起止空闲堆）：真机联调时这些数字比 pass/fail 更有用——既能核对大栈是否真的需要，也能发现 init/deinit 反复调用时的泄漏。栈高水位过低时还会提示可调小用例栈 |
| 设备侧数据路径用例 | 完成（待烧录执行） | 新增 `target_virtual_netif_data_path`：**不需要任何对端**即可验证 M4 最容易出错的净荷路径——手工构造 UDP 报文（IP/UDP 校验和由独立实现离线校验并作为黄金值写入断言）→ `netif->input`/`tcpip_input` → `ip_input`/`udp_input` → 绑定虚拟地址的 socket 收到；反向 `sendto` → lwIP 路由到本网卡 → `output` 回调 → 数据面队列收到**完整 IP 报文**（无链路层头），并检查 `pbuf` 引用计数由数据面归还 |
| 设备侧静态审查（第四轮：网卡与 pbuf 路径） | 完成 | 复核 `virtual_netif` 的 output 回调（TCPIP 线程只 `pbuf_ref` + 零等待入队，队列满归还引用并返回 `ERR_MEM`）、入站 `netif->input` 失败时由调用方释放、`pbuf_alloc/take/copy_partial` 的链式 pbuf 处理，以及数据面 `deliver_to_netif` 的所有权转移；本轮未发现新缺陷 |
| 设备侧生命周期用例 | 完成（待烧录执行） | 新增 3 个设备专用用例（共 6 个）：`target_instance_lifecycle_without_peer`（init→start→stop→deinit 全流程：连接槽/TcpConn 分配、三任务创建与退出确认、网卡创建、**事件状态与 get_status() 一致**）、`target_instance_restart_after_stop`、`target_instance_single_instance_guard`；入口用不可达的 TEST-NET 地址，不依赖真实对端。测试镜像现已链接完整组件运行时（0x88310） |
| 设备侧静态审查（第二轮，含多连接路径） | 完成 | ① **发送队列背压被当成失败丢弃**：`pump_tx` 在目标槽位暂满时走到"无匹配槽"的丢弃分支，正常背压下会静默丢帧（控制面 RPC/握手也会丢）；现区分"有槽认领但忙"（保留队头）与"没有任何槽匹配"（丢弃）。② **每槽写入预算退化成 1 字节**：均分 `kTxBudgetBytes` 后第二个槽每轮只能写 1 字节（≈50 B/s）；现按槽均分（各 4 KiB）。③ **事件里的状态是陈旧的**：`emit()` 回显配置里的静态地址并把 `reachable_route_count` 写死为 0，dhcp 节点的 `NetworkReady` 事件里虚拟地址为 0；现统一取 `my_ipv4/my_prefix/reachable_routes` 的实时值，与 `get_status()` 一致
| 真机必崩问题修复（静态审查所得） | 完成 | ① **`TcpConn` 从未分配**：`Context` 里是 `TcpConn*`，但 `start_runtime` 没有 `new`，I/O 任务会直接报"no connection object allocated"退出——即设备永远连不上任何入口；现按槽分配/释放。② **公共 init 路径栈帧过大**：`Instance::init` 最坏栈帧 7,792 B、`init_from_toml` 3,312 B，而示例在 `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304` 的 WiFi 事件任务里调用它（必然溢出）；现把 `StoredConfig`/`ConfigReport` 移到静态缓冲，示例改为在 8 KiB 独立任务里做初始化，事件回调立即返回 |
| 联调准备 | 完成 | 新增《硬件联调指南》（`docs/硬件联调指南.md`）：静态检查 → 设备自检 148 用例 → Linux 入口节点 → 示例配置与期望日志逐条对照 → 直连/中继连通性验证 → 13 项排错表 → 需反馈的证据清单；示例启动时打印**生效配置**与每 10 秒一行的状态摘要（`state/peer_id/vip/direct/routes/dropped`），并新增 `Instance::get_config_info()` 诊断接口 |
| 任务栈静态校验 | 完成 | `tools/check_stack_usage.py` 用 `-fstack-usage` + 调用图计算三个任务入口的最坏调用链：I/O 2400/6144、控制面 5632/10240、数据面 4400/8192 字节；并列出组件内最大栈帧（当前最大 `et::proto_task` 4368 B）。**修复了一处必然崩溃**：`SyncEngine::build_request` 曾占 9440 字节栈帧，控制面最坏链 14736 > 10240（首次路由同步就会栈溢出） |
| 设备侧测试工程 | 完成（构建通过，待烧录执行） | `test_apps/easytier_esp_tests` 把宿主用例搬到设备运行：152 个用例（其中 142 个与宿主完全相同的断言 + 10 个设备专用：PSA AES-GCM NIST 向量、虚拟网卡生命周期、**网卡数据路径往返**、网段重叠、Instance 生命周期/重启/单实例守卫、**API 契约与错误路径**），镜像 0x91530 |
| 宿主多节点验证 | 完成 | 抽取 `SyncEngine`（拓扑合并/最短路/dhcp 的纯逻辑），用真实编解码让 2~3 个引擎互相对话：两节点互通、A—B—C 经中继收敛、bitmap/list 两种形式、missing 补发、断链撤销共 6 个用例 |
| M4 示例形态 | 完成 | `main` 只做 NVS/esp-netif/WiFi/内嵌 TOML/underlay 通知；固件链接了组件符号 |

当前固件实测（ESP32-S3，ESP-IDF 6.1，`idf.py build`）：应用镜像 `0xdaa70` 字节，1 MiB 应用分区剩余 15%。这是**包含 WiFi 全家桶**的体积，后续 M3/M4 需要关注分区余量。

### 14.2 与设计文档的差异（实现选择）

1. **AES-GCM 使用 PSA Crypto**：IDF 6.1 的 mbedTLS 4.x 不再公开 `mbedtls_gcm_context`，改为 `psa_aead_encrypt/decrypt` + `PSA_ALG_GCM`；tag 由 PSA 追加后，组件再拼接自身的 nonce。
2. **`TcpConn` 的收发缓冲放在堆上**（约 12 KiB），不占用 I/O 任务栈；I/O 任务栈因此保持 6 KiB。
3. **TOML 限额的强制点**：嵌套深度与累计分配字节在解析过程中强制；语法树节点/数组/表数量在解析后校验（字节预算已是硬上限，故内存风险已被覆盖）。
4. **`Started` 事件在控制面任务内上报**，而不是在 `start()` 返回时同步回调，保证回调上下文一致。
5. **`originally` 计划中的 `easytier_esp32.h` C API 不再提供**：公开接口全部为 C++（按用户要求）。
6. 组件私有源码中的 `easytier-esp.cpp` 占位文件已删除，实现位于 `src/` 下。
7. **RPC 重组缓冲取 8 KiB/槽位**：为支持乱序到达，每个槽位需要"到达顺序"与"片号顺序"两份缓冲，因此把单条 RPC 消息上限从 16 KiB 收紧到 8 KiB（两个槽位合计约 32 KiB，与设计预算一致）；超过上限的消息按 ResourceLimit 拒绝。
8. **M2 的路由同步请求只带最小头部**（`my_peer_id`/`my_session_id`/`is_initiator`），不携带 `peer_infos`/`conn_info`：M2 只负责打通 RPC 传输与压缩协商，M3 在同一入口补齐拓扑与会话语义。服务端在 M2 返回空响应（proto3 合法，但 `session_id=0`，上游可能在日志中提示会话不匹配）。
9. **同步请求携带"已知的整个拓扑"**：`peer_infos` 与 `conn_list` 都包含本节点已知的全部节点与邻接（受 `kMaxPeerInfosBytes`/`kMaxConnListBytes` 限制，放不下时按条目递减），这是多跳收敛的前提——只宣告自身会导致 A 永远学不到 B 另一侧的 C。邻接行保存原始 peer id（不过滤未知节点），等待 peer_info 补齐后再自然进入建图。
9. **并发入口连接数为 2**（`kMaxConnSlots`）：配置可声明最多 10 个入口，前 2 个各占一个连接槽并发连接，其余入口作为失败时的轮换备用；每槽独立代际、退避、握手与保活。帧选路：控制帧按"代际 → 槽"，数据面/转发的帧按 `to_peer`（下一跳）选槽，找不到目标连接则计数丢弃（不回队，避免队头阻塞）。按延迟选路的 ECMP 不在范围内。
10. **IP 分片报文直接丢弃**：本组件不做 IPv4 分片重组，虚拟 MTU 1380 由上层按 MTU 分片即可。
11. **出站广播/组播丢弃**（与设计一致）：`ipv4_is_broadcast_or_multicast` 显式判定并计数，不会当作单播发往某个 peer。
12. **dhcp 不提供服务端**：只做本机自分配（对齐上游 `DhcpIpv4Allocator`），并且启用 dhcp 时默认子网总是存在（配置值或内置 10.126.126.0/24），先等两轮通告再回落。
13. **物理网段重叠检查**：创建网卡前与物理 STA 网卡的 IP/掩码比较，重叠时拒绝创建并上报错误（避免虚拟流量被物理网卡直发）。
14. **netif 出站回调只 ref+入队**：`pbuf` 由 TCPIP 线程 `pbuf_ref` 后入队（深度 8），数据面任务负责展平、校验、加密与释放引用；`output` 绝不做加密。
9. **avoid_relay 的代价用"惩罚跳数"近似**：上游把这类节点的出边代价设为 `i32::MAX`，本组件用 `kAvoidRelayPenalty = 0x4000` 跳惩罚（效果相同：除非无路可走，否则不经过它），避免 16 位跳数溢出。
10. **路由快照的 IP 映射包含自身 IP**：数据面用它判断"目的 IP 即本机"，从而决定投递到 lwIP 还是继续转发。
11. **反向同步限速 1 秒**：应答方在合并对方拓扑后会主动发送自己的同步请求（响应报文本身不携带拓扑），限速用于避免同步风暴；上游使用更复杂的会话/版本机制，M3 用"脏标记 + 限速"达到同等效果。
12. **本节点的邻接行目前只列出已握手的直连对端**（当前实现为单连接）；多直连（最多 10 个入口）在 M4 与多连接管理一起扩展。
13. **拓扑容量固定为 32 个节点**：超出时 `merge_peer` 返回 `RejectedFull` 并计数，不会静默丢弃后仍报告成功。
9. RPC 对**直连对端一律加密**（与上游 `PeerManager::send` 的判定一致：直连且无 RoutePeerInfo 视为非 public server），握手与 Ping/Pong 始终明文。

### 14.3 尚未实现（后续里程碑）

- **M3**：双向 `SyncRouteInfo` 的 `peer_infos`/`conn_info`（list 与 bitmap）、拓扑合并、版本/会话与 missing 补发、Dijkstra 与下一跳快照。因此 `Status::reachable_route_count` 目前恒为 0，`NetworkReady` 事件尚未上报，M2 发出的同步请求只是最小头部。
- **真机验证**：M4 的 lwIP 网卡、pbuf 所有权、dhcp 换址与端到端 ICMP/TCP/UDP 需要在 ESP32-S3 上实际运行（需要 Linux 入口节点配合），这是 M4 剩余的验收步骤。
- **多入口并发连接**已实现（2 个连接槽），但尚未在真机上验证；按延迟选路的 ECMP 与超过 2 条的并发连接不在当前范围。
- **M5/M6**：UDP mux、STUN 与打洞。UDP 落地后需要重新评估 RPC 重组：当前实现已按片号顺序组装（支持乱序到达），但 8 KiB 上限来自两份缓冲的取舍。
- **真实 EasyTier 节点的互操作验证**（握手、RPC 压缩协商、路由同步、数据面）需要至少一台 Linux 入口节点，这是 M1~M4 的共同验收前提，目前尚未执行。
- M2 的"链路 5 分钟稳定"与"含 zstd 能力的上游完成 None 协商"两项验收同样依赖真实对端。
- M3 的"Linux 学到 ESP IP/邻接""三节点下一跳正确"两项验收同样依赖真实对端；宿主侧已用上游黄金样本覆盖等价的数据流。
## 保活与超时参数（与上游对齐）

| 参数 | 取值 | 上游对应 | 说明 |
| --- | --- | --- | --- |
| 保活发送间隔 | 1/2/4/8/16/32 秒 | `max_ping_interval()`：直连/监听为 32 秒，仅 TCP 打洞为 1 秒 | 每次成功往返翻倍，上限 32 秒；失败后立刻回到 1 秒快速探测 |
| Ping 响应超时 | **固定 2 秒** | `timeout(Duration::from_secs(2), ...)` | 与发送间隔无关 |
| 连续失败关闭 | **5 次** | `loss_counter >= 5` 即关闭连接 | 只有匹配的 Pong 清零计数；**入站数据流量不算存活** |
| 握手超时 | 5 秒 | — | 超时后重新连接该入口 |
| 路由同步间隔 | 5 秒 | — | 另有限速 1 秒的反向宣告 |
| RPC 事务超时 | 10 秒 | — | 超时回收事务与未完成的组装 |

关闭动作统一走"I/O 关闭连接 → 代际作废 → 控制面复位该槽 → 同步引擎撤销该对端与路由"，
因此保活失败与 TCP 断开走的是同一条恢复路径。

## 15. 任务栈预算的静态校验

设备侧三个任务的栈大小（I/O 6 KiB、控制面 10 KiB、数据面 8 KiB）是设计里的硬约束，
但宿主测试完全覆盖不到"栈是否够用"，而真机上的栈溢出表现为难以定位的崩溃。为此本仓库
提供 `tools/check_stack_usage.py`：读取编译器 `-fstack-usage` 数据（每个函数的栈帧字节数）
并基于 ELF 反汇编构建真实调用图，计算从三个任务入口出发的**最坏调用链**，与代码中声明的
栈大小比较。

```bash
idf.py -DEASYTIER_STACK_USAGE=ON build
python3 tools/check_stack_usage.py     # 退出码非 0 表示有任务超预算
```

当前结果（最坏链 / 预算，单位字节）：

| 任务 | 最坏链 | 预算 | 余量 |
| --- | --- | --- | --- |
| `et_io_task_entry` | 2400 | 6144 | 3744 |
| `et_proto_task_entry` | 5632 | 10240 | 4608 |
| `et_data_task_entry` | 4400 | 8192 | 3792 |

顺带实测了常驻内存：`sizeof(Context)` = 99,248 字节（RPC 重组 33,184 + `SyncEngine` 38,512
+ RPC 收发缓冲约 14,000 + 其它），加上帧队列约 24 KiB、`TcpConn` 约 12 KiB、任务栈 24 KiB，
单实例常驻约 160 KiB，因此示例默认启用 PSRAM，并用 `static_assert(sizeof(Context) < 112 KiB)`
与启动日志守住这条预算。

工具会额外列出"组件内最大的若干栈帧"：调用链分析依赖反汇编能否解码全部 call/j（间接调用
与部分入口壳无法解码时会漏边），大帧清单是另一道保险——`Instance::init`（7,792 B）与
`init_from_toml`（3,312 B）正是这样被发现的：它们运行在**调用方**的栈上，而示例原来在
2,304 B 的系统事件任务里调用，真机必然溢出。现在两者的大对象都放在静态缓冲，示例把初始化
放进 8 KiB 的独立任务；入口壳无法解码的问题则通过"从 C++ 任务函数开始分析、再叠加入口壳
自身的帧"解决。

该工具还在更早的阶段发现并促成修复了一处**必然崩溃**：`SyncEngine::build_request` 内部曾有一个
9.4 KiB 的栈帧（32 行邻接表的构建与"放不下时再复制一份"的副本），叠加控制面任务自身的
4.3 KiB 帧后最坏链达到 14.7 KiB，超过 10 KiB 预算——首次路由同步即会栈溢出。
修复方式是把两个 `ConnList`（各约 4.6 KiB）移到 `SyncEngine` 成员、并去掉那份副本；
`Context` 另加 `static_assert(sizeof(Context) < 96 KiB)` 防止常驻内存悄悄膨胀。

## 16. 设备侧测试工程（test_apps/easytier_esp_tests）

宿主测试覆盖了协议与算法逻辑，但**设备上的实际实现**（mbedTLS/PSA 的 AES-GCM、lwIP 的
netifapi/tcpip_input 路径、真实任务栈与内存占用）只有烧录后才能验证。为此本仓库提供一个
设备侧测试工程，它与主工程共用同一批测试源码，因此设备与 PC 跑的是**同一批断言**。

```bash
# 构建
idf.py -C test_apps/easytier_esp_tests build
# 烧录并查看结果（115200 波特率）
idf.py -C test_apps/easytier_esp_tests -p /dev/ttyACM0 flash monitor
```

- **用例组成**：152 个用例 = 142 个与宿主完全相同的用例（密码学/线格式/protobuf/握手/
  RPC 分片/路由同步/拓扑/dhcp/数据面/TOML 配置/保活时序）+ 10 个设备专用用例；
  仅宿主运行的 15 个用例是 4 个 VFS 文件读取、8 个本机 socket 测试（设备上无 /tmp
  与 127.0.0.1 监听）与 3 个示例配置解析用例（按源码路径读取 `main/easytier.toml`）。
- **设备专用用例**：
  - `target_psa_aes_gcm_matches_nist_vector`：用设备实际的 PSA Crypto 复现 NIST
    AES-128-GCM 向量与 EasyTier 的 `密文||tag||nonce` 尾部布局，并验证篡改被拒绝；
  - `target_virtual_netif_lifecycle`：虚拟网卡创建/改址/上下链/销毁（不依赖 WiFi，
    只需 esp_netif_init 建好的 TCPIP 线程）；
  - `target_virtual_netif_overlap_check`：虚拟网段与物理网段重叠判定。
- **实现要点**：每个用例在自己的 64 KiB 栈任务里运行（最大的用例在栈上构造 3 个
  `SyncEngine`，约 75 KiB），失败上报只打印计数、不跨任务 longjmp；测试 main 组件使用
  `WHOLE_ARCHIVE`，否则只靠静态注册对象自我登记的用例会被链接器丢弃（表现为 0 个用例）。
- **当前状态**：构建通过（镜像 `0x7ca10`，1 MiB 分区剩余 51%），**尚未烧录执行**——
  执行需要开发板；结果应当是 `=== 通过 152 / 152，失败 0 ===`。

