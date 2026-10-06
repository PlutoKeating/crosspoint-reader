# StockStick 固件版本管理与 OTA 升级

本文是 StockStick 固件版本、发布与升级的权威说明，覆盖从构建到设备确认的完整闭环。
服务端接口位于 `Project.StockStick` 的 `/api/v2/device/*`（见其 `docs/API.md`、
`docs/product/STUDIO-API.md`）。

## 1. 设计原则

- **分区表不可 OTA 变更。** `partitions.csv` 的两个 OTA 槽各 `0x640000`（6,553,600 字节），
  出厂后只能通过 USB 改写，因此它是永久的体积上限。`scripts/check_studio_image_size.py`
  在每次构建时校验最终镜像，发布脚本再次校验。
- **一次只信任经过验证的镜像。** 安装前依次校验 SHA-256、ESP 镜像结构（段表、异或校验、SHA 尾）、
  StockStick 描述符（产品、芯片、板型、最低构建号、目录版本）。
- **新固件先试运行。** 新镜像必须证明自己能运行并连上 StockStick API，才被确认；
  否则自动回到旧分区，并把回滚作为事件上报给服务端。
- **设备自己完成升级。** 升级由手机经蓝牙触发（用户就在设备旁），或在设备设置里手动检查；设备自己经 Wi‑Fi
  下载目录里的镜像并安装。服务端没有命令队列，设备以新版本注册即为升级完成（2.4.0 起）。

## 2. 版本标识

唯一来源是 `platformio.ini`：

```ini
[crosspoint]
version = 2.4.3            ; semver，展示给用户并与服务端目录比对（≤ 31 字节）
build = 20403              ; 单调递增的构建号，OTA 比较它而不是字符串
min_install_build = 20000  ; 本固件愿意安装的最低构建号
```

- 正式版 `build = major*10000 + minor*100 + patch`；预发布版（如 `2.1.0-rc.1`）的构建号仍须严格递增。
- `min_install_build` 不得低于第一个带试运行/回滚能力的构建（20000），以免设备降级到无法再 OTA 的固件。
- `gh_release_rc` 环境生成 `<version>-rc.<短哈希>`，`default` 开发环境生成 `<version>-dev-<分支>-<短哈希>`。

### 镜像描述符

每个镜像在 `.rodata_custom_desc` 段嵌入 96 字节描述符（`lib/ProjectStick/StickFirmware.h`，
实例在 `src/platform/StickFirmwareDescriptor.cpp`）。ESP-IDF 链接脚本把它紧跟在 `esp_app_desc_t`
之后，因此在 `.bin` 中的位置固定：文件偏移 `24 + 8 + 256 = 288`。

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `u32 magic` | `"SSFW"`（0x57465353） |
| 4 | `u16 descriptorVersion` / `u16 descriptorSize` | 1 / 96 |
| 8 | `u32 build` | 构建号 |
| 12 | `u32 boards` | bit0 = X3，bit1 = X4 |
| 16 | `char product[16]` | `"stockstick"` |
| 32 | `char version[32]` | 版本字符串 |
| 64 | `char commit[16]` | 源码提交短哈希 |
| 80 | `u8 reserved[16]` | 保留 |

符号 `stick_firmware_descriptor` 以 C 链接导出，并作为链接器根（`-u`），防止 LTO/`--gc-sections` 丢弃。
上游 CrossPoint 或其它 ESP 镜像没有描述符，会被识别为“不是 StockStick 固件”。

## 3. 发布流程

```bash
# 1. 修改 platformio.ini 的 version/build，提交
# 2. 构建并打包（工作区必须干净）
python3 scripts/firmware_release.py --build --notes RELEASE_NOTES.md \
    --url-base https://<固件存储>/stockstick/
```

输出 `dist/firmware/<version>/`：

| 文件 | 用途 |
|---|---|
| `stockstick-<version>.bin` | OTA / SD 卡 / 恢复模式镜像 |
| `stockstick-<version>.elf` | 崩溃栈符号，内部保留，不发布 |
| `manifest.json` | 版本、构建号、板型、芯片、字节数、SHA-256、提交、说明 |
| `catalogue.json` | 管理后台固件目录的登记内容 |

脚本会拒绝：版本不符合目录格式、构建号未递增、描述符与配置或 HEAD 不一致、镜像超出分区。
推送 `v<version>` 或 `<version>` 标签会触发 `.github/workflows/release.yml`，它校验标签与版本一致后执行同一脚本并上传产物。

