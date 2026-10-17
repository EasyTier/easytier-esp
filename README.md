# EasyTier-ESP

EasyTier-ESP 是一个使用 C++ 20 编写的 EasyTier 异地组网工具的轻量级协议实现。便于把 ESP32 接入既有 EasyTier 虚拟网络。组件目前实现 EasyTier客户端侧的基本协议与数据面。

- 组件注册表 ID：`myqfeng/easytier-esp`（ESP-IDF Component Registry）
- 适配 ESP-IDF：>=6.1
- 当前已验证目标：ESP32-S3-N16R8

## 项目特点

- **协议兼容**：TCP 入口连接、PeerManagerHeader 分帧、非安全模式握手（网络名 +
  SipHash 摘要 + peer id 冲突校验）、Ping/Pong 保活、RPC 分片/乱序重组/事务匹配、
  OSPF 式路由同步（`RoutePeerInfo` / 邻接表 / 邻接位图）。
- **加解密**：支持 EasyTier 默认的 AES-128-GCM（PSA Crypto）加密xieyi。
- **数据面**：lwIP raw L3 虚拟网卡、中继转发。
- **有界资源**：三个任务（`io` / `proto` / `data`）、固定容量队列与缓冲；
  队列深度、任务栈/优先级、并发连接槽数都可在 Kconfig 调整。
- **公开 API 只有一个类 `et::Inst`**（RAII）：TOML 构造 → `start()` →
  `connect()` / `disconnect()` → `stop()` / 析构。

## 代码层级结构

```
easytier-esp/
├── components/easytier-esp/          # 组件本体（可被 IDF 组件管理器安装）
│   ├── include/easytier-esp.hpp      # 唯一公开头：Inst / Settings / Status / Event
│   ├── src/
│   │   ├── inst.cpp                  # et::Inst 实现（TOML → Settings → Context）
│   │   ├── context.{h,cpp}           # RAII 运行上下文：队列/任务/连接槽/AEAD/网卡
│   │   ├── config/                   # TOML 解析与设置校验（tomlc17）
│   │   ├── crypto/                   # SipHash-1-3 与 AES-128-GCM
│   │   ├── protocol/                 # 线格式、protobuf 编解码
│   │   ├── transport/                # 非阻塞 TCP（切帧/部分写/背压）
│   │   ├── peer/                     # 握手与保活
│   │   ├── rpc/                      # 服务注册表、事务表、分片重组
│   │   ├── route/                    # 拓扑、最短路、DHCP、路由同步
│   │   ├── netif/                    # lwIP raw 虚拟网卡
│   │   ├── data/                     # 数据面纯逻辑（路由决策/组帧）
│   │   ├── tasks/                    # io / proto / data 三个任务
│   │   └── support/                  # 日志、时间、随机数、字节序、编译期上限
│   ├── third_party/tomlc17/          # 内置 TOML 解析器（MIT）
│   ├── Kconfig                       # 队列深度、任务栈/优先级等编译期选项
│   └── CMakeLists.txt
├── main/                             # 示例应用（NVS/WiFi/加载 TOML/连接控制）
├── tools/                            # 栈预算分析、第三方源码导入脚本
└── CMakeLists.txt                    # ESP-IDF 工程入口
```

## 如何导入

### 通过 ESP-IDF 组件注册表

在应用的 `main/idf_component.yml` 中加入依赖：

```yaml
dependencies:
  myqfeng/easytier-esp: "^0.0.1"
  idf: ">=6.1"
```

或者用命令：

```bash
idf.py add-dependency "myqfeng/easytier-esp^0.0.1"
idf.py build
```

构建时组件会被下载到工程的 `managed_components/` 下。

### 本地源码导入

- 把 `components/easytier-esp` 拷贝或软链到应用的 `components/` 目录；或
- 在工程顶层 `CMakeLists.txt` 把本仓库的 `components` 目录加入 `EXTRA_COMPONENT_DIRS`。

然后在应用组件的 `CMakeLists.txt` 里声明依赖：

```cmake
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES easytier-esp
)
```

