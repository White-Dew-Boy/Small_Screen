# Small_Screen

ESP32-S3 多功能桌面小屏固件：320×240 触摸界面（LVGL 8.3.11），把温湿度、九轴姿态、
WiFi/MQTT 状态、RGB 灯、SD 卡文件、图片轮播、WAV 播放、系统信息和 PC 性能监控
集中到一块小屏上，全部功能都能在触摸屏上操作。

- **主控**：ESP32-S3（160 MHz，8 MB Octal PSRAM @ 80 MHz，16 MB Flash）
- **框架**：ESP-IDF **v6.0.1**
- **GUI**：LVGL 8.3.11（组件管理器拉取），UI 默认横屏 320×240
- **日志口**：USB-Serial-JTAG（Type-C），UART0 留给 JY901S IMU

---

## 1. 硬件与接线

| 模块 | 信号 | GPIO |
|---|---|---|
| LCD（ILI9341，SPI） | CS / RST / RS(DC) / MOSI / SCK / MISO / LED(背光) | 21 / 47 / 48 / 38 / 39 / 41 / 40 |
| 触摸（FT6336G，I2C） | INT / RST，SCL / SDA | 2 / 42，13 / 12 |
| microSD（SPI，**与 LCD 共用总线**） | CS，共用 MOSI/SCK/MISO | 1，38 / 39 / 41 |
| 按键 | KEY1 / KEY2 / KEY3 | 4 / 5 / 6 |
| 喇叭（MAX98357A，I2S） | BCLK / LRCLK / DIN | 17 / 18 / 16 |
| 九轴 IMU（JY901S，UART0 @9600） | TXD0 / RXD0 | 43 / 44 |
| RGB LED（WS2812 ×3，RMT） | DIN | 8 |
| USB | D+ / D- | 20 / 19 |
| 模块电源控制（GPA） | AUDIO_PWR / TEMP_PWR / IMU_PWR / LCD_PWR | 15 / 7 / 11 / 10 |

**关键约束**：LCD 与 microSD 共用 **SPI3**（单主机、单 DMA 通道）。因此**所有** LCD 刷屏和
文件系统访问都被收敛到 LVGL 任务里串行执行（详见 §4）。从第二个任务访问 SD 卡会与 LCD 的
DMA 传输冲突。

模块电源由 `drv/power.c` 通过 GPA 引脚控制：LCD/温湿度/IMU 在启动时打开并常开，音频功放
只在播放期间打开。

---

## 2. 功能与界面

| 页面 | 源码 | 说明 |
|---|---|---|
| Home | `gui/ui_home.c` | 页面菜单、时钟（SNTP）、Deep Sleep 按钮 |
| Sensor | `gui/ui_sensor.c`（+ `ui_accel.c` / `ui_gyro.c`） | SHTC3 温湿度；JY901S 加速度/角速度/姿态子页 |
| WiFi | `gui/ui_wifi.c`（+ `ui_saved_wifi.c` / `ui_nearby_wifi.c`） | 状态、IP/RSSI、**射频开关**、已保存网络（连接/忘记）、附近网络扫描 |
| MQTT | `gui/ui_mqtt.c`（+ `ui_mqtt_interval/history/config/pub/sub.c`） | 连接/断开、发布间隔、消息历史、broker 配置、手动发布/订阅 |
| LED | `gui/ui_led.c`（+ `ui_led_preset.c` / `ui_led_custom.c`） | 3 颗 WS2812 单颗或全部、颜色预设、自定义色、亮度滑条 |
| SD Card | `gui/ui_sd.c`（+ `ui_files.c` / `ui_gallery.c` / `ui_player.c`） | 挂载状态、上传服务器开关、文件浏览（删除）、图片轮播、**音频播放页** |
| SysInfo | `gui/ui_sysinfo.c` | 堆/PSRAM、任务与 CPU 占用、每任务栈水位、关于页（芯片/Flash/PSRAM/IDF 版本） |
| PC-Perf | `gui/ui_pc_perf.c`（+ `ui_pc_perf_cfg.c`） | CPU/内存/上下行/GPU/磁盘，来源可选 MQTT / BLE / 关闭 |

### 按键

| 按键 | 作用 |
|---|---|
| KEY1（GPIO4） | 电源锁存；深睡后按它唤醒（Home 页的 Deep Sleep 按钮触发） |
| KEY2（GPIO5） | 循环切换 7 个主页：Sensor → WiFi → MQTT → LED → SD → SysInfo → PC-Perf |
| KEY3（GPIO6） | 回到 Home |