3. 把 `.bin` 托管到网站 `public/firmware/<version>/`（设备只接受 `<API 基址>/firmware/` 下的 URL；
   静态资源支持 `Range`，断点续传依赖它）。
4. 在管理后台 `/console/studio` 登记 `catalogue.json` 中的版本、URL、SHA-256、字节数与说明。
5. 需要下线时在后台“撤回”：撤回的版本不再出现在检查更新和小程序中；已开始的下载若镜像被删除（404/410）即失败。

预发布版本（带 `-` 后缀）不会出现在设备端“检查更新”的稳定通道，但所有者仍可在小程序中手动选择。

## 4. 升级入口

| 入口 | 发起方 | 路径 |
|---|---|---|
| 小程序设备详情（手机在设备旁） | 所有者 | 2.7.4 起：小程序下载镜像，经蓝牙协议 4 的 `fw4` 操作分块传给设备（证明 `fw4|device_id|N|epoch|version|sha256|size|time`，仅已绑定模式），设备随收随写 SD 卡，收齐后走 SD 卡安装。2.7.3 及以前：协议 3 操作 `ota`，设备自己经 Wi‑Fi 下载（2.7.4 起不再接受） |
| 设备「设置 → 系统 → 固件更新」 | 设备（无需绑定，联网即可，2.7.1 起） | `GET /api/v1/public/firmware/latest?channel=stable`（不带凭据）→ 设备本地比较版本 → 确认 → 直接下载 `url` 到 SD 卡、校验、刷写并重启 |
| 设备「SD 卡固件更新」 | 用户 | 选择 `/` 或 `/firmware` 下的 `.bin`；只接受适用于本机的 StockStick 镜像，并启用试运行 |
| 恢复模式（按住左侧键开机） | 用户 | 同上但不做描述符限制，用于救砖；只有 StockStick 镜像启用试运行 |
| USB / 网页刷机 | 开发者 | 不经过试运行，视为可信镜像 |

2.7.2 起 Wi‑Fi 按需开启：固件更新页打开期间或排队了下载任务时，设备自行连接已保存的网络，任务最多等待 20 秒。有已保存网络时，检查更新不再弹出 Wi‑Fi 选择页。2.7.4 起，手机连着设备时 Wi‑Fi 一直关闭（后台任务等手机断开），蓝牙固件传输期间同样关闭。

2.7.3 起，设备上的检查更新和安装开始时，如果手机正通过蓝牙连着设备，设备会先在 STATUS 里给出 `net_busy`，约 0.4 秒后主动断开手机，并释放蓝牙协议栈。任务结束前设备不再广播，手机这段时间无法重连；升级时一直持续到重启。以前手机连着时蓝牙无法释放，内存不够建立 HTTPS 连接，检查会报「设备内存不足」，下一次又被设备自己的退避误报成「云端繁忙」。现在只有服务器真的返回 429 时才显示「云端繁忙」（附剩余秒数），手动检查不受设备自身退避的限制；内存不足的提示会给出当时的可用 KB 数。

## 5. 设备端执行流程

设备注册与蓝牙 STATE 上报 `firmware_version`、`firmware_build` 与能力 `ble: 3`、`ota: 4`（2.7.4 起；3 表示设备自行下载的旧 `ota` 操作）、`panel: xteink_x3|xteink_x4`。

三条路径最后都调用同一个 SD 卡安装例程 `firmware_install::installFromSd()`
（`src/project_stick/FirmwareInstall.*`）：试运行保护 → 读取并校验镜像描述 → 刷写另一 OTA 槽并读回比对 →
仅 StockStick 镜像启用试运行 → 失败时撤销试运行记录。它不负责重启，由调用方决定。

蓝牙 `fw4`（2.7.4 起，详见 studio-protocol.md "Firmware over BLE"）：

```
fw4 操作（仅已绑定模式）
  0. 拒绝：版本不比当前新 / 大小不在 100,000~6,553,600 / sha 格式错 → invalid_target；
     正在传输 → device_busy；试运行中 → trial_active；电量 < 30% 且未充电 → low_battery；
     SD 空间不足 → insufficient_storage
  1. 停 Wi‑Fi（最多等 10 秒），分配接收队列（不足 → insufficient_memory）
  2. 打开 /.crosspoint/studio/firmware.tmp：同一 sha 的半截文件按 firmware.meta
     "<sha> <字节> <已写原始字节> <流偏移>" 从块边界续传（重算前缀哈希），否则重建
  3. 每条记录（32 KB 原始块，raw deflate）解压后立即写 SD 并更新 SHA-256 与 firmware.meta
  4. 收齐后比对 SHA-256（不一致删除半截文件 → checksum_mismatch）
  5. PROGRESS state 3 通知手机（此时蓝牙仍连着）→ STATUS ota=verifying → 释放蓝牙 →
     installing → installFromSd → restarting → 重启；失败则恢复蓝牙并报原因
```

