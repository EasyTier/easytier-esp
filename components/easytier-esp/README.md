# easytier-esp

把 ESP32 设备接入一个既有 EasyTier 虚拟网络的 ESP-IDF 组件（C++20，公开接口全部为 C++）。

- **仅适配 ESP-IDF 6.1.x**（`idf_component.yml` 声明 `>=6.1,<6.2`），首版在 ESP32-S3 上验证。
- 协议基线：本地 EasyTier 提交 `32dc7f1f406e5fe43ba095a918a7a1c4b28f2335`（`v2.7.0-24-g32dc7f1f`）。
- 遗留问题与精简待办：[docs/遗留问题清单.md](../../docs/遗留问题清单.md)。

## 职责边界

| 由组件负责 | 由应用负责 |
|---|---|
| 连接入口节点、握手、加密、RPC、路由同步、虚拟网卡、任务与缓冲、状态事件 | NVS、`esp_netif`、默认事件循环、WiFi/以太网、文件系统挂载、业务 socket |

组件不初始化 WiFi、不挂载文件系统、不修改物理默认网卡。

## 当前实现状态

已实现（有宿主测试与 ESP-IDF 构建证据）：

- 与上游核心相同格式的 TOML 配置（`et::Inst` 构造函数直接接受 TOML 文本，应用自行读文件）：
  支持的字段必须写对，不支持的字段不解析也不报错（对齐上游 serde 行为）；
- SipHash-1-3 密钥派生、AES-128-GCM（PSA Crypto）、PeerManagerHeader/分帧等线格式；
- 有界 protobuf 编解码（含未知字段跳过、packed repeated、负整数、optional 存在性）；
- 非阻塞 TCP 传输：非阻塞连接、按帧重组（拆包/粘包）、部分写游标与背压、代际隔离；
- 非安全模式握手（网络名/摘要/peer id 冲突校验）、Ping/Pong 报文与保活退避；
- RPC：按上游 UDP 预算（1300 字节，每片编码 ≤ 1248）分片、乱序重组、事务匹配、
  服务注册/分发、控制面 AEAD 加密，以及压缩协商（只接受 None，收到 Zstd 视为不支持）；
- 路由同步：`RoutePeerInfo`/邻接表/邻接位图（LSB-first）编解码与上游逐字节一致，
  双向主动同步、版本合并、missing 补发、90 秒退出建图 / 3600 秒删除、
  有向图 Dijkstra（avoid_relay 惩罚）与下一跳/IP 路由快照，可达后上报 `NetworkReady`；
- 三个任务、队列与有界资源、`et::Inst` RAII 实例的完整生命周期
  （构造=解析 TOML+校验+分配+派生密钥，析构=自动停止并释放；**允许并存多个实例**）；
- **唯一的公开类 `et::Inst`**：没有独立的配置类，全部设置项都是实例私有实现
  （内部 `Settings`：`std::string`/`std::vector`，缺省值 + TOML 全量替换）、身份补齐
  （hostname→instance_name、随机 UUID v4）；解析失败与忽略项直接输出日志，不缓存诊断；
- `SyncEngine`：路由同步的全部纯逻辑（拓扑合并、最短路、dhcp、请求/响应编解码）
  与任务解耦，因此**可以在宿主测试里让多个引擎互相对话**，验证两节点互通与
  A—B—C 经中继收敛；
- 数据面：lwIP raw L3 虚拟网卡（`netifapi` + `tcpip_input`，不与物理网段重叠）、
  pbuf 出站队列（TCPIP 线程只 ref+入队）、目的 IP → 目标节点 → 下一跳查表、
  源节点加密（AES-128-GCM 尾布局）、入站本机投递 / 按下一跳转发（保留目的与密文、
  计数防环）、转发计数与丢弃统计；非本地 RPC 也按 overlay 路由转发；
- dhcp 自分配虚拟 IPv4（对齐上游 `DhcpIpv4Allocator`：优先沿用当前子网、避开已用
  地址、先等两轮再回落默认子网），地址变化会即时更新网卡并重新通告；