### 可开关的功能

| 功能 | 入口 | 重启后保留 |
|---|---|---|
| WiFi 射频（真关 RF，`esp_wifi_stop()`） | WiFi 页 `WiFi Off` / `WiFi On` | 否（开机射频为开） |
| HTTP 上传/下载服务器 | SD 页 `Start Upload` / `Stop Upload` | 否 |
| MQTT 连接 | MQTT 页 `Connect` / `Disconnect` | 配置存 NVS，连接状态不保留 |
| PC-Perf 数据源（MQTT / BLE / Off） | PC-Perf 配置页 | 是（NVS） |
| WiFi 连接与凭据 | 附近/已保存网络页 | 是（NVS） |
| RGB LED 颜色与亮度 | LED 页 | 否 |
| 音频播放与音量 | Audio 页 | 音量每次开机回到 Kconfig 默认值 |

---

## 3. 音频播放（沙沙声问题与设计）

WAV → MAX98357A 的链路分两段，跨任务解耦：

```
SD 卡 ──fread──► PSRAM 预读 FIFO ──memcpy──► 环形缓冲 ──► 内部 RAM 中转 ──► I2S DMA
        (LVGL 任务)   128 KB / 16 KB 每次     4096 帧 ≈93 ms      4×240 帧 ≈21.8 ms
                                                │                        ▲
                                                └──► audio_play 任务 ─────┘
```

- **生产者跑在 LVGL 任务上**（SD 与 LCD 共用 SPI3 的约束），**消费者是独立的 `audio_play` 任务**，
  它只碰 I2S、绝不访问文件系统。
- 一次 LVGL tick 的成本约 40 ms（10 ms 循环 + 两次 5–12 ms 的 SD 读 + 一次局部刷屏），而早先
  2048 帧/tick 的预算只覆盖 46 ms 音频，环永远长不到 46 ms 以上；实测 166 s 播放出现
  **9828 次环饥饿 + 3198 次 DMA 欠载**，30 s 的曲子播了 32.4 s（8% 被 `auto_clear` 补零），
  听感就是"沙沙声"（只在有声音时听得出来，静音段补零听不出）。
- 现在的做法：**128 KB PSRAM 预读 FIFO**（每次 SD 读 16 KB，每个 tick 最多读一次），环形缓冲
  永远从 PSRAM 微秒级补齐，SD 延迟再也到不了 DMA；预算提升到 4096 帧让一次 tick 能填满整个环；
  I2S 启动**之前**先预填充；软件音量在转换时应用。
- 修复后实测：**0 DMA 欠载、0 环饥饿**，30 s 曲子 30.06 s 播完，SD 读取约 1.1 MB/s。
- 调试开关（`app/audio_player.c`）：`AUDIO_TELEMETRY 1` 打开每秒一行的 `stream:` 遥测；
  `AUDIO_RA_BYTES 0` 退回"直读 SD"的老路径。

支持格式：RIFF/WAVE、PCM 16-bit、单/双声道、8 kHz–96 kHz（MP3/24-bit/float 会被拒绝并记日志）。

---

## 4. 架构与关键约定

- **单一 LVGL 任务**（`lvgl_task`，prio 5，**固定在 core 0**，16 KB 栈）：LVGL 渲染、LCD 同步
  刷屏、**全部** SD 文件系统访问（音频生产、文件浏览、图库、上传服务器执行）、音频补给。
- **LCD 同步刷屏**：`flush_cb` 里发 DMA 后等待完成信号量，保证 LCD 与 SD 的 SPI 事务不交错。
- **上传服务器用邮箱解耦**：httpd 任务只把命令入队并等 ack，真正的 `fopen/fwrite/fread` 由
  LVGL 任务执行，绝不在 httpd 任务里碰文件系统。
- **内部 RAM 是最稀缺资源**：大缓冲一律放 PSRAM
  （LVGL 绘制缓冲 2×240×80、图库帧缓冲 320×240×2 = 150 KB、音频预读 128 KB、
  `EXT_RAM_BSS_ATTR` 的目录缓存与音频页文件表）；NimBLE 内存池也配置为外部
  （`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`）。播放音频时内部堆仅剩约 15 KB。
- **跨核可见性**：音频环形缓冲放内部 RAM（LVGL 任务与 `audio_play` 任务可能不同核）。
- 主要任务：

