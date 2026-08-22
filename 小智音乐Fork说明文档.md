# 小智音乐 Fork 说明文档

> Fork 来源：`benjenq/xiaozhi-esp32-music-player`（分支 `esp-adf/http-mp3-player`）
> Fork 仓库：`https://github.com/smart-open/xiaozhi-esp32-music-player.git`（分支 `esp-adf/http-mp3-player`，提交 `14e237f`）
> 目标硬件：ESP32-S3（16MB Flash / 8MB PSRAM）+ bread-compact-wifi 板型
> 配套服务端：MCPilot（本地 Subsonic 兼容层 + MCP 音乐服务）
> 文档日期：2026-08-22

---

## 一、Fork 概述：这个 Fork 做了什么

原版固件的工作方式是**设备自主查询**：烧录时写入 Subsonic 服务器地址，设备收到"播放 XX"后自己去服务器搜歌、拼 URL、拉歌词封面再播放。服务器换 IP / 换端口 / 换机器都要重新烧录固件。

本 Fork 改为**云端 MCP 音乐服务 + 设备播放**模式：

```
语音"你好小智，播放江南"
   │
   ▼
小智云端 AI ──调用──► MCP 音乐服务（MCPilot 后端，music_search/music_play）
   │                    │  检索引擎 v1.2.0：拼音/首字母/繁简/错字容错/相关度排序
   │                    │  返回 song.url = http://<自动探测的局域网IP>:8000/subsonic/rest/stream.view?...&id=xx.mp3
   │◄──── 返回 url ─────┘
   │
   ▼ 调用设备工具
固件 self.music.play_url(url, title, artist)
   │  ParseStreamUrl(): 从 url 解析出服务器 base 和歌曲 id → 歌词/封面地址运行时自适应
   │  play_scheduler_task: 等语音会话结束 → 释放内部 RAM → 再建播放任务
   ▼
STREAM_TASK（10KB 栈）
   │  1) get_song_lyrics(): 查询歌词（.lrc → OpenSubsonic 结构化歌词）
   │  2) HTTP → MP3 解码 → RAW PCM 管线（ESP-ADF audio_pipeline）
   ▼
I2S 输出（44100Hz，立体声混音单声道）+ 屏幕歌词逐行同步 + 状态栏"♪ 播放中"
```

**核心收益**：
- 固件不烧录任何服务器地址，服务端 IP 变更、换电脑部署**无需重新烧录**
- 检索能力升级到服务端（拼音缩写 `ljj`、错字"不再优豫"、随机推荐等）
- 修复了原版在 ESP32-S3 上播放的一系列稳定性问题（详见变更明细）

---

## 二、变更文件说明

### 1. `main/boards/common/mp3_player.cc`（核心，重写播放调度）

| 变更 | 说明 |
|---|---|
| MCP 工具 `play_song` → `play_url` | 参数从 `song_name/artist_name` 改为 `url/title/artist`；AI 先经音乐服务查询拿 url，再调本工具播放，工具说明中明确两步用法 |
| URL 解析 | 调用 `ParseStreamUrl()` 从播放 URL 提取服务器 base 与歌曲 id，写入 `song_id/cover_id` 供歌词、封面查询使用；非 Subsonic URL 时跳过歌词（日志提示） |
| 新增 `play_scheduler_task` | `Play()` 不再直接创建 8KB 播放任务（语音会话期间内部 RAM 不足会失败），改为先建 2.5KB 轻量调度任务：等小智说完 → 强制结束会话释放 AFE/Opus/MQTT 内存 → 再重试（最多 10 次）创建播放任务 |
| 歌词查询移入 `streaming_task` | 原先放在调度任务（2.5KB 栈）导致 HTTP+JSON 深调用栈溢出、设备重启；移入 10KB 栈的播放任务后修复 |
| STREAM_TASK 栈 8KB → 10KB | 为歌词 HTTP+JSON 解析预留调用栈 |
| 状态栏"播放中" | 播放启动后状态栏显示 `MUSIC_PLAYING`（"♪ 播放中"）替代"待命"，避免误判未播放；结束/中断后若设备仍待命则恢复"待命" |
| 诊断日志 | 播放循环新增：首块 PCM 采样值校验（识别静音数据）、15 秒心跳（PCM 读取量 / I2S 写入样本数 / 歌词进度）、结束时输出播放统计（用于无声问题排查） |