- `main` 示例：NVS + esp-netif + WiFi STA 重连 + 内嵌 TOML + underlay 通知。

尚未实现（按设计文档分阶段推进）：

- 真机验证：虚拟网卡的 pbuf 所有权、dhcp 换址、经中继的端到端 ICMP/TCP/UDP 需要在
  ESP32-S3 上实际运行（需 Linux 入口节点）；
- 按延迟选路的 ECMP（并发入口连接数由 Kconfig 的 `EASYTIER_ESP_MAX_CONNECTED_PEERS`
  控制，默认 6；配置里的其它入口轮换备用）；
- UDP mux、NAT 打洞；
- Noise/credential、共享公共中继/foreign network、IPv6、子网代理、出口节点、
  Magic DNS、KCP/QUIC、管理界面；
- 与真实 EasyTier 节点的互操作验证（需要一台 Linux 入口节点）。

入口必须是**同网络、相同密钥**的节点；它可以是公网节点，但不能是任意 EasyTier 公共中继。

## 配置

组件直接读取上游核心格式的 TOML，例如：

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

字段策略与上游 `easytier-core/src/config/toml.rs` 一致：**支持的字段必须写对，不支持的
字段根本不解析也不报错**。

1. **支持的字段（必须正确填写）**：`instance_name`、`hostname`、`instance_id`、`ipv4`、
   `dhcp`、`network_identity.network_name/network_secret`、`[[peer]].uri`（`tcp://`，
   **条目数量无上限**）、`flags.mtu`（576..1380）、`flags.enable_encryption`（布尔，
   缺省 `true`，别名 `enableEncryption`）。字段存在但类型错误或取值非法（UUID/CIDR/
   MTU 越界/URI 非法等）→ **构造失败**（Error 日志 + `last_error`）。
   唯一例外是 `ipv4`：字符串类型正确但内容无法解析时，与上游 `get_ipv4().parse().ok()`
   一样**静默忽略**（视为未配置静态地址）。
2. **不支持的字段（不解析、不报错）**：未知键，以及上游有但本组件未实现的字段——
   `listeners`、`mapped_listeners`、`routes`、`proxy_network`、`port_forward`、`exit_nodes`、
   `secure_mode`、`acl`、`vpn_portal_config`、`ipv6*`、`netns`、STUN/日志相关字段、
   `flags` 里的其它标志（`disable_p2p`、`encryption_algorithm`、`data_compress_algo`……）、
   `peer` 表里的其它键（如 `peer_public_key`）。它们**既不解析也不校验**，写错也不会报错。
3. **缺失的字段**保留组件缺省值（`network_name` 缺省 `default`、`instance_id` 随机生成、
   `hostname` 回退 `Espressif` 等）。
4. **peer 只能写对**：非数组/非表、缺少 uri、uri 类型错误、无效主机或端口、非 TCP 协议、
   IPv6 均报错；不能通过另一个有效 peer 掩盖错误。入口 URI 校验只有这一份实现。

入口**数量**不设上限；**同时连接**的数量由 Kconfig 的 `EASYTIER_ESP_MAX_CONNECTED_PEERS`
决定（默认 6），其余入口作为轮换备用。

文本字段（实例名 / 主机名 / 网络名 / 网络密钥 / 入口主机）同样**没有固定长度上限**：
它们用 `std::string` 按实际长度在堆上分配（短字段走 SSO）；TOML 文本本身也没有长度上限。
唯一保留的上游规则是 hostname 规范化后最多 32 个 Unicode 码点。

`inst.get_config_info()` 返回的是 **`et::Settings` 深拷贝**：没有定长缓冲、不会截断，
入口列表按实际条数返回；其中 `network_secret` 字段在返回前已被清零（恒为空串），
因此可以安全打印。

