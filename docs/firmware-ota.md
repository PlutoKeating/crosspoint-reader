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
  否则自动回到旧分区，并把失败上报给服务端。
- **设备自己完成升级。** 小程序与后台只下达命令、展示状态；只有设备以新版本重新注册，服务端才把命令记为完成。

## 2. 版本标识

唯一来源是 `platformio.ini`：

```ini
[crosspoint]
version = 2.3.0            ; semver，展示给用户并与服务端目录比对（≤ 31 字节）
build = 20300              ; 单调递增的构建号，OTA 比较它而不是字符串
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
| `lang/*.lang` | 与本构建匹配的 SD 卡语言包 |
| `catalogue.json` | 管理后台固件目录的登记内容 |

脚本会拒绝：版本不符合目录格式、构建号未递增、描述符与配置或 HEAD 不一致、镜像超出分区。
推送 `v<version>` 或 `<version>` 标签会触发 `.github/workflows/release.yml`，它校验标签与版本一致后执行同一脚本并上传产物。

3. 把 `.bin` 上传到 HTTPS 存储（URL 不得含凭据或片段；存储支持 `Range` 时续传更省流量）。
4. 在管理后台 `/console/studio` 登记 `catalogue.json` 中的版本、URL、SHA-256、字节数与说明。
5. 需要下线时在后台“撤回”：尚未进入安装阶段的命令会失败，安装前设备还会再向服务端确认一次。

预发布版本（带 `-` 后缀）不会出现在设备端“检查更新”的稳定通道，但所有者仍可在小程序中手动选择。

## 4. 升级入口

| 入口 | 发起方 | 路径 |
|---|---|---|
| 小程序设备详情 | 所有者 | 创建 `studio_command`，设备在 Studio 轮询中发现并执行 |
| 设备「设置 → 系统 → 固件更新」 | 设备（代表绑定的所有者） | `GET /firmware/latest` → 确认 → `POST /commands {action:"request"}` → 立即执行 |
| 设备「SD 卡固件更新」 | 用户 | 选择 `/` 或 `/firmware` 下的 `.bin`；只接受适用于本机的 StockStick 镜像，并启用试运行 |
| 恢复模式（按住左侧键开机） | 用户 | 同上但不做描述符限制，用于救砖；只有 StockStick 镜像启用试运行 |
| USB / 网页刷机 | 开发者 | 不经过试运行，视为可信镜像 |

## 5. 设备端执行流程（OTA 协议 2）

设备在配对/注册时上报 `firmware_version`、`firmware_build` 与能力 `ota: 2`、`panel: xteink_x3|xteink_x4`。

```
Studio 轮询（每 5 秒，Wi-Fi 在线且无 BLE 会话）
  └─ 响应 command_pending=true（旧服务端无此字段时每次都查）
      └─ GET /commands → {id, version, sha256, bytes, firmware_id}
          0. 新固件仍在试运行时不开始安装（命令保留，试运行结束后再执行）
          1. 元数据校验；电量 ≥ 30%；Studio 未在接收、BLE 未连接
          2. 暂停 BLE 广播，持有电源锁，屏幕切换为升级进度页
          3. POST downloading → 下载到 /.crosspoint/studio/firmware.tmp
             · firmware.meta 记录 "<sha> <字节>"；同一镜像的半截文件（包括被重新下达的命令）带 offset 续传（206）
             · 每写约 256 KB 刷新一次 SD 文件，掉电后续传偏移以文件实际大小为准
             · 服务端忽略 offset（返回 200）时从头下载
             · 网络类失败不结束命令：保持 downloading，60 秒后在后续轮询中续传；撤回/无权限（401/403/404/409/416）才上报失败
          4. POST verifying → 大小 + SHA-256 → 描述符与安装策略
          5. POST installing（服务端再次确认未撤回）→ 写入另一 OTA 槽（同一时间只允许一个安装），
             写完后读回整个分区计算 SHA-256，与写入的数据比对
          6. 切换启动槽之前写入试运行记录 → 切换 otadata
          7. POST restarting → 重启
```

任一步失败都会以 `failed` 和原因（如 `Firmware checksum mismatch`、`VERSION_MISMATCH`、
`BELOW_MINIMUM_BUILD`、`low_battery`）上报，并在屏幕上提示；半截下载保留供下次续传，校验失败的文件会被删除。

## 6. 试运行与自动回滚

实现：`src/network/OtaTrial.*`，策略（可单元测试）：`lib/ProjectStick/StickFirmware.*`。
状态保存在 NVS 命名空间 `stick_ota`，掉电、深睡、重启后都保留。

| 时机 | 行为 |
|---|---|
| 写完新镜像、切换启动槽之前 | 记录旧槽、目标槽、命令 ID、目标版本，`armed=true` |
| 每次启动（`setup()` 最早阶段） | 运行在旧槽 → 记为回滚（`bootloader_rollback`）；上一次是 panic/看门狗复位 → 失败次数 +1；达到 3 次 → 切回旧槽并重启（`repeated_crash`）。掉电、欠压复位和深睡唤醒不计入。计数写入 NVS 后即向 bootloader 标记镜像有效，之后由应用层计数负责 |
| 运行中（主循环） | 任意一次 StockStick API 返回 HTTP 状态 → 确认；同一次 Wi-Fi 连接期间，API 请求周期在传输层失败（每 30 秒最多计 1 次，Wi-Fi 断开即清零）累计 ≥ 5 次且首末相隔 ≥ 5 分钟 → 回滚（`cloud_unreachable`）；从未联网且运行满 2 分钟 → 确认；超过 60 分钟仍无结论 → 确认。刷写进行中不做判定 |
| 正常进入深度睡眠 | 视为健康，确认 |
| 下一次注册 / Studio 轮询 | 回滚结果以 `failed`、`Rolled back from <版本> (<原因>)` 上报对应命令 |

`verifyRollbackLater()` 返回 true，Arduino 不再在 `initArduino()` 中自动确认镜像，而是由
`ota_trial::onBoot()` 在写入计数后调用 `esp_ota_mark_app_valid_cancel_rollback()`。
本仓库构建的 bootloader 启用了回滚（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`），它会把
PENDING_VERIFY 状态下的**任何**复位都当作失败；若把确认推迟到健康判定，一次掉电就会误回滚。
因此 bootloader 只负责 `onBoot()` 之前就崩溃的镜像，之后交给上表的应用层策略。出厂 bootloader
若不支持回滚，则完全依靠应用层计数。试运行期间设置中的 SD 卡更新和云端安装都会等待，
避免覆盖回滚目标分区；恢复模式不受此限制。
在全局构造阶段就崩溃、且 bootloader 不支持回滚的镜像无法自救，只能用恢复模式或 USB 刷机，
因此每次发布都必须先在真机上完成验证清单。

## 7. 服务端契约

| 接口 | 语义 |
|---|---|
| `GET /api/v2/device/studio` | 返回 `command_pending`，设备据此决定是否查询命令 |
| `GET /api/v2/device/commands` | 返回当前命令；设备以目标版本注册后自动 `completed`；重启后版本不符记为失败；目录已撤回则失败 |
| `POST /api/v2/device/commands` | 状态上报 `downloading/verifying/installing/restarting/failed`，可带 `progress`；`{action:"request", firmware_id}` 由设备发起升级（只能升级到更新的有效版本） |
| `GET /api/v2/device/firmware?command_id=&offset=` | 仅代理当前命令的镜像；`offset` 续传返回 206 + `Content-Range` |
| `GET /api/v2/device/firmware/latest?channel=stable\|beta` | 设备端检查更新 |

## 8. 迁移与已知限制

- **旧域名已失效。** 生产 API 只保留 `https://stockstick.plutokeating.beer`（证书链 Let's Encrypt
  YE1 → Root YE → ISRG Root X2），旧固件（1.5.0-project-stick.11/.12）固定旧域名和 GTS Root R4，
  已无法联网，不能通过 OTA 升级：需要用 SD 卡（设置或恢复模式）或 USB 刷入 2.0.0 一次。之后的升级都可以 OTA。
- 2.0.0 移除了电子书阅读器；旧 `settings.json` 中与阅读相关的字段在加载时被忽略，
  `.crosspoint/epub_*` 等阅读缓存不再使用，可以手动删除。
- 设备出厂 bootloader 是否启用回滚未经确认；应用层回滚在两种情况下都工作。
- SD 语言包中的文案若改变了 `printf` 转换说明（如把 `%02u` 换成 `%s`），该条目被忽略并显示内置文案。
- BLE 通道只传输 Studio 画面，不传输固件（6 MB 级镜像经 BLE 需十余分钟且占用会话）。

## 9. 真机验证清单（每次发布）

1. SD 卡安装新版本：试运行日志出现 `Trial boot`，联网后出现 `confirmed (API reachable)`。
2. 小程序发起升级：进度页显示下载/校验/安装，重启后命令变为完成。
3. 下载中断电/断网：再次上线后日志出现 `Resuming firmware download at`，最终 SHA 校验通过。
4. 撤回目录后发起的命令在安装前失败；电量低于 30% 时拒绝升级。
5. 故意安装会在启动后崩溃的测试构建：三次 panic 后自动回到旧版本，并在小程序看到回滚原因。
6. 安装非 StockStick 镜像（上游 CrossPoint）：设置中的 SD 更新拒绝，恢复模式仍可刷入。
7. 深睡唤醒、Wi-Fi 自动重连、Studio 画面与按键反馈正常；阅读器相关入口已不存在。