组件在 `CMakeLists.txt` 中以 `PUBLIC` 方式要求 C++20，并关闭本组件源码的异常与 RTTI；
公开头本身不依赖 FreeRTOS/lwIP 头。

### 资源建议

- 单个实例的 `Context` 约 92 KiB，建议开启 PSRAM（大块分配会落到 PSRAM）。
- 任务栈、队列深度、并发连接数等都可通过 `idf.py menuconfig` 的
  `EasyTier-ESP` 菜单调整；默认值按 ESP32-S3 上的实测栈余量选取。

## 使用说明

### 最小调用示例

```cpp
#include <optional>

#include "easytier-esp.hpp"

static std::optional<et::Inst> g_inst;

static void on_event(const et::Event& event, void*) noexcept {
    // 在控制面任务里同步执行，必须快速返回
    if (event.type == et::EventType::NetworkReady) {
        // 组网可用；event.status.virtual_ipv4 是本机虚拟地址
    }
}

void start_easytier(std::string_view toml_text) {
    et::RuntimeOptions options{};
    options.on_event = &on_event;

    // TOML 解析是递归下降，请在大栈任务里构造（示例用 8 KiB 任务）
    g_inst.emplace(toml_text, options);
    if (!g_inst->valid()) {
        ESP_LOGE(TAG, "config rejected: 0x%x", g_inst->last_error());
        g_inst.reset();
        return;
    }

    ESP_ERROR_CHECK(g_inst->start());    // 创建队列/任务/网卡（异步）
    ESP_ERROR_CHECK(g_inst->connect());  // WiFi/以太网取得 IP 后调用
}
```

### TOML 示例

```toml
instance_name = "esp32-demo"
hostname = "esp32-s3"
ipv4 = "10.144.144.10/24"   # 或 dhcp = true

[network_identity]
network_name = "esp-demo"
network_secret = "replace-with-your-network-secret"

[[peer]]
uri = "tcp://192.168.1.20:11010"

[flags]
mtu = 1380
enable_encryption = true
```

支持的字段必须写对（类型/取值非法会让构造失败并打 Error 日志）；不支持的字段
（如 `listeners`、`routes`、其它 flags）不解析也不报错；未给出的字段使用缺省值。
`ipv4` 是唯一例外：字符串类型正确但内容无法解析时静默忽略。

### 接口说明

| 接口 | 说明 |
| --- | --- |
| `Inst(toml, options)` | 构造即完成解析、校验、上下文分配与密钥派生；失败用 `valid()`/`last_error()` 查询 |
| `start()` | 创建运行资源与三个任务；返回成功不代表组网可用 |
| `connect()` / `disconnect()` | 应用侧的连接开关：`connect()` 后组件才连接入口；`disconnect()` 立即关闭连接并撤销路由。两者非阻塞、幂等，可在 WiFi/IP 事件回调里直接调用；WiFi 重连后必须再次 `connect()` |
| `get_status(Status&)` | 读取状态快照（state、虚拟 IP、直连/可达数、丢弃计数、最近错误） |
| `get_config_info()` | 返回生效设置 `Settings` 副本（网络密钥恒为空） |
| `stop(timeout)` / 析构 | 停止任务并释放资源；禁止在事件回调中调用 |

## 示例项目（`main/`）

`main/` 是一个可以直接 `idf.py build` 的示例工程：

- `main.cpp`：NVS、`esp_netif`、WiFi STA 连接与自动重连；
- `easytier_app.{hpp,cpp}`：把 `main/easytier.toml` 通过 `EMBED_TXTFILES` 内嵌
  （文件不存在时只连 WiFi 并打印提示），在独立的 8 KiB 任务里构造 `et::Inst`、
  `start()`、`connect()`；WiFi 事件回调里只调用 `connect()` / `disconnect()`；
  启动时打印生效配置，之后每 10 秒打印一行状态；
- `http_control.{hpp,cpp}`：演示用 HTTP 服务（GPIO38 电平翻转），与组网无关。

示例依赖 `easytier-esp` 组件与 `esp_wifi`/`esp_event`/`esp_netif`/`esp_timer`/
`nvs_flash` 等；仓库不包含 `main/easytier.toml`，新克隆的仓库需要自行创建配置后再构建。