TOML 语法错误、支持字段取值非法、解析嵌套/语法树超限都会让 **`et::Inst` 构造失败**
（`valid()==false`，错误码见 `inst.last_error()`）。失败原因由组件以 **Error 级日志**
直接输出（含出错行号或字段路径），**不做缓存**。
空文档、无关字段、缺少入口或虚拟地址都可以解析；解析成功不代表组网成功——入口与虚拟地址的
结构校验同样在构造时执行。TOML 解析使用进程级解析器选项，因此**构造必须串行调用**
（不能跨任务并发构造）；并发解析会被直接拒绝并返回 `ESP_ERR_INVALID_STATE`。

字段名、层级、默认网络身份、IPv4 /32→/24、DHCP 布尔/CIDR 形式参考上游
`easytier-core/src/config/toml.rs`。

`dhcp` 接受布尔或 IPv4 CIDR 字符串（前缀 0..=30，越界或格式错误即报错）；配置了静态
`ipv4` 时 `dhcp` 被忽略（与上游"静态地址优先"一致）。

## 支持字段的缺省值

上游共享 core 把配置解析与 Host 环境回退分开处理。以下在初始化后的生效配置中可见，
不会为上游本来为空的配置凭空创建入口或静态地址。

| 字段 | 未传入时 | 依据 |
| --- | --- | --- |
| `instance_name` | 取 `hostname`（按用户要求：未显式配置时优先用 hostname） | 显式配置的 `instance_name` 优先 |
| `instance_id` | 构造 `et::Inst` 时生成并缓存随机 UUID v4 | 上游 `toml.rs::get_id()`；显式 UUID（含 nil UUID）保持原值 |
| `hostname` | `Espressif` | 构造 `et::Inst` 时即补齐；上游优先使用非空配置 hostname，否则取 Host fallback，ESP fallback 按用户要求设为此值 |
| `network_identity.network_name` | `default`（未写 `[network_identity]` 表时） | 上游 `NetworkIdentity::default()`；**一旦出现该表就是必填字段**（与上游 serde 一致），缺失/空串/非字符串都报错 |
| `network_identity.network_secret` | 空字符串 | 上游普通模式身份归一化；不是随机密钥。表出现时可选 |
| `ipv4` | 不设置静态地址 | 上游 `get_ipv4()` 缺省 `None`；裸地址或 /32 归一化为 /24；**字符串无法解析时静默忽略**（与上游 `.parse().ok()` 一致） |
| `dhcp` | `false` | 上游 `DhcpConfig::default()` |
| DHCP 自举网段 | 启用 DHCP 且未学到可用网段时回落 `10.126.126.0/24` | 上游 `DhcpIpv4Service`；可能先采用同网络其他节点宣告的网段 |
| `peer` | 空列表 | 上游 `get_peers()` 缺省空列表；指定的入口必须校验通过 |
| `flags.mtu` | `1380` | 上游 `common.proto` 的 flags 默认值 |
| `flags.enable_encryption` | `true` | 上游 `common.proto` 中 `enable_encryption` 的 `default: "true"`；关闭时数据与 RPC 报文不带 AEAD 尾与加密位，并拒绝收到的已加密报文（对齐上游 `NullCipher`） |

## 本次真机崩溃修复

`dhcp assigned virtual ip` 后崩溃的调用链落在 `esp_netif_internal_dhcpc_cb`：
IDF 6.1 默认把 lwIP `netif->state` 当作 `esp_netif_t*`，原实现却在此保存
`VirtualNetif*`，导致地址变化回调访问错误对象。raw 虚拟网卡现在保持 `state=nullptr`，
通过标准布局首成员找回自身对象，继续使用 `netifapi_*` 与 `tcpip_input`。
设备侧生命周期/改址测试检查该隔离关系；仍需用户上板执行确认。

单入口不再被两个连接槽同时连接，相同 host:port 已占用时不会再次选用。
每槽重试使用独立截止时间，避免等待备用入口时阻塞现有连接的收发与保活。