### 2. `main/boards/common/mp3_player.h`

- 新增声明：`SetSubsonicBaseUrl()`、`ParseStreamUrl()`、`play_scheduler_task()`

### 3. `main/boards/common/mp3_player_http.cc`

| 变更 | 说明 |
|---|---|
| `base_url` 改为运行时可覆盖 | `const static` → `static`；新增 `SetSubsonicBaseUrl()` 供 play_url 从播放 URL 动态推导歌词/封面查询地址，`CONFIG_SUBSONICAPI_URL` 仅作初始默认值（可为空） |
| 新增 `ParseStreamUrl()` | 解析 Subsonic stream URL → `{base, song_id}`；识别 `stream.view`/`stream` 端点，支持查询串中任意位置的 `id` 参数 |
| 新增 `url_decode()` | `%XX` 十六进制与 `+` 转空格解码，还原中文文件名 song_id |
| HTTP 超时 1.5s → 10s | `get_subsonic_response()` 与封面下载两处；低功耗 WiFi 下响应延迟可达数秒，1.5s 会误判超时 |

### 4. `main/boards/bread-compact-wifi/compact_wifi_board.cc`

- `InitializeTools()` 中创建 `Mp3Player`（面包板功放单声道，不开立体声）
- 实现 `GetMusicPlayer()` 虚函数，供 Application 在会话切换时停止音乐

### 5. `main/audio/audio_codec.cc`

- 启动日志附带当前音量值（无声问题排查时确认音量状态）

### 6. `main/assets/locales/zh-CN/language.json`、`en-US/language.json`

- 新增字符串 `MUSIC_PLAYING`（zh-CN："♪ 播放中"；en-US："♪ Playing"）
- 说明：`lang_config.h` 为构建生成产物（已 gitignore），语言字符串必须改 `locales/*/language.json` 源文件；其余语言经 en-US 回退机制自动获得该键

### 7. `sdkconfig.defaults`

- 板型锁定 `CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y`
- `CONFIG_SUBSONICAPI_URL=""`、`CONFIG_SUBSONICAPI_PARA=""`（地址改由播放 URL 运行时推导，无需烧录）
- 编译优化改 `CONFIG_COMPILER_OPTIMIZATION_PERF=y`

### 8. `sdkconfig.defaults.esp32s3`

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 2048 → 512：小内存分配优先走 PSRAM，保留更多内部 RAM 给 WiFi/LWIP/任务栈，修复语音会话期间 TCP 接收任务创建失败（内部 RAM 耗尽）问题

### 9. 新增开发辅助脚本

| 文件 | 用途 |
|---|---|
| `build_mcpilot.bat` | 一键编译（固定本机 ESP-IDF v5.5.5 环境路径，按需修改） |
| `flash_mcpilot.bat` | 一键烧录 COM6（端口按需修改） |
| `monitor_serial.py` | 串口日志抓取：`python monitor_serial.py <秒数> <输出文件>`，观察启动/播放/唤醒词状态 |

---

## 三、环境需求

### 3.1 硬件

| 项目 | 要求 | 说明 |
|---|---|---|
| 主控 | ESP32-S3，**16MB Flash + 8MB PSRAM** | 8MB PSRAM 是歌词+封面+播放管线稳定运行的保障；flash 小于 16MB 需自行调整分区表 |
| 板型 | bread-compact-wifi（`CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI`） | 其他板型需改 `sdkconfig.defaults` 中的板型配置并确认喇叭/编解码接线 |
| 音频输出 | I2S 功放 + 喇叭（单声道） | 无屏也可运行（歌词/状态栏不显示，不影响播放） |
| 串口 | USB 转串口（CH343/CP2102/CH340 均可） | 本例 COM6，115200 波特率 |
| 网络 | 设备与服务端同一局域网 | 设备 WiFi、服务端有线/无线均可，需互通 |

### 3.2 软件（编译机，Windows）

