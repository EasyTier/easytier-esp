# easytier-esp

把 ESP32 设备接入一个既有 EasyTier 虚拟网络的 ESP-IDF 组件（C++20，公开接口全部为 C++）。

- **仅适配 ESP-IDF 6.1.x**（`idf_component.yml` 声明 `>=6.1,<6.2`），首版在 ESP32-S3 上验证。
- 协议基线：本地 EasyTier 提交 `32dc7f1f406e5fe43ba095a918a7a1c4b28f2335`（`v2.7.0-24-g32dc7f1f`）。
- 设计文档：`docs/easytier-esp-port-design.md`。

## 职责边界

| 由组件负责 | 由应用负责 |
|---|---|
| 连接入口节点、握手、加密、RPC、路由同步、虚拟网卡、任务与缓冲、状态事件 | NVS、`esp_netif`、默认事件循环、WiFi/以太网、文件系统挂载、业务 socket |

组件不初始化 WiFi、不挂载文件系统、不修改物理默认网卡。

## 当前实现状态

已实现（有宿主测试与 ESP-IDF 构建证据）：

- 与上游核心相同格式的 TOML 配置（文本与已挂载 VFS 文件两种入口），不兼容参数忽略并报告；
- SipHash-1-3 密钥派生、AES-128-GCM（PSA Crypto）、PeerManagerHeader/分帧等线格式；
- 有界 protobuf 编解码（含未知字段跳过、packed repeated、负整数、optional 存在性）；
- 非阻塞 TCP 传输：非阻塞连接、按帧重组（拆包/粘包）、部分写游标与背压、代际隔离；
- 非安全模式握手（网络名/摘要/peer id 冲突校验）、Ping/Pong 报文与保活退避；
- RPC：按上游 UDP 预算（1300 字节，每片编码 ≤ 1248）分片、乱序重组、事务匹配、
  服务注册/分发、控制面 AEAD 加密，以及压缩协商（只接受 None，收到 Zstd 视为不支持）；
- 路由同步：`RoutePeerInfo`/邻接表/邻接位图（LSB-first）编解码与上游逐字节一致，
  双向主动同步、版本合并、missing 补发、90 秒退出建图 / 3600 秒删除、
  有向图 Dijkstra（avoid_relay 惩罚）与下一跳/IP 路由快照，可达后上报 `NetworkReady`；
- 三个任务中的两个（I/O、控制面）、队列与有界资源、`Instance` 完整生命周期
  （init/start/set_underlay_ready/get_status/stop/deinit）；
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

字段处理分三类：

1. **支持并应用**：`instance_name`、`hostname`、`instance_id`、`ipv4`、`dhcp`、
   `network_identity.network_name/network_secret`、`[[peer]].uri`（`tcp://`，**条目数量无上限**）、
   `flags.mtu`（576..1380）、`flags.enable_encryption`（布尔，缺省 `true`，别名
   `enableEncryption`）。
2. **忽略并报告**：`listeners`、`routes`、`proxy_network`、`port_forward`、`exit_nodes`、
   `secure_mode`、`acl`、`vpn_portal_config`、`ipv6`、STUN/日志相关字段，以及
   非 `aes-gcm` 算法、非 `None` 压缩、`disable_p2p=false`、
   其他非 peer 的不兼容取值。peer 的错误或不支持取值按第 4 项报错。
3. **非 peer 字段无法采用也忽略**：类型错误、无效 IPv4/UUID/DHCP/MTU、超过字段
   存储容量的值均跳过，保留默认值，其余有效字段继续提取。
4. **peer 只能写对**：非数组/非表、缺少 uri、uri 类型错误、无效主机或端口、
   非 TCP 协议、IPv6 均报错；不能通过另一个有效 peer 掩盖错误。
   peer 表内额外的无关字段仍忽略。TOML 与编程式 Config 使用同一入口 URI 校验。

入口**数量**不设上限（只受 TOML 文本长度与解析预算约束）；**同时连接**的数量由
Kconfig 的 `EASYTIER_ESP_MAX_CONNECTED_PEERS` 决定（默认 6），其余入口作为轮换备用。

TOML 语法错误、peer 配置错误或解析长度/深度/内存预算耗尽会返回错误；VFS 读取失败另报
读取错误。空文档、无关字段、缺少入口或虚拟地址都可以加载；加载成功不代表组网成功。
编程式 `init(Config)` 入口仍保留严格字段校验。