后续上板日志已确认虚拟网卡创建成功，但出现 IDLE 看门狗超时：FreeRTOS 100 Hz 时，
5 ms 转换为 0 tick，原控制面/数据面循环持续轮询。两任务现在至少阻塞 1 tick，且每轮
队列处理限额为 8 条，持续流量下也会让出 CPU；不通过关闭看门狗来规避问题。
DHCP 网卡创建/改址成功后同步更新公开 IPv4/前缀，再发送 NetworkReady，避免 `vip=none`。
新调度行为和状态输出仍需用户上板确认。若监控提示 ELF checksum mismatch，应先确认
运行固件与本地 ELF 一致；不一致时自动反解的函数名与行号不可作为新固件的崩溃证据。

## 用法

运行期实例是 `et::Inst`（**RAII 类，也是组件唯一的公开类**）：构造即解析 TOML、补齐实例
身份、校验结构并在堆上分配上下文，析构即停止任务并释放资源。**没有独立的配置类**——全部
设置项都是实例的私有实现细节，构造后不可修改。组件内部也**没有**全局单实例状态，因此
**允许同时存在多个实例**——每个对象独占自己的上下文、队列、任务、socket、虚拟网卡与事件
回调。组件**不接受文件路径**——应用自己读取文件（或像示例那样用 `EMBED_TXTFILES` 内嵌），
再把 TOML 文本交给构造函数。