| 组件 | 版本 | 安装位置（本例） | 用途 |
|---|---|---|---|
| ESP-IDF | **v5.5.5**（要求 ≥5.5.2） | `C:\Espressif\frameworks\esp-idf-v5.5.5` | 固件编译烧录（含 Python 3.12 工具环境 `idf5.5_py3.12_env`） |
| ESP-ADF | `release/v2.x`（eac70fd） | `D:\ai_work\esp-adf` | audio_pipeline / esp-adf-libs 等音频组件来源 |
| Python | 3.10+ | 系统 PATH | 运行 MCPilot 后端与 music_service |
| pypinyin | 最新 | pip 安装 | 音乐检索拼音索引 |
| pyserial | 最新 | pip 安装 | `monitor_serial.py` 串口监控 |
| MCPilot | 本项目 | `D:\ai_work\MCPilot` | Subsonic 兼容层 + MCP 音乐服务 |

> 网络要求：编译时 IDF Component Manager 需从 GitHub 拉取依赖组件（lvgl、esp-sr 等），国内网络建议配置代理。

---

## 四、环境安装步骤

### 4.1 安装 ESP-IDF v5.5.5

1. 下载 Windows 离线安装器（ESP-IDF v5.5.5 Offline Installer）或在线安装器
2. 安装目录选 `C:\Espressif`（安装器自动完成：IDF 框架、交叉编译工具链、`idf5.5_py3.12_env` Python 虚拟环境）
3. 验证：打开 "ESP-IDF 5.5 PowerShell" 快捷方式，执行 `idf.py --version` 应显示 5.5.5

### 4.2 安装 ESP-ADF（音频框架）

固件通过 `ADF_PATH` 环境变量引用 ESP-ADF 的音频组件（audio_pipeline、esp-adf-libs 等，见顶层 `CMakeLists.txt` 的 `EXTRA_COMPONENT_DIRS`）。

```powershell
# 克隆 ADF（release/v2.x 分支）
git clone -b release/v2.x https://github.com/espressif/esp-adf.git D:\ai_work\esp-adf
cd D:\ai_work\esp-adf

# 只需初始化 esp-adf-libs 子模块（预编译音频库，必须！）
git submodule update --init components/esp-adf-libs

# 验证：该目录应存在且为有效 git 仓库
git -C components\esp-adf-libs log -1   # 期望: 3472016
```

> ADF 的 `esp-idf` 子模块**不需要**初始化——编译用的是步骤 4.1 独立安装的 ESP-IDF v5.5.5。

### 4.3 安装服务端依赖（Python）

```powershell
# MCPilot 后端环境（Python 3.10+）
pip install pypinyin      # 音乐检索：拼音/首字母匹配
pip install pyserial      # 串口监控脚本
# MCPilot 自身依赖见其仓库 README（FastAPI/uvicorn/websockets 等）
```

### 4.4 部署 MCPilot 服务端

1. 按 MCPilot 仓库说明安装并启动后端（本例：`D:\ai_work\MCPilot\scripts\start-backend.bat`，监听 `0.0.0.0:8000`）
2. 配置音乐服务 `backend/configs/mcp-services/music.yaml`：

```yaml
id: music-service
name: Music Service
version: 1.2.0
transport:
  type: stdio
  command: python
  args:
    - scripts/music_service.py
  env:
    MUSIC_DIR: "D:\\音乐\\musics"   # 改成你的曲库目录
  timeout: 30
```

3. 准备曲库目录（`MUSIC_DIR` 指向的目录）：
   - 音乐文件命名建议 `歌手 - 歌名.mp3`（检索按此解析歌手/歌名）
   - 歌词文件与音乐同名 `.lrc`（`歌手 - 歌名.lrc`），有则播放时逐行同步显示
4. 将 MCPilot 接入小智云端（xiaozhi 适配器，配好设备激活），确保设备与后端在同一局域网
5. 验证服务端：

```powershell
# Subsonic 兼容层连通性（应返回 JSON pong）
curl "http://127.0.0.1:8000/subsonic/rest/ping.view?u=admin&p=1111&v=1.16.1&c=xiaozhi&f=json"

# MCP 服务注册（应看到 music-service 及 music_list/music_search/music_play）
curl "http://127.0.0.1:8000/api/v1/services"
```

