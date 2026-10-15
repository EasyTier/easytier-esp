# 仓库代理指南

## 用户约定

- **不要自行烧录、擦除 Flash、启动串口监控或访问开发板串口。** 可以构建固件、运行宿主测试和离线分析用户日志。烧录和监控由用户通过 ESP-IDF 命令手动执行；代理不可代为操作。
- 用户要求快速推进：**不要求 TDD，不强制测试先行或额外设计审批**。理解问题后直接实施，完成后运行与改动相关的验证。不要仅提供计划就结束。
- 保留用户已有修改，特别是 `main/easytier.toml`、WiFi 配置和工作树中的未提交文件。不要恢复演示配置覆盖用户配置，不提交或打印网络密钥、WiFi 密码。`main/easytier.toml` 含真实入口地址与网络密钥，已被 `.gitignore` 忽略且不在任何提交历史里；**仓库不提供配置模板**（TOML 格式见组件 README 的「配置」一节），`main/CMakeLists.txt` 只在文件存在时才 `EMBED_TXTFILES` 它，缺失时示例只连 WiFi 并打印提示。
- 使用中文解释，公共接口与关键实现写清晰的中文 Doxygen 注释。报告时区分宿主测试通过、设备工程构建通过、用户上板运行结果。

## 项目定位与目录

本项目把 ESP32 接入既有 EasyTier 网络，组件只适配 **ESP-IDF 6.1.x**，当前目标为 ESP32-S3。C++20，命名空间 `et`。只支持**全局单实例**：配置类是 `Config`（TOML 文本构造 / `init`，不接受文件路径），运行期接口是全局命名空间 `et::Inst` 下的自由函数，**不要重新引入实例类或 `Node`/C 包装层**。配置**只走 TOML**：不要加编程式 setter，`Config::init` 是全量替换（先复位为缺省值再解析），因此未显式给出的 `instance_id`/`hostname`/`instance_name` 每次都会重新生成。组件禁用异常与 RTTI。

- `components/easytier-esp/include/easytier-esp.h`：公开 C++ API、配置、状态、事件和诊断。
- `components/easytier-esp/src/`：组件实现；`config/`、`crypto/`、`protocol/`、`transport/`、`peer/`、`rpc/`、`route/`、`netif/`、`data/` 为主要模块。
- `inst.cpp` / `context.hpp`：运行资源、队列、生命周期、共享原子状态（`et::Inst` 的全局实现）。
- `io_task.cpp`：socket 的唯一所有者；并发连接槽数量由 Kconfig `EASYTIER_ESP_MAX_CONNECTED_PEERS`（默认 6）决定，**入口条目数不设上限**，其余入口轮换备用。不能把一个入口重复连接到两个槽，重试不得阻塞已有连接的收发。
- `proto_task.cpp`：握手、保活、RPC、路由同步和路由快照发布；`data_task.cpp`：虚拟网卡数据、加解密与中继转发。
- `main/`：应用示例，仅负责 NVS、事件循环、WiFi、加载 TOML 和 underlay 通知，不放协议实现。
- 当前工作区没有 `tests/` 或 `test_apps/`；旧文档中的测试记录不能作为当前可执行入口，不要自行恢复用户移除的目录。
- `docs/`：只保留 `遗留问题清单.md`（一次全项目精简 Review 查出的**未修**问题与待办，含位置/证据/改法/风险）；组件 README 为使用入口。原「硬件联调指南」「验收证据清单」已按用户要求删除，可从提交 `4ecd063` 取回。

## 上游与兼容行为

本机上游源码在 `~/Desktop/EasyTier/`。变更协议、默认值或配置语义时读取当前源码，不凭旧总结推断。当前上游配置入口主要在 `easytier-core/src/config/toml.rs`，运行期 Host 归一化在 `easytier-core/src/instance/config.rs`；旧路径可能仅作 re-export。

- TOML：采用支持且有效的非 peer 字段；未知、无关或无法采用的非 peer 字段忽略并报告，不因缺少入口/地址拒绝解析。真正语法错误、读取失败、解析资源耗尽分别报告。
- **peer 只能多写附加字段，不能写错入口**：协议不支持、IPv6、无效 URI/主机/端口、错误类型、缺少 `uri` 必须报错，即使同时有其他有效入口。**TOML 是唯一配置入口**（没有编程式 setter），入口 URI 校验只有这一份。入口条目数**没有上限**（按实际条数分配），只有可同时连接的数量受 Kconfig 限制。
- 缺省网络名为 `default`，网络密钥为空，MTU 为 1380，静态 IPv4 与 peer 列表缺省为空，DHCP 缺省关闭。DHCP 自举回落子网为 `10.126.126.0/24`，可以优先采用网络中宣告的网段。缺省 `instance_name` **取 hostname**（不再固定为 `default`）。
- 缺省实例 UUID 在创建/导入 `Config` 时生成并缓存 UUID v4；显式 UUID（包括全零 UUID）不得被覆盖。主机名遵循“有效显式值优先，否则 Host fallback”，ESP fallback 按用户要求为 **`Espressif`**。
- `EMBED_TXTFILES` 的 `end-start` 包含末尾 NUL，不能把终止符当作 TOML 内容；内部非法 NUL 仍要拒绝。

## 构建与验证

从仓库根目录执行。当前没有宿主/设备测试源目录，只执行实际存在的构建与检查；
不要因旧文档提到测试目录而重新创建它们。

ESP-IDF 构建（不烧录）：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
"$IDF_PYTHON_ENV_PATH/bin/python3" "$IDF_PATH/tools/idf.py" build
```

激活脚本不能在 `set -u` 下直接 source；自动化子 shell 的 `$0` 应为 `bash`。检查实际 IDF 路径及命令退出码，不把激活脚本的返回值等同于环境是否可用。

直接使用 IDF 命令验证，检查真实退出码；不要以日志字符串匹配代替命令成功，也不要在构建失败后运行陈旧二进制。`tools/` 只保留栈分析和第三方源码导入工具。栈静态分析是近似证据，不能替代真机栈高水位与稳定性验证。

新增组件源码时更新组件 `CMakeLists.txt` 的显式源文件列表。不要在指南中写死易变化的用例数量或固件大小而不同步验证。

## 已知平台陷阱

- **IDF 6.1 的 raw lwIP netif 不可把 `VirtualNetif*` 存入 `netif->state`**：默认配置下 esp-netif 全局回调会将它解释为 `esp_netif_t*`。当前网卡保持 `state=nullptr`，通过标准布局首成员恢复所属对象；保留相关布局断言。
- FreeRTOS 100 Hz 时 `pdMS_TO_TICKS(5)` 为零。任务循环必须真正阻塞至少一 tick，并限制每轮队列处理量，避免高优先级任务饿死 IDLE；不要关闭 SDK 看门狗规避问题。
- 网卡创建或改址成功后，先更新公开 IPv4/前缀状态，再发送 `NetworkReady`。路由存在不等于虚拟地址已可用。
- 控制面与数据面持有独立加密实例；中继 Data 保留最终目的及密文，仅更新转发计数和下一跳。
- TCPIP output 回调只做 pbuf 引用和零等待入队；注销网卡先于销毁队列。大缓冲放在上下文/堆中，不挤占任务栈。
- 生命周期调用串行，初始化也不要跨任务并发；只读查询要求实例仍存活且不与销毁并发。事件回调在控制面同步执行，禁止在其中 stop/deinit/析构或长时间阻塞。
- 用户日志若提示 flashed/built ELF checksum mismatch，先区分运行固件版本；错配 ELF 的自动回溯函数名和行号不可信。只读分析，不自行重新烧录。