设置里确认安装：

```
设置里确认安装
  └─ 后台任务 installFirmware(version, url, sha256, bytes)
      0. 新固件仍在试运行时拒绝安装（trial_active）
      1. 目标校验：URL 必须在 <API 基址>/firmware/ 下、SHA-256 格式、大小在 100 KB~分区上限；
         电量 ≥ 30%，或正在外接电源充电（2.7.1 起）；Studio 正在接收内容时最多等待 30 秒
         （仍未结束则 device_busy）。不需要绑定，也不需要设备令牌（2.7.1 起）
      2. 断开已连接的手机（2.7.3 起，STATUS 先给出 net_busy），释放 BLE（TLS 与刷写需要 NimBLE
         占用的内存），持有电源锁，屏幕切换为升级进度页
      3. 下载到 /.crosspoint/studio/firmware.tmp
         · firmware.meta 记录 "<sha> <字节>"；同一镜像的半截文件以 Range 续传（206）
         · 每写约 256 KB 刷新一次 SD 文件，掉电后续传偏移以文件实际大小为准
         · 服务器忽略 Range（返回 200）时从头下载；403/404/410/416 直接失败
      4. 大小 + SHA-256 → 描述符与安装策略
      5. installFromSd：写入另一 OTA 槽（同一时间只允许一个安装），写完后读回整个分区计算 SHA-256 比对
      6. 切换启动槽之前写入试运行记录 → 切换 otadata → 重启（刷写失败时保留半截下载以便续传）
```

任一步失败都会记入 `firmware_update`（原因如 `download_failed`、`checksum_mismatch`、`VERSION_MISMATCH`、
`BELOW_MINIMUM_BUILD`、`low_battery`、`trial_active`、`device_busy`），屏幕与蓝牙 STATUS `ota` 都能看到，BLE 恢复广播；
半截下载保留供同一镜像再次安装时续传，校验失败的文件会被删除。2.7.1 起固件更新页按原因显示
具体文案（下载中断可续传、校验失败、电量不足、试运行中、正在接收内容、镜像不适用），
安装失败后同一按键直接重试同一镜像。

设备端检查更新（2.7.1 起）：`ProjectStickService::checkFirmware()` 不带凭据请求公开目录接口，
用 `stick_fw::compareVersions()` 在本机比较版本（数字段按数值比较；数字相同时不带后缀的正式版
高于带后缀的构建），只在目录版本严格更新时提示。未绑定的设备同样可用；这条路径上的任何响应
都不会改动绑定状态。蓝牙固件传输在任意页面都可进行（`studio_ble::pump()` 在后台任务上运行），
安装开始后屏幕自动切到进度页（见 project-stick.md "Page-independent host"）。

## 6. 试运行与自动回滚

实现：`src/network/OtaTrial.*`，策略（可单元测试）：`lib/ProjectStick/StickFirmware.*`。
状态保存在 NVS 命名空间 `stick_ota`，掉电、深睡、重启后都保留。

| 时机 | 行为 |
|---|---|
| 写完新镜像、切换启动槽之前 | 记录旧槽、目标槽、目标版本，`armed=true` |
| 每次启动（`setup()` 最早阶段） | 运行在旧槽 → 记为回滚（`bootloader_rollback`）；上一次是 panic/看门狗复位 → 失败次数 +1；达到 3 次 → 切回旧槽并重启（`repeated_crash`）。掉电、欠压复位和深睡唤醒不计入。计数写入 NVS 后即向 bootloader 标记镜像有效，之后由应用层计数负责 |
| 运行中（主循环） | 任意一次 StockStick API 返回 HTTP 状态 → 确认；手机经蓝牙完成一次签名的 `sync` 操作（2.6.0）→ 确认（`phone_ok`）；同一次 Wi-Fi 连接期间，API 请求周期在传输层失败（每 30 秒最多计 1 次，Wi-Fi 断开即清零）累计 ≥ 5 次且首末相隔 ≥ 5 分钟 → 回滚（`cloud_unreachable`）；从未联网且运行满 2 分钟 → 确认；超过 60 分钟仍无结论 → 确认。刷写进行中不做判定 |
| 正常进入深度睡眠 | 视为健康，确认 |
| 下一次同步 | 回滚结果作为事件 `firmware_rolled_back`（`payload.detail` = "<版本> <原因>"）上报：优先由手机经蓝牙 STATE 的 `ota_outcome` 中继到 `/api/v1/miniapp/studio/devices/state`，否则随下一次注册心跳 |

