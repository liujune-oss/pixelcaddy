# Pixel Caddy 项目技术文档

## 项目概述
高尔夫练习记分器，使用 ESP32-S3 + 16x16 WS2812 LED 矩阵 + 蓝牙连接。

## 硬件配置 (ESP32-S3)

| 功能 | GPIO | 备注 |
|------|------|------|
| LED 矩阵 | GPIO 8 | WS2812, 16x16 |
| 绿键 (Good) | GPIO 2 | 内部上拉 |
| 黄键 (Normal) | GPIO 4 | 内部上拉 |
| 红键 (Bad) | GPIO 3 | 内部上拉 |
| 蜂鸣器 | GPIO 1 | PWM 无源蜂鸣器 |
| 电池电压 | GPIO 5 | ADC, 1:1 分压 |

## 核心文件

### [main_s3.cpp](file:///f:/pixel%20caddy/main_s3.cpp)
主固件文件，约 1665 行。

**关键代码位置：**
| 功能 | 行号范围 | 说明 |
|------|----------|------|
| GPIO 定义 | 55-65 | `PIN_*` 常量 |
| 状态机枚举 | 345 | `GameState` |
| 设置变量 | 368-380 | `currentBrightness`, `currentVolume`, `settingsMode` |
| 数据存取 | 407-470 | `loadData()`, `saveData()` |
| 电池读取 | 480-520 | `readBatteryVoltage()`, `getBatteryPercent()` |
| UI 绘制 | 663-910 | `drawPlayingUI()`, `drawSettingsUI()` 等 |
| 按键处理 | 1410-1620 | `loop()` 中的按键逻辑 |
| 设置菜单处理 | 1533-1615 | `STATE_SETTINGS` 分支 |

### [AudioPlayer.h](file:///f:/pixel%20caddy/AudioPlayer.h)
音频播放类，约 220 行。

**关键代码位置：**
| 功能 | 行号 | 说明 |
|------|------|------|
| 音符定义 | 12-101 | `NOTE_*` 常量 |
| 旋律定义 | 109-122 | `MELODY_*` 数组 |
| 音量设置 | 145-151 | `setVolume()` - 映射到 0-1024 占空比 |
| 播放逻辑 | 175-201 | `update()` 非阻塞播放 |

### [index.html](file:///f:/pixel%20caddy/index.html)
网页仪表盘，约 570 行。

**关键代码位置：**
| 功能 | 行号范围 | 说明 |
|------|----------|------|
| BLE UUID | 185-190 | 服务和特征 UUID |
| 电池显示 | 137-141 | `#batteryStatus` HTML |
| 电池读取 | 355-370 | BLE 电池特征连接 |
| 手动读取 | 428-445 | `manualReadBattery()` |

## 状态机

```
STATE_PLAYING ─────┬──(10球)──> STATE_SUMMARY_GROUP ──> 继续
                   │
                   └──(长按红键3秒)──> STATE_SETTINGS ──(长按绿键3秒)──> STATE_PLAYING
                   │
                   └──(10组完成)──> STATE_SUMMARY_FINAL
```

## 设置菜单

### 进入/退出
- **长按红键 3 秒**：进入设置
- **长按绿键 3 秒**：保存并退出

### 操作
- **绿键短按 (<1秒)**：切换模式
- **黄键短按**：增加 +10
- **红键短按**：减少 -10

### 模式
- `settingsMode = 0`：亮度 (10-100)
- `settingsMode = 1`：音量 (0-100)

## BLE 特征

| UUID | 用途 |
|------|------|
| `5fafc201-...` | 服务 UUID |
| `beb5483e-...` | 电池电量 (READ/NOTIFY) |
| 历史/时间 | 见代码 92-97 行 |

## 电池校准

| 参数 | 值 |
|------|-----|
| 满电电压 | 4040 mV |
| 空电电压 | 3300 mV |
| 滤波 | 8次采样 + 5位 FIFO |

## 常见修改位置

| 需求 | 文件 | 位置 |
|------|------|------|
| 添加新设置项 | main_s3.cpp | `SETTINGS_MODE_COUNT`, `drawSettingsUI()`, 按键处理 |
| 调整电池阈值 | main_s3.cpp | `getBatteryPercent()` 约 510 行 |
| 修改音量范围 | AudioPlayer.h | `setVolume()` 约 145 行 |
| 添加新音效 | AudioPlayer.h | `MELODY_*` 和 `play*()` 函数 |
| 修改按键逻辑 | main_s3.cpp | `loop()` 约 1400-1620 行 |