| 任务 | 优先级 | 栈 | 职责 |
|---|---|---|---|
| `lvgl_task` | 5（core 0） | 16 KB | UI、刷屏、SD I/O、音频补给 |
| `audio_play` | 6 | 3 KB | 环 → I2S（每首曲子创建/删除） |
| `key_task` | 6 | 2 KB | 按键扫描 → 切页请求 |
| `shtc3_task` / `imu_task` | 5 | 4 KB | 传感器读取 → 投递给 LVGL |
| `heartbeat` | 1 | 2 KB | 每 60 s 打印存活与内部堆 |
| httpd / NimBLE host / lvgl tick | IDF 管理 | — | 上传服务器、BLE 外设、1 ms LVGL tick |

---

## 5. 目录结构

```
main/
  main.c                  app_main：初始化各驱动/服务，创建所有页面与任务
  Kconfig.projbuild       工程级 Kconfig（见 §7）
  idf_component.yml       组件依赖（LVGL / ILI9341 / led_strip / esp_mqtt）
  app/                    应用层（不含 UI）
    wifi_manager          扫描/连接/凭据 NVS/**射频开关**
    mqtt_manager          连接、遥测发布、命令与扩展订阅、配置 NVS
    upload_server         HTTP 上传/下载服务器（邮箱模型）
    audio_player          WAV 流式播放（预读 FIFO + 环形缓冲 + 音量）
    ble_perf              NimBLE PC 性能外设（PC_perf 源的 BLE 通道）
    pc_perf_src           数据源选择（NVS 持久化）
    pc_perf_mqtt          MQTT 源的解析与缓存
    time_manager          SNTP 与本地时区
    sd_file               目录列举（供文件浏览/图库/音频页共用）
  drv/                    驱动层
    lcd_driver            ILI9341 + SPI3 总线 + 同步刷屏信号量
    ft6336 / i2c_bus      触摸与 I2C
    sd_card               SD 挂载/探测（与 LCD 共用 SPI3）
    max98357a             I2S 功放（DMA 4×240 帧）
    jy901s                JY901S 九轴 IMU（UART0）
    shtc3 / rgb_led / key / power
  gui/                    LVGL 界面
    lv_port_disp/indev    显示与触摸移植（含 1 ms tick）
    ui_home ... ui_pc_perf 各页面
partitions.csv            nvs 24K + phy 4K + factory app 8M
sdkconfig.defaults        工程默认配置（16MB Flash、OCT PSRAM 80M、USB-Serial-JTAG 日志等）
```

---

## 6. 构建与烧录

需要 **ESP-IDF v6.0.1**（仓库不含 `build/` 与 `managed_components/`，组件由组件管理器自动拉取：
`lvgl/lvgl@8.3.11`、`espressif/esp_lcd_ili9341@^2.0`、`espressif/led_strip@^3.0.3`、
`espressif/mqtt@^1.1.0`）。

```bash
idf.py set-target esp32s3      # 首次
idf.py build
idf.py -p COM5 flash monitor   # 日志在 USB-Serial-JTAG 上
```

注意事项：

- `sdkconfig` **已被跟踪**（含 `CONFIG_AUDIO_PLAYER_VOLUME` 等生成项），改 Kconfig 后请一并提交。
- 分区表是自定义的（`partitions.csv`，app 8 MB）。`nvs`/`phy_init` 的偏移保持默认，升级分区表
  不会丢 WiFi 凭据与 MQTT 配置。
- UART0 被 JY901S 占用，**不要把日志切回 UART0**（否则 IMU 收到日志字节）。
- 无 SD 卡、无传感器、无 IMU 都不致命：对应页面显示 `--`，其余功能照常。

---

## 7. 配置项

**编译期（`menuconfig` → 各菜单，见 `main/Kconfig.projbuild`）**

| 选项 | 默认 | 说明 |
|---|---|---|
| `WIFI_MAX_RETRY` | 10 | 单个网络的重连次数，之后轮换到下一个已保存网络 |
| `MQTT_BROKER_URI` / `_USERNAME` / `_PASSWORD` | wss://broker.example.com:443/mqtt | 可在 UI 里改并存 NVS |
| `MQTT_TELEMETRY_INTERVAL` | 5 | 温湿度发布间隔（秒） |
| `MQTT_PC_PERF_TOPIC` | pc/performance | PC 性能订阅主题；**留空＝关闭 MQTT 这条 PC-Perf 路径**（自动退回 BLE） |
| `TIME_TZ` / `TIME_NTP_SERVER` | CST-8 / pool.ntp.org | Home 页时钟 |
| `UPLOAD_SERVER_PORT` / `UPLOAD_DIR` / `UPLOAD_MAX_SIZE_MB` | 80 / /sdcard/esp32_files / 64 | 上传服务器（目录不存在会自动创建，重名加 `_1`） |
| `AUDIO_PLAYER_VOLUME` | 40 | 软件音量默认值（功放模块 9 dB 增益，满幅会削顶）；0＝静音但流水线继续跑 |