字段名、层级、默认网络身份、IPv4 /32→/24、DHCP 布尔/CIDR 形式参考上游
`easytier-core/src/config/toml.rs`。ESP 端按需求对字段转换采用更宽容的策略，
不会套用上游所有结构体的强类型反序列化约束。

忽略项通过 `et::ConfigReport` 返回（只记录字段路径，不含任何密钥取值）。

`dhcp` 接受布尔或 IPv4 CIDR 字符串（前缀 0..=30）；配置了静态 `ipv4` 时 `dhcp` 被忽略。

## 支持字段的缺省值

上游共享 core 把配置解析与 Host 环境回退分开处理。以下在初始化后的生效配置中可见，
不会为上游本来为空的配置凭空创建入口或静态地址。

| 字段 | 未传入时 | 依据 |
| --- | --- | --- |
| `instance_name` | `default` | 上游 `toml.rs` 的 `default_instance_name()` / `get_inst_name()` |
| `instance_id` | 初始化时生成并缓存随机 UUID v4 | 上游 `toml.rs::get_id()`；显式 UUID（含 nil UUID）保持原值 |
| `hostname` | `Espressif` | 上游优先使用非空配置 hostname，否则取 Host fallback；ESP fallback 按用户要求设为此值 |
| `network_identity.network_name` | `default` | 上游 `NetworkIdentity::default()` |
| `network_identity.network_secret` | 空字符串 | 上游普通模式身份归一化；不是随机密钥 |
| `ipv4` | 不设置静态地址 | 上游 `get_ipv4()` 缺省 `None`；裸地址或 /32 归一化为 /24 |
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

```cpp
#include "easytier-esp.h"

static et::Instance instance;
static et::ConfigReport report;

extern const uint8_t easytier_toml_start[] asm("_binary_easytier_toml_start");
extern const uint8_t easytier_toml_end[] asm("_binary_easytier_toml_end");

void start_easytier() {
    auto len = static_cast<size_t>(easytier_toml_end - easytier_toml_start);
    if (len != 0 && easytier_toml_start[len - 1] == 0) --len; // 排除 EMBED_TXTFILES 终止符
    ESP_ERROR_CHECK(instance.init_from_toml(
        std::string_view(reinterpret_cast<const char*>(easytier_toml_start), len), {}, &report));
    ESP_ERROR_CHECK(instance.start());
    // 物理网络取得 IP 后通知组件；断网时传 false。
    ESP_ERROR_CHECK(instance.set_underlay_ready(true));
}
```

已挂载文件系统时改用 `instance.init_from_file("/spiffs/easytier.toml", {}, &report)`。

生命周期约定：

- `Instance` 不可复制、不可移动；首版只允许一个已初始化实例。
- `init*()` 会复制所有配置视图，返回后调用方可以释放这些视图指向的对象。
- 事件回调在控制面任务中执行，必须快速返回，且不得调用 `stop()/deinit()` 或销毁实例。
- 析构会请求停止并等待任务退出（没有超时），建议应用显式 `stop()` 以便观察超时。

## 资源上限

编译期上限通过 Kconfig 配置（`CONFIG_EASYTIER_ESP_*`）：**同时连接**的入口数、组件日志
等级、并发 RPC 事务、分片重组槽、TOML 文本长度与解析内存预算。容量不足时返回明确错误码
并在诊断中体现，不会静默截断。

入口地址**条目数不受这些上限约束**：列表按实际条数在堆上分配，能写多少只取决于 TOML
文本长度（`EASYTIER_ESP_TOML_MAX_BYTES`）与解析预算（`EASYTIER_ESP_TOML_HEAP_BUDGET`）。
`EASYTIER_ESP_MAX_CONNECTED_PEERS` 只决定同一时刻建立多少条连接。

## 线程与回调约束

| 接口 | 可从任意任务 | 可在事件回调中 | 说明 |
| --- | --- | --- | --- |
| `get_status` / `get_config_info` | 是 | 是 | 只读原子量与不可变配置，返回副本 |
| `set_underlay_ready` | 是 | 是 | 非阻塞（队列投递），物理网络事件里直接调用 |
| `init*` | 仅初始化阶段 | 否 | 全组件只允许一个实例 |
| `start` / `stop` / `deinit` | 同一任务串行 | **否** | 会等待组件任务退出 |
| 析构 | 否 | **否** | 无限等待任务退出 |