`verifyRollbackLater()` 返回 true，Arduino 不再在 `initArduino()` 中自动确认镜像，而是由
`ota_trial::onBoot()` 在写入计数后调用 `esp_ota_mark_app_valid_cancel_rollback()`。
本仓库构建的 bootloader 启用了回滚（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`），它会把
PENDING_VERIFY 状态下的**任何**复位都当作失败；若把确认推迟到健康判定，一次掉电就会误回滚。
因此 bootloader 只负责 `onBoot()` 之前就崩溃的镜像，之后交给上表的应用层策略。出厂 bootloader
若不支持回滚，则完全依靠应用层计数。试运行期间设置中的 SD 卡更新和在线安装都会拒绝，
避免覆盖回滚目标分区；恢复模式不受此限制。
在全局构造阶段就崩溃、且 bootloader 不支持回滚的镜像无法自救，只能用恢复模式或 USB 刷机，
因此每次发布都必须先在真机上完成验证清单。

## 7. 服务端契约

| 接口 | 语义 |
|---|---|
| `GET /api/v1/public/firmware/latest?channel=stable\|beta` | 公开的最新版本指针，设备端检查更新（2.7.1 起，不带凭据）与官网模拟器共用；含 `version`、`url`（或 `bin_url`）、`sha256`、`bytes`、`notes`，无发布时 404 |
| `GET <API 基址>/firmware/<version>/stockstick-<version>.bin` | 网站静态托管的镜像，支持 `Range`；设备不带凭据直接下载 |
| `POST /api/v2/device/events` | 回滚事件 `firmware_rolled_back` |
| `POST /api/v2/device/register` | 以新 `firmware_version` 注册即表示升级完成 |
| `POST /api/v1/miniapp/studio/devices/state`（手机调用） | 手机读取蓝牙 STATE 后中继：新 `firmware_version`、运行指标、回滚事件；6 小时内有过手机同步的设备不再自行注册 |

## 8. 迁移与已知限制

- **旧域名已失效。** 生产 API 只保留 `https://stockstick.plutokeating.beer`（证书链 Let's Encrypt
  YE1 → Root YE → ISRG Root X2），旧固件（1.5.0-project-stick.11/.12）固定旧域名和 GTS Root R4，
  已无法联网，不能通过 OTA 升级：需要用 SD 卡（设置或恢复模式）或 USB 刷入 2.0.0 一次。之后的升级都可以 OTA。
- 2.0.0 移除了电子书阅读器；旧 `settings.json` 中与阅读相关的字段在加载时被忽略，
  `.crosspoint/epub_*` 等阅读缓存不再使用，可以手动删除。
- 设备出厂 bootloader 是否启用回滚未经确认；应用层回滚在两种情况下都工作。
- 2.7.4 起固件经蓝牙传输（每块独立压缩，约 3.4 MB 的镜像传输量明显更小；可断点续传）。旧固件（`ota` < 4）
  仍由小程序发协议 3 的 `ota`，设备自己经 Wi‑Fi 下载；设备端「检查更新」始终经 Wi‑Fi。

## 9. 真机验证清单（每次发布）

1. SD 卡安装新版本：试运行日志出现 `Trial boot`，联网后出现 `confirmed (API reachable)`。
2. 小程序经蓝牙传输固件（`fw4`）：设备显示接收进度、校验/安装进度，重启后蓝牙 STATUS 的 `fw` 为新版本；
   传输中途断开蓝牙再连上，从块边界续传（日志 `Receiving firmware … resume at N`，N > 0），最终 SHA 校验通过。
3. 下载中断电/断网：再次上线后日志出现 `Resuming firmware download at`，最终 SHA 校验通过。
4. 目录中已删除的镜像下载失败（404）；电量低于 30% 时拒绝升级。
5. 故意安装会在启动后崩溃的测试构建：三次 panic 后自动回到旧版本，下次注册后服务端收到 `firmware_rolled_back` 事件。
6. 安装非 StockStick 镜像（上游 CrossPoint）：设置中的 SD 更新拒绝，恢复模式仍可刷入。
7. 深睡唤醒、Wi-Fi 自动重连、Studio 画面与按键反馈正常；阅读器相关入口已不存在。