**运行期（UI 修改，存 NVS）**：WiFi 凭据（最多 5 个）、MQTT 配置与间隔、PC-Perf 数据源。

---

## 8. SD 卡文件格式

| 用途 | 格式 |
|---|---|
| 图片轮播（SD 页 → Slide Show） | **320×240 RGB565 原始数据 `.bin`，必须恰好 153,600 字节**（会校验尺寸，不匹配的文件被忽略） |
| 音频播放（SD 页 → Audio Player） | WAV，PCM 16-bit，单/双声道，8 k–96 k，放在 `/sdcard/esp32_files` 下 |
| 上传/下载 | 浏览器打开 `http://<设备IP>:80/`，文件落在 `/sdcard/esp32_files`（扁平目录） |

> 轮播 `.bin` 的字节布局就是 LVGL 16 位色的原始像素：**小端 RGB565**
> （`((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3)`），逐行连续排列，无文件头、
> 无行填充，因此长度恰好是 `320 × 240 × 2 = 153,600` 字节。用任意图像工具把图片缩放到
> 320×240 并导出为 RGB565 原始数据即可；设备端加载时不做字节交换（刷屏时由 LCD 驱动统一处理）。

---

## 9. PC 性能数据的两种来源

**MQTT**（`PC_PERF_SRC_MQTT`，默认）：订阅 `CONFIG_MQTT_PC_PERF_TOPIC`（QoS 1），载荷为扁平 JSON：

```json
{"timestamp": 1, "cpu": 23.5, "memory": 61.0, "upload_speed": 12.3,
 "download_speed": 45.6, "gpu": 8.0, "disk": 3.0}
```

（`cpu`/`memory`/`gpu`/`disk` 为百分比，`upload_speed`/`download_speed` 为 KB/s。）

**BLE**（`PC_PERF_SRC_BLE`）：设备作为外设广播 `ESP32_PC_Monitor`，PC 端写入固定特征值：

```
service        4fafc201-1fb5-459e-8fcc-c5c9c331914b
characteristic beb5483e-36e1-4688-b7f5-ea07361b26a8   (write)
帧 v2：23 字节，小端，pack '<BBBHHIIHHhH'
  [0] magic 0x50 ('P')   [1] version 0x02   [2] flags（bit0..7 = cpu/mem/up/down/gpu/disk/temp/fps）
  cpu/mem 为 u16 ×0.1 %
```

BLE 广播只在 PC-Perf 页可见且数据源为 BLE 时开启，离开页面即停止（省 RAM 与射频）。

---

## 10. 已知限制

- 内部 RAM 长期紧张（播放音频时约 15 KB 空闲）。新增缓冲请优先用 PSRAM（`MALLOC_CAP_SPIRAM`
  或 `EXT_RAM_BSS_ATTR`），并注意 LVGL 组件版本锁在 **8.3.11**（不是 v9 API）。
- 所有页面在 `app_main` 里一次性创建（约 20 个 screen），启动瞬间内存占用较高。
- 音频播放没有暂停/拖动进度，只有播放/停止与音量；播放进度数据（`position_ms`）已在
  `audio_player_get_status()` 里提供，但界面刻意不显示（播放期间刷屏会抢 SD 时间）。
- 上传服务器运行期间若关闭 WiFi 射频，服务器仍在监听但不可达，射频恢复后可用（IP 可能变化）。
- MQTT 采用 `wss://` + 证书捆绑包校验，需要设备能访问互联网与正确时间（SNTP 同步后握手才成功）。

---

## 11. 开发约定

- **不要自动构建/烧录**：构建由开发者在自己的 ESP-IDF 环境中执行（见 `AGENTS.md`）。
- 提交信息用 Conventional Commits（`feat(scope): …` / `fix(scope): …`），正文说明"为什么"，
  涉及实测数据的问题把数字写进提交信息。
- 改动 SD/SPI/LVGL 相关代码前先读 `drv/sd_card.h`、`drv/lcd_driver.h`、`app/upload_server.h`
  顶部的约束说明。
