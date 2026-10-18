## EasyTier-ESP v0.0.2 - 2026-10-18

### 修复

- 修复运行约 90 秒后路由全部消失、状态反复回到 syncing、其他设备无法访问本机虚拟 IP 的问题。
- 修复重连后路由偶发不同步的问题；长时间运行更稳定。

### 示例

- `main` 示例简化：组网流程移到 `app_main`（main 任务），不再创建额外的启动任务；应用层只额外保留 HTTP 任务。
- 示例的 WiFi 凭据改用 Kconfig（`EXAMPLE_WIFI_SSID` / `EXAMPLE_WIFI_PASSWORD`）。
- 示例的 main 任务栈默认提高到 8 KiB，以容纳 TOML 解析与实例构造。

### 其他

- 新增 LGPL-3.0 LICENSE。
- 使用说明统一放到根目录 README。
- 一些性能优化与内部整理。

---

## EasyTier-ESP v0.0.1 - 2026-10-17

### 初版

- EasyTier-ESP 是一个使用 C++20 编写的 ESP-IDF 组件，用于把 ESP32 接入既有 EasyTier
  虚拟网络；适配 ESP-IDF 6.1.x，当前在 ESP32-S3 上验证。
- 公开 API 只有 `et::Inst`：用 TOML 文本构造 → `start()` → `connect()` / `disconnect()`
  → `stop()` / 析构；支持多个实例并存。
- 实现 TCP 入口、握手与保活、RPC、路由同步、AES-128-GCM 加密、lwIP 虚拟网卡与数据转发。
- 可通过 ESP-IDF 组件注册表 `myqfeng/easytier-esp` 导入；仓库内提供 `main/` 示例和
  Kconfig 配置项。