> 示例的 `main/easytier.toml` 含真实入口地址与网络密钥，**已被 `.gitignore` 忽略、
> 也不在任何提交历史里**，仓库里也不提供配置模板。新克隆后请按本 README 的
> [配置](#配置) 一节自己创建 `main/easytier.toml`；文件不存在时示例只连 WiFi、
> 不启动组网，并在日志里提示。

```cpp
#include "easytier-esp.h"
#include <optional>
#include <string_view>

// 示例只创建一个实例；组件本身支持多个 Inst 并存。
static std::optional<et::Inst> inst;

extern const uint8_t easytier_toml_start[] asm("_binary_easytier_toml_start");
extern const uint8_t easytier_toml_end[] asm("_binary_easytier_toml_end");

void start_easytier() {
    auto len = static_cast<size_t>(easytier_toml_end - easytier_toml_start);
    if (len != 0 && easytier_toml_start[len - 1] == 0) --len; // 排除 EMBED_TXTFILES 终止符

    et::RuntimeOptions options{};
    options.on_event = &on_easytier_event;
    // 构造即初始化（TOML 解析 + 校验 + 分配）。组件禁用异常：失败通过 valid()/last_error()
    // 查询；失败原因已由组件直接写入 Error 日志。
    inst.emplace(std::string_view(reinterpret_cast<const char*>(easytier_toml_start), len),
                 options);
    if (!inst->valid()) { /* 看 inst->last_error() */ inst.reset(); return; }
    if (inst->start() != ESP_OK) { inst.reset(); return; }
    // 物理网络取得 IP 后通知组件；断网时传 false。
    (void)inst->set_underlay_ready(true);
}
```

上面的 `_binary_easytier_toml_*` 符号只在 `main/easytier.toml` 存在时才生成；仓库里的示例用
`#if defined(EASYTIER_EXAMPLE_TOML_EMBEDDED)` 包住这段代码。已挂载文件系统时由应用自行读取，
例如 `fopen`/`fread` 成缓冲区后把文本交给 `Inst` 构造函数。

### 多实例

- 多个 `et::Inst` 可同时运行，互不共享状态；每个实例各自调用 `start/set_underlay_ready/stop`。
- 每个实例的常驻内存、任务栈与连接槽都独立计算，N 个实例线性叠加（见「内存预算」）。
- 应用需自行避免**虚拟 IP / 虚拟网段冲突**：两个实例的虚拟地址或子网重叠会破坏路由。
- `RuntimeOptions::user_data` 与事件回调属于各自实例，实例 `stop()` 返回后才可释放其 `user_data`。

### 设置与身份补齐

- TOML 是**唯一配置入口**，也没有编程式 setter：要改设置就重新构造一个 `Inst`。
- 构造总是"缺省值 + TOML"：先取各字段的缺省值，再由 TOML 逐字段覆盖，最后补齐身份。
  因此**随机生成的缺省值每次都会重新生成**——TOML 里没写 `instance_id` 时每次构造都会得到
  新的 UUID v4；`hostname` 回退 `Espressif`，`instance_name` 按 hostname 推导。
  TOML 里显式给出的 `instance_name` / `instance_id` 仍然优先。
- 设置项全部是 `Inst` 的私有实现，应用无法直接读写；可读的只有 `get_config_info()`
  返回的 `et::Settings` 副本（`network_secret` 恒为空）；解析诊断不缓存，直接进日志。

生命周期约定：

- `et::Inst` 是 RAII 对象：**构造即初始化**（解析 TOML、补齐身份、校验结构、分配上下文、
  派生密钥），失败时 `valid()==false`、原因见 `last_error()`，析构对失败对象安全。
- **析构即释放**：若实例仍在运行，析构先做无限等待的停止，再释放队列/连接/AEAD 并清零密钥；
  也可以先显式 `stop(timeout_ms)` 观察超时，再销毁对象。
- 同一实例的 `start/stop`/析构必须**串行**；不同实例互不影响。构造会解析 TOML 并分配约
  91 KiB 的上下文，必须在栈足够大的任务里执行（示例用 8 KiB 启动任务）。
- 事件回调在控制面任务中执行，必须快速返回，且不得调用 `stop()` 或析构该实例。

## 资源上限

编译期上限通过 Kconfig 配置（`CONFIG_EASYTIER_ESP_*`）：**同时连接**的入口数、组件日志
等级、并发 RPC 事务、分片重组槽与各队列深度。容量不足时返回明确错误码并在诊断中体现，
不会静默截断。

**TOML 文本没有长度上限、也没有解析堆预算**：组件直接解析应用传入的文本（文本由应用以
`std::string`/`std::string_view` 持有），不复制、不按固定容量截断；解析器分配失败由
tomlc17 自身以 "out of memory" 报错并映射为 `ESP_ERR_NO_MEM`。仍然保留的是语法树规模
限额（节点数/数组元素数/表键数、嵌套深度、dotted key 分段数），用于拒绝异常放大的文档。
入口地址**条目数不受这些上限约束**：列表按实际条数在堆上分配。
`EASYTIER_ESP_MAX_CONNECTED_PEERS` 只决定同一时刻建立多少条连接。

## 线程与回调约束

| 接口 | 可从任意任务 | 可在事件回调中 | 说明 |
| --- | --- | --- | --- |
| `inst.last_error` | 是 | 是 | 只读构造阶段的错误码 |
| `inst.get_status` / `get_config_info` | 是 | 是 | 只读原子量 / 深拷贝 `Settings`，与实例无共享 |
| `inst.set_underlay_ready` | 是 | 是 | 非阻塞（队列投递），物理网络事件里直接调用 |
| `et::Inst` 构造 | 仅初始化阶段（大栈任务，串行） | 否 | 解析 TOML 并分配约 91 KiB 上下文 |
| `inst.start` / `stop` / 析构 | 同一任务串行 | **否** | 会等待组件任务退出 |

- 同一实例的生命周期接口必须**串行**调用；不同实例互不影响。
- 事件回调运行在**控制面任务（`et_proto`）**里且是同步回调，因此必须快速返回；在回调里
  调用 `stop()` 会等待该任务自身、最终以 `ESP_ERR_TIMEOUT` 返回，析构则会永久阻塞。
  `get_status()`/`get_config_info()`/`set_underlay_ready()` 在回调里可安全调用。
- `stop()` 返回后任务已全部退出、不会再有任何回调，此时才可以释放 `user_data` 指向的对象。
- 也可以直接销毁对象（析构自动停止并释放）；注意析构没有超时，必须确保不在回调中执行。
- 示例应用遵循同样的约束：WiFi 事件里只调用非阻塞的 `set_underlay_ready()`，而把
  `et::Inst` 构造 + `start()` 放在独立的 8 KiB 任务里执行
  （见 [`main/easytier_app.cpp`](../../main/easytier_app.cpp)）。

## Kconfig 可调项

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `EASYTIER_ESP_LOG_LEVEL` | Info | 组件日志编译等级：Closed、Error、Warning、Info、Debug；低于所选等级的调用会在编译期裁剪，Closed 裁剪全部日志 |
| `EASYTIER_ESP_MAX_CONNECTED_PEERS` | 6 | **同时连接**的入口节点数上限（1..8）。配置里的入口条目数不受此限制，多出的作为轮换备用；每槽约 12 KiB 收发缓冲 |
| `EASYTIER_ESP_RPC_MAX_TRANSACTIONS` | 4 | 并发 RPC 事务上限 |
| `EASYTIER_ESP_RPC_REASSEMBLY_SLOTS` | 2 | RPC 分片重组槽数量（每槽约 16 KiB） |
| `EASYTIER_ESP_QUEUE_RX_CTRL_DEPTH` | 8 | I/O → 控制面的帧队列 `q_rx_ctrl` 深度（1..32，每元素约 2 KiB） |
| `EASYTIER_ESP_QUEUE_TX_DEPTH` | 8 | 控制面/数据面 → I/O 的发送队列 `q_tx` 深度（1..32，每元素约 2 KiB） |
| `EASYTIER_ESP_QUEUE_RX_DATA_DEPTH` | 8 | I/O → 数据面的 `q_rx_data` 深度（1..32，每元素约 2 KiB） |
| `EASYTIER_ESP_QUEUE_SMALL_DEPTH` | 8 | 事件/命令队列 `q_evt`/`q_io_cmd`/`q_app_cmd` 共用深度（1..32） |
| `EASYTIER_ESP_QUEUE_NETIF_DEPTH` | 16 | TCPIP → 数据面的 pbuf 队列 `q_netif_rx` 深度（1..64） |

> 队列深度单列在 menuconfig 的 **EasyTier-ESP → 队列深度** 菜单里；每加深一格，
> 帧队列就多占约 2 KiB 常驻堆内存（`q_snapshot` 固定深度 1，不提供选项）。
> 旧版本的帧队列默认是 4，突发路由同步/RPC 下容易溢出（丢帧并计入 `dropped`），
> 因此新默认调到 8；内存紧张或流量很大时按需增减。

宿主构建没有 sdkconfig，使用 `support/config_limits.hpp` 里的同一批默认值
（Kconfig 的 `range` 之外还有 `static_assert` 兜底，越界会让固件编译失败）。

## 内存预算（实测，ESP32-S3）

下表是**单个 `et::Inst` 实例**的开销。多个实例并存时，`Context`、帧队列、连接槽与任务栈
都按实例数量线性叠加（每实例约 91 KiB 常驻 + 约 48 KiB 队列 + 连接槽 + 24 KiB 任务栈）。

| 项目 | 大小 | 说明 |
| --- | --- | --- |
| `Context`（堆） | 92,808 B | RPC 重组 33,184（2 槽 × 16 KiB）+ `SyncEngine` 35,240（拓扑 22,928 + 邻接表 8,968 + 快照 524 + 编码缓冲）+ RPC 收发缓冲约 14,000 + `Settings` 外壳 + 其它 |
| `Inst` 对象本体 | 8 B | 私有 `Context*` + `esp_err_t last_error_`（诊断与 `started_` 都不在 `Inst` 上）；设置项与运行状态在堆上的 `Context` 内 |
| `Settings` 文本字段（堆） | 按实际长度 | 实例名 / 主机名 / 网络名 / 网络密钥都是 `std::string`（短字段走 SSO），**长度无固定上限** |
| 入口地址列表（堆） | 每条约 32 B + 主机文本 | `std::vector<PeerEndpoint>`，按配置实际条数分配；条目数量无上限 |
| 帧队列 `q_rx_ctrl`/`q_tx`/`q_rx_data` | 默认约 47 KiB | 深度各 8（Kconfig `EASYTIER_ESP_QUEUE_*_DEPTH`）× 约 2 KiB；深度可直接换算成常驻内存 |
| 事件/命令队列 `q_evt`/`q_io_cmd`/`q_app_cmd` | 默认约 192 B | 深度各 8 × `SmallMessage` 8 B |
| 网卡出站队列 `q_netif_rx` + 快照 `q_snapshot` | 默认约 0.7 KiB | `q_netif_rx` 深度 16 × `NetifRxItem` 8 B；`q_snapshot` 固定深度 1 |
| `TcpConn`（堆） | 约 12 KiB × 连接数 | 每槽一份收发缓冲，槽数见 `EASYTIER_ESP_MAX_CONNECTED_PEERS`（默认 6） |
| 任务栈 | 24 KiB | I/O 6 KiB + 控制面 10 KiB + 数据面 8 KiB |
| TOML 解析 | 按文本规模（瞬时） | 无固定预算；分配失败由解析器报 OOM，受语法树/深度限额约束 |

因此**强烈建议启用 PSRAM**：示例 `sdkconfig.defaults` 已启用（大于 16 KiB 的分配会落到
PSRAM）。组件内用 `static_assert(sizeof(Context) < 112 KiB)` 防止常驻内存悄悄膨胀，
启动日志也会打印实际 `sizeof(Context)`。

## 构建与验证（不需要开发板）

激活 ESP-IDF 6.1 环境后直接构建，检查命令退出码：

```bash
idf.py build
```

当前工作区没有 `tests/` 和 `test_apps/`，不提供可执行的宿主/设备测试入口。
此前的测试记录仅代表历史验证，不作为本次构建已经执行测试的证据。
硬件运行与真实对端互操作由用户手动完成（代理不接触串口与烧录）。

## 任务栈预算校验

```bash
idf.py -DEASYTIER_STACK_USAGE=ON build
python3 tools/check_stack_usage.py
```

用 `-fstack-usage` + ELF 调用图计算三个任务入口的最坏调用链，与声明的栈大小比较
（当前：I/O 2400/6144、控制面 5632/10240、数据面 4400/8192 字节）。大缓冲一律放在
`Context`/`SyncEngine` 成员或堆上，任务栈上只保留小对象——这一点很容易在重构中被破坏，
所以该检查应当与宿主测试一起跑。

## 硬件联调

上板、烧录与串口监控由用户手动完成，代理不接触串口与烧录。要点：

1. 按 [配置](#配置) 一节自己创建 `main/easytier.toml`，填好网络名、网络密钥与入口地址；
2. 由用户执行 `idf.py -p <PORT> flash monitor`；
3. 示例启动后会打印**实际生效的配置**（`inst.get_config_info()`）与每 10 秒一行
   状态摘要，可与入口节点（Linux 侧 `easytier-cli`）的日志逐条对照。

> 原来的《硬件联调指南》与《验收证据清单》已按用户要求移除；需要时可按
> [docs/遗留问题清单.md](../../docs/遗留问题清单.md) 附录 B 取回。

示例在启动时会打印**实际生效的配置**（`inst.get_config_info()`），并每 10 秒打印一行
状态摘要，便于和入口节点日志对照：

```
I (3120) easytier-example: config: instance=esp32-demo network=esp-demo instance_id=...
I (3130) easytier-example: config: peer[0]=192.168.1.20:11010
I (3140) easytier-example: config: ipv4=10.144.144.10/24 mtu=1380
I (3150) easytier-example: easytier started, peer_id=0x1a2b3c4d
I (4120) easytier-example: peer connected: 0x5e6f7a8b
I (5120) easytier-example: network ready: 1 reachable peer(s)
I (5130) easytier-example: status: state=ready peer_id=0x1a2b3c4d vip=10.144.144.10/24 direct=1 routes=1 dropped=0 err=0x0
```

## 第三方代码

`third_party/tomlc17` 为 MIT 许可的 tomlc17（固定提交，公开函数符号加 `et_` 前缀以避免
与应用的其它 TOML 解析器冲突）；详见该目录下的 README 与 LICENSE。

## 许可与来源

从 EasyTier 改编的算法与线格式（SipHash-1-3 派生、摘要生成、报文布局）在源码注释中标明
上游依据；EasyTier 本体为 LGPL-3.0，使用本组件时请遵守相应许可证要求。