> 防火墙：需放行 8000 端口入站（局域网），否则设备访问不到流式地址。

---

## 五、固件烧录流程（详细步骤）

### 5.1 获取固件源码

```powershell
git clone -b esp-adf/http-mp3-player https://github.com/smart-open/xiaozhi-esp32-music-player.git D:\ai_work\xiaozhi-esp32-music-player
cd D:\ai_work\xiaozhi-esp32-music-player
```

### 5.2 配置编译环境

每次编译前需设置环境变量（推荐直接使用仓库自带脚本，见 5.5）：

```powershell
$env:ADF_PATH = "D:\ai_work\esp-adf"          # ESP-ADF 路径
# 然后进入 ESP-IDF 环境（二选一）：
#   方式 A：打开 "ESP-IDF 5.5 PowerShell" 快捷方式后再 cd 到固件目录
#   方式 B：C:\Espressif\frameworks\esp-idf-v5.5.5\export.ps1
```

### 5.3 关键配置确认（`sdkconfig.defaults` 已内置，一般无需改动）

| 配置 | 值 | 说明 |
|---|---|---|
| `CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI` | y | 板型；其他板型改此项 |
| `CONFIG_SUBSONICAPI_URL` / `CONFIG_SUBSONICAPI_PARA` | 空 | **保持为空**——地址由播放 URL 运行时推导 |
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | y | 16MB Flash |
| `CONFIG_SPIRAM`（OCT / 80M） | y | 8MB PSRAM |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 512 | 小分配走 PSRAM，保护内部 RAM（勿改回 2048，会引发会话期任务创建失败） |
| 语言 | `CONFIG_LANGUAGE_ZH_CN` | 屏幕中文 |

### 5.4 编译与烧录

```powershell
# 首次编译（生成 sdkconfig；之后增量编译可跳过 set-target）
idf.py set-target esp32s3
idf.py build

# 烧录（COM 口按实际改；烧录前关闭占用串口的监控程序！）
idf.py -p COM6 flash
```

### 5.5 一键脚本（可选，改路径后使用）

仓库根目录自带三个脚本，用编辑器把路径/端口改成你的环境后即可：

| 脚本 | 作用 | 需按需修改 |
|---|---|---|
| `build_mcpilot.bat` | 设置 IDF/ADF 环境并 `idf.py build` | IDF 安装路径、ADF_PATH、固件目录 |
| `flash_mcpilot.bat` | 设置环境并 `idf.py -p COM6 flash` | 同上 + 串口号 |
| `monitor_serial.py` | 串口日志抓取 | 脚本内 PORT/BAUD 常量 |

### 5.6 设备首次启动与配网

1. 烧录完成后自动重启，串口监控观察启动日志：

```powershell
python monitor_serial.py 60 serial_log.txt   # 抓 60 秒日志
```

2. 首次启动进入配网模式（按板型提示操作，bread-compact-wifi 为触摸按键进入 WiFi 配置），连接与服务端相同的局域网
3. 激活成功标志（串口日志）：
   - `WifiStation: Got IP: 192.168.x.x`
   - `Application: Activation done`
   - `StateMachine: State: activating -> idle`（屏幕显示"待命"）
   - `MCP: Add tool: self.music.play_url`（音乐工具已注册）

---

## 六、联调验证

对设备说以下指令，逐项验证（建议同时开串口监控）：

| 测试 | 说法 | 预期 |
|---|---|---|
| 基础点歌 | "你好小智，播放江南" | 状态栏"♪ 播放中"，出声，屏幕逐行歌词 |
| 拼音检索 | "播放 linjunjie 的歌" / "播放 ljj 的歌" | 匹配林俊杰曲目 |
| 错字容错 | "播放不再优豫" | 匹配《不再犹豫》 |
| 随机推荐 | "随便来几首 beyond 的歌" | 随机抽取匹配曲目 |
| 歌手点歌 | "我想听周杰伦" | 播放周杰伦歌曲 |
| 打断播放 | 播放中说"你好小智" | 音乐停止，进入新会话（既有设计） |
| 播放结束 | 等歌曲播完 | 显示"音乐播放完毕"，状态栏恢复"待命" |

