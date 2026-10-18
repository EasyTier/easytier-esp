# EasyTier-ESP

EasyTier-ESP 是一个使用 C++ 20 编写的 EasyTier 异地组网工具的轻量级协议实现。便于把 ESP32 接入既有 EasyTier 虚拟网络。组件目前实现 EasyTier客户端侧的基本协议与数据面。

- 组件注册表 ID：`myqfeng/easytier-esp`（ESP-IDF Component Registry）
- 适配 ESP-IDF：>=6.1
- 当前已验证目标：ESP32-S3-N16R8

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

> 更多内容请前往 [cnb.cool/myqfeng/qteasytier/easytier-esp](https://cnb.cool/myqfeng/qteasytier/easytier-esp) 查看