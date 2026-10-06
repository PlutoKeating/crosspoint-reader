# StockStick 固件

StockStick 是投资者专用的墨水屏陪伴设备。本仓库是运行在 Xteink X3（ESP32-C3）上的设备固件，
与微信小程序和 StockStick 云服务（相邻仓库 `Project.StockStick`）一起组成完整产品。

固件源自开源项目 [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
（MIT 许可），2.0.0 起移除了电子书阅读器及其生态功能，只保留设备平台与 StockStick 功能。

## 功能

- **StockStick 内容**：Studio 单屏画面与离线播放计划、兼容版 Release 文案轮换、行情提醒、
  左右侧键“没啥用 / 有用”反馈、X3 全局按键锁。详见 [docs/project-stick.md](docs/project-stick.md)、
  [docs/studio-protocol.md](docs/studio-protocol.md)。
- **同步**：Wi-Fi 自动重连与 TLS 连接复用；BLE 协议 2 离线加密传输；设备绑定、归属变更与清理。
- **固件升级**：单一来源的版本号与构建号、镜像内置描述符、断点续传 OTA、试运行与自动回滚、
  设备端检查更新、SD 卡与恢复模式刷机。详见 [docs/firmware-ota.md](docs/firmware-ota.md)。
- **界面语言**：只有内置简体中文（2.7.5 起移除了 SD 卡语言包）。详见 [docs/i18n.md](docs/i18n.md)。
- **硬件**：X3 为产品目标，保留 X3/X4 运行时识别。

生产 API：`https://stockstick.plutokeating.beer`（`/api/v2/device`，设备 Bearer 凭证，固定根证书）。

## 构建

需要 PlatformIO Core 6.1.19（pioarduino 平台，见 `platformio.ini`）。

```bash
git submodule update --init --recursive
pio run -e gh_release          # 发布构建；构建后自动校验镜像不超过 OTA 分区
pio run -e default             # 开发构建（带分支与提交号的版本串）
pio run -e default -t upload   # USB 刷机
```

首次构建（或 `custom_sdkconfig` 变化后）会先用 ESP-IDF 重新编译 Arduino 框架库，耗时较长。
LTO 由 `build_unflags = -fno-lto` 交给 pioarduino 只对应用代码开启，不要在 `build_flags` 中加 `-flto`，
否则干净环境中的框架库编译会失败。

版本号、构建号与最低安装构建号只在 `platformio.ini` 的 `[crosspoint]` 中维护。
打包 OTA 发布见 [docs/firmware-ota.md](docs/firmware-ota.md#3-发布流程)：

```bash
python3 scripts/firmware_release.py --build --notes RELEASE_NOTES.md --url-base https://<存储>/
```

## 模拟器与测试

桌面模拟器是本源码的原生（SDL2）构建，使用相邻的 `crosspoint-simulator` 仓库和本地
`platformio.local.ini`（已 gitignore；示例见模拟器仓库的 `sample-platformio-*.ini`）。

```bash
pio run -e simulator -t run_simulator
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test
```

主机单元测试覆盖 StockStick 同步与调度、Studio 播放计划、固件描述符/安装策略/试运行策略、
内置文案与字形覆盖。模拟器不执行真实刷写、BLE 或功耗测试。

## 文档

| 文档 | 内容 |
|---|---|
| [USER_GUIDE.md](USER_GUIDE.md) | 设备使用说明 |
| [SCOPE.md](SCOPE.md) | 固件功能范围 |
| [docs/firmware-ota.md](docs/firmware-ota.md) | 版本管理、发布、OTA、试运行与回滚 |
| [docs/project-stick.md](docs/project-stick.md) | StockStick 同步、存储与按键行为 |
| [docs/studio-protocol.md](docs/studio-protocol.md) | Studio 画面、播放计划与 BLE 协议 |
| [docs/i18n.md](docs/i18n.md) | 界面文案 |
| [docs/activity-manager.md](docs/activity-manager.md) | 页面（Activity）框架 |
| [docs/troubleshooting.md](docs/troubleshooting.md) | 故障排查 |
| [docs/contributing/](docs/contributing/) | 继承自上游的开发者指南（架构、调试） |

## 许可

MIT，见 [LICENSE](LICENSE)。上游 CrossPoint Reader 与 FreeInk SDK 的版权声明保留在各自文件中。