- 生命周期接口必须**串行**调用；不要在其它任务正在 stop/deinit 时销毁实例。
- 事件回调运行在**控制面任务（`et_proto`）**里且是同步回调，因此必须快速返回；在回调里
  调用 `stop()` 会等待该任务自身、最终以 `ESP_ERR_TIMEOUT` 返回，带无限超时的析构则会
  **永久阻塞**。`get_status()`/`get_config_info()`/`set_underlay_ready()` 在回调里可安全调用。
- `stop()` 返回后任务已全部退出、不会再有任何回调，此时才可以释放 `user_data` 指向的对象。
- 示例应用遵循同样的约束：WiFi 事件里只调用非阻塞的 `set_underlay_ready()`，而把
  `init_from_toml()` + `start()` 放在独立的 8 KiB 任务里执行（见 [`main/main.cpp`](../../main/main.cpp)）。

## Kconfig 可调项

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `EASYTIER_ESP_LOG_LEVEL` | Info | 组件日志编译等级：Closed、Error、Warning、Info、Debug；低于所选等级的调用会在编译期裁剪，Closed 裁剪全部日志 |
| `EASYTIER_ESP_MAX_CONNECTED_PEERS` | 6 | **同时连接**的入口节点数上限（1..8）。配置里的入口条目数不受此限制，多出的作为轮换备用；每槽约 12 KiB 收发缓冲 |
| `EASYTIER_ESP_TOML_MAX_BYTES` | 16384 | TOML 文本/文件上限 |
| `EASYTIER_ESP_TOML_HEAP_BUDGET` | 262144 | 单次 TOML 解析允许累计申请的堆字节上限 |
| `EASYTIER_ESP_RPC_MAX_TRANSACTIONS` | 4 | 并发 RPC 事务上限 |
| `EASYTIER_ESP_RPC_REASSEMBLY_SLOTS` | 2 | RPC 分片重组槽数量（每槽约 16 KiB） |

宿主测试没有 sdkconfig，使用 `support/config_limits.hpp` 里的同一批默认值；`tests/host/test_config_limits.cpp`
把"宿主默认值 == Kconfig 默认值"固定下来，避免两端行为漂移。

## 内存预算（实测，ESP32-S3）

| 项目 | 大小 | 说明 |
| --- | --- | --- |
| `Context`（堆） | 93,768 B | RPC 重组 33,184（2 槽 × 16 KiB）+ `SyncEngine` 约 33,000（拓扑 22,928 + 邻接表 8,968 + 快照与编码缓冲）+ RPC 收发缓冲约 14,000 + 其它 |
| 入口地址列表（堆） | 每条约 260 B | 按配置实际条数分配，不占用 `Context`；条目数量无上限 |
| 帧队列 | 约 24 KiB | 控制面 / 数据面 / 发送队列各 4 × 2008 B |
| `TcpConn`（堆） | 约 12 KiB × 连接数 | 每槽一份收发缓冲，槽数见 `EASYTIER_ESP_MAX_CONNECTED_PEERS`（默认 6） |
| 任务栈 | 24 KiB | I/O 6 KiB + 控制面 10 KiB + 数据面 8 KiB |
| TOML 解析 | ≤ 256 KiB（瞬时） | 受 `EASYTIER_ESP_TOML_HEAP_BUDGET` 限制 |

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
硬件运行与真实对端互操作由用户手动完成，步骤见硬件联调指南。

## 任务栈预算校验

```bash
idf.py -DEASYTIER_STACK_USAGE=ON build
python3 tools/check_stack_usage.py
```

用 `-fstack-usage` + ELF 调用图计算三个任务入口的最坏调用链，与声明的栈大小比较
（当前：I/O 2400/6144、控制面 5632/10240、数据面 4400/8192 字节）。大缓冲一律放在
`Context`/`SyncEngine` 成员或堆上，任务栈上只保留小对象——这一点很容易在重构中被破坏，
所以该检查应当与宿主测试一起跑。

> 验收项 ↔ 命令 ↔ 期望输出的完整对照见 [docs/验收证据清单.md](../../docs/验收证据清单.md)。

## 硬件联调

完整步骤见 **[docs/硬件联调指南.md](../../docs/硬件联调指南.md)**（静态检查 → 设备自检 →
Linux 入口节点 → 期望日志逐条对照 → 直连/中继验证 → 排错表）。

示例在启动时会打印**实际生效的配置**（`Instance::get_config_info()`），并每 10 秒打印一行
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