**串口诊断日志解读**（播放中每 15 秒一条心跳）：

```
首塊PCM: len=2048 樣本=[-10 1 -10 1] ✓非零      ← 解码输出正常（全零=静音数据异常）
♪心跳: 已播104s 已讀18062KB 已寫I2S 4623872樣本(約104s@44100Hz) 歌詞23/61
        ↑播放时长      ↑PCM读取量      ↑I2S写入量与时长实时同步=输出正常    ↑歌词进度
播放統計: 讀取18062KB PCM, 寫入I2S 4623872樣本(約104s@44100Hz), 完整播放=是
```

---

## 七、常见问题排查

| 现象 | 原因 | 解决 |
|---|---|---|
| 编译报 `Failed to resolve component 'esp-adf-libs'` | ADF 子模块未初始化或 `ADF_PATH` 未设 | `git -C <adf> submodule update --init components/esp-adf-libs`；确认 `$env:ADF_PATH` |
| 编译报 build 目录异常（CMake 残留） | 增量编译状态损坏 | 删除 `build` 目录后重编 |
| 烧录失败 / 串口打不开 | 监控程序占用串口 | 关闭 monitor_serial.py / IDF Monitor 后重试 |
| 说"播放"后设备重启（串口见 `stack overflow in task PLAY_SCHED`） | 旧版本歌词查询在 2.5KB 栈上执行 | 本 Fork 已修复（查询移入 10KB STREAM_TASK）；若自行改动播放流程注意栈深 |
| 语音对话后音乐任务创建失败（`tcp_receive` / `STREAM_TASK 建立失敗`） | 会话期间内部 RAM 耗尽 | 本 Fork 已修复（调度任务 + SPIRAM 策略）；勿把 `SPIRAM_MALLOC_ALWAYSINTERNAL` 改回 2048 |
| 歌词/封面请求超时 | WiFi 低功耗导致响应延迟 | 本 Fork 已把 HTTP 超时放宽到 10s |
| 播放无声但状态显示正常 | 看"首塊PCM"与"心跳"日志：全零=解码静音；I2S 样本数不增长=输出链路问题；均正常仍无声→查音量（启动日志 `output volume`）与功放接线 | 按诊断日志三件套定位 |
| 云端说找不到歌 | 关键词与文件名不匹配 | 用更短关键词；确认曲库命名 `歌手 - 歌名.mp3`；支持拼音/错字重试 |
| 服务端 IP 变了 | — | **无需重新烧录**：music_service 每次调用自动探测局域网 IP，设备经播放 URL 自适应 |
| 封面图不显示（串口 `cover not found`） | 该歌曲目录无封面图 | 仅影响显示，不影响播放；后端按需扩展封面逻辑 |

---

## 八、目录速览与后续扩展

```
xiaozhi-esp32-music-player/
├── build_mcpilot.bat / flash_mcpilot.bat / monitor_serial.py   # 本 Fork 新增工具
├── sdkconfig.defaults / sdkconfig.defaults.esp32s3             # 构建预设（板型/内存策略）
├── main/
│   ├── boards/common/mp3_player.{h,cc}                         # 播放器核心（play_url/调度/诊断）
│   ├── boards/common/mp3_player_http.cc                        # Subsonic HTTP 层（URL 解析/歌词/封面）
│   ├── boards/bread-compact-wifi/compact_wifi_board.cc         # 板级集成
│   ├── audio/audio_codec.cc                                    # 音频编解码基类
│   └── assets/locales/*/language.json                          # 语言串（MUSIC_PLAYING）
└── 变更内容已并入本文档
```

可扩展方向（服务端 MCPilot 侧，不在本仓库）：
- 封面图支持（getCoverArt 返回真实图片）
- 播放列表/连播模式与 MCP 服务联动（`self.music.set_play_mode` 已在固件注册）
- 多设备/多曲库隔离

---

*本文档由实际联调过程整理生成：从固件适配、内存/栈问题修复、检索引擎升级到端到端语音播放验证全流程闭环。*
