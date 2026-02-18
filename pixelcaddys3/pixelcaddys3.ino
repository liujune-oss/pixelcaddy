/*
 * Pixel Caddy - Mark 4 (Pro Analytics Edition) - ESP32-S3 Version
 * --------------------------------------------
 * 核心功能:
 * 1. 📊 [新增] 结算系统:
 * - 每组(10球)结束后进入小组结算，循环显示本组 G/N/B 数量。
 * - 任意按键退出小组结算，开始下一组。
 * - 第10组结束后，先看小组结算，再按键进入全场总成绩结算。
 * - 全场结算循环显示: 好球数(%) -> 普通数(%) -> 坏球数(%)。
 * 2. 💤 [保留] 屏幕保护: 10分钟无操作熄屏，按键唤醒。
 * 3. 💾 [升级] 数据结构: 完整记录 Total/Good/Normal/Bad 以支持统计。
 * --------------------------------------------
 */

#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#define FIRMWARE_VERSION "v2.6.0" // [新增] 便于修改固件版本

#include "AudioPlayer.h" // [NEW] Advanced Audio
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <Fonts/TomThumb.h>
#include <HIDKeyboardTypes.h>
#include <HIDTypes.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

const int MAX_HISTORY_SIZE = 100;

struct GroupRecord {
  uint32_t timestamp;    // Unix Timestamp
  uint32_t recordMillis; // [新增] 相对时间回溯
  uint16_t good;
  uint16_t normal;
  uint16_t bad;
};

GroupRecord allGroupsHistory[MAX_HISTORY_SIZE];
int historyCount = 0;            // 当前存了多少条
int historyHead = 0;             // 下一条写入的位置 (Ring Buffer)
volatile int requestedPage = -1; // [新增] 请求的页码 (-1 表示无请求)

#include "secrets.h"

// ================= 1. 用户配置 =================
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

// ================= 2. 硬件引脚 (ESP32-S3) =================
#define PIN_MATRIX 8     // D9 (GPIO 8)
#define PIN_BTN_GOOD 2   // D1 (GPIO 2)
#define PIN_BTN_BAD 3    // D2 (GPIO 3)
#define PIN_BTN_NORMAL 4 // D3 (GPIO 4)
#define PIN_BUZZER 1     // D0 (GPIO 1)
#define PIN_BATTERY 5    // D4 (GPIO 5) - 电池电压 ADC

// ================= 3. 屏幕与颜色 =================
Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
    16, 16, PIN_MATRIX,
    NEO_MATRIX_TOP + NEO_MATRIX_RIGHT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG,
    NEO_GRB + NEO_KHZ800);

const uint16_t C_GREEN = matrix.Color(0, 150, 0);
const uint16_t C_RED = matrix.Color(150, 0, 0);
const uint16_t C_YELLOW = matrix.Color(200, 150, 0);
const uint16_t C_BLUE = matrix.Color(0, 100, 255);
const uint16_t C_WHITE = matrix.Color(120, 120, 120);
const uint16_t C_DIM = matrix.Color(5, 5, 5);

// ================= 4. 全局变量与状态机 =================
Preferences prefs;
AsyncWebServer server(80);
bool isOTAMode = false;
String ipSuffix = "";
bool isScreenSaver = false;

// ================= [S3 DUAL-CORE] LED 刷新任务 =================
TaskHandle_t ledTaskHandle = NULL;
SemaphoreHandle_t displayMutex = NULL;
volatile bool displayNeedsUpdate = false;

// ================= BLE Objects & Logic =================
#define SERVICE_UUID "5fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_HISTORY_UUID                                                      \
  "beb5483e-36e1-4688-b7f5-ea07361b26ac" // Changed to 'ac' to bust cache again
#define CHAR_TIME_UUID "87a7d400-5343-4565-a9b7-1601b0034876"
#define CHAR_BATTERY_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a1" // 电池电量
#define CHAR_VERSION_UUID                                                      \
  "beb5483e-36e1-4688-b7f5-ea07361b26a2" // [New] Firmware Version
#define CHAR_CONFIG_UUID                                                       \
  "beb5483e-36e1-4688-b7f5-ea07361b26a3" // [New] Settings (Brightness/Volume)

BLEServer *pServer = NULL;
BLECharacteristic *pHistoryCharacteristic = NULL;
BLECharacteristic *pTimeCharacteristic = NULL;
BLECharacteristic *pBatteryCharacteristic = NULL; // [新增] 电池特征值
BLECharacteristic *pVersionCharacteristic =
    NULL; // [New] Firmware Version Characteristic
BLECharacteristic *pConfigCharacteristic = NULL; // [New] Config (Read/Write)
int deviceConnectedCount = 0; // [Fix] Counter instead of bool
bool isBleEnabled = true;
bool oldDeviceConnected = false;
bool wipeRequested = false; // [New] Flag for remote wipe
bool isTimeSynced = false;  // [New] Flag for time sync status
volatile bool uiRefreshRequested =
    false; // [New] Flag to trigger UI update from callbacks
volatile bool advertisingRestartRequested =
    false; // [New] Safer Advertising Restart

// ================= 电池电量监测 (变量提前声明) =================
unsigned long lastBatteryUpdate = 0;
const unsigned long BATTERY_UPDATE_INTERVAL = 30000; // 30秒更新一次
int lastBatteryPercent = -1;

// ================= Audio Object =================
AudioPlayer audio(PIN_BUZZER);

// [Camera Remote Globals]
BLEHIDDevice *pHidDevice;
BLECharacteristic *inputKeyboard;
BLECharacteristic *inputConsumer;
bool isAutoRecordEnabled = false;
unsigned long camSequenceStartTime = 0; // 0 = Inactive
const int CAM_PRE_DELAY = 3000;         // 3s Prepare
const int CAM_REC_DURATION = 9000;      // 9s Recording
bool hasSentStart = false;
bool hasSentStop = false;

// HID Key Definitions (Bitmask for Report ID 2)
const uint8_t hid_volume_up = 0x01; // Bit 0 = Usage 0xE9
const uint8_t hid_volume_release = 0x00;

// ================= Settings Variables (Moved for Scope) =================
int currentBrightness = 20;
int currentVolume = 30; // [新增] 音量设置 (0-100)
const int SETTING_STEP = 10;
const int BRT_MAX = 100;
const int BRT_MIN = 10;
const int VOL_MAX = 100;
const int VOL_MIN = 0;

// [新增] 设置菜单状态
int settingsMode = 0; // 0=亮度, 1=音量
const int SETTINGS_MODE_COUNT = 2;

// [NEW] Diagnostic HID Helper (Keyboard ID 1)
void sendHIDKey(uint8_t keycode) {
  if (deviceConnectedCount > 0 && inputKeyboard != NULL) {
    uint8_t buffer[8] = {0, 0, keycode, 0, 0, 0, 0, 0};
    inputKeyboard->setValue(buffer, 8);
    inputKeyboard->notify();

    delay(200); // [Fix] Increase from 20ms to 200ms for better compatibility
    uint8_t release[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    inputKeyboard->setValue(release, 8);
    inputKeyboard->notify();

    // [OPTIMIZED] Removed redundant matrix.show()
    matrix.drawPixel(15, 0, C_GREEN);
    Serial.printf("HID KB: Sent Keycode 0x%02X\n", keycode);
  }
}

// [NEW] Consumer Control Helper (ID 2)
void sendConsumerKey(uint8_t mask) {
  if (deviceConnectedCount > 0 && inputConsumer != NULL) {
    inputConsumer->setValue(&mask, 1);
    inputConsumer->notify();

    // [OPTIMIZED] Remove redundant matrix.show() to save power and prevent
    // brownout Visual indicator will be drawn; next main loop iteration will
    // show it.
    matrix.drawPixel(15, 0, C_GREEN);
    Serial.printf("HID CONS: Sent Mask 0x%02X\n", mask);
  } else {
    Serial.println("HID Error: inputConsumer NULL or No Connection");
  }
}

// Forward Declarations
void saveData();
void saveBrightness(); // [Fix] Forward declaration
void sendHistoryPage(int page);
// void playSound(int type);    // [DEPRECATED] Old blocking sound
void requestDisplayUpdate(); // [S3 DUAL-CORE] Request display refresh

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnectedCount++;

    // [HACK] Force Enable Notifications
    // Windows sometimes fails to write the Descriptor, so we do it for them.
    // This tells the library "Yes, someone subscribed, please allow notify()"
    BLEDescriptor *pDesc =
        pHistoryCharacteristic->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
    if (pDesc) {
      uint8_t on[] = {0x01, 0x00};
      pDesc->setValue(on, 2);
      Serial.println("Forced History Notifications ON");
    }

    // [新增] 为电池特征也强制启用通知
    BLEDescriptor *pBattDesc =
        pBatteryCharacteristic->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
    if (pBattDesc) {
      uint8_t on[] = {0x01, 0x00};
      pBattDesc->setValue(on, 2);
      Serial.println("Forced Battery Notifications ON");
    }

    // [新增] 强制立即更新电池 (重置缓存以确保首次发送)
    lastBatteryPercent = -1;
    lastBatteryUpdate = 0; // 触发立即更新

    Serial.print("Device Connected. Count: ");
    Serial.println(deviceConnectedCount);
    uiRefreshRequested = true;          // Refresh LED
    advertisingRestartRequested = true; // Request advertising restart safely
  }
  void onDisconnect(BLEServer *pServer) {
    deviceConnectedCount--;
    if (deviceConnectedCount < 0)
      deviceConnectedCount = 0;

    // Request restart in main loop
    advertisingRestartRequested = true;

    Serial.print("Device Disconnected. Count: ");
    Serial.println(deviceConnectedCount);
    // Reset time sync status on disconnect if desired,
    // but usually we keep time if it was set.
    // Ideally we might want to show "Connected" status lost.
    uiRefreshRequested = true;
  }
};

class HistoryCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();
    if (value.length() > 0) {
      // [New] Check for WIPE command
      if (value == "WIPE") {
        Serial.println("Command Received: WIPE ALL DATA");
        wipeRequested = true;
        return;
      }

      int page = value.toInt();
      Serial.print("Queueing Page Request: ");
      Serial.println(page);
      requestedPage = page;
    }
  }
};

class TimeCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    // Fix: cast to String correctly
    String value = pCharacteristic->getValue().c_str();

    // [New] OTA Trigger Command
    if (value == "OTAREQ") {
      Serial.println("OTA Triggered via BLE");
      isOTAMode = true;
      setupOTA();
      return;
    }

    if (value.length() > 0) {
      long t = value.toInt(); // e.g. "1700000000"
      if (t > 1600000000) {
        struct timeval now = {.tv_sec = (time_t)t, .tv_usec = 0};
        settimeofday(&now, NULL);
        isTimeSynced = true; // [New] Mark time as synced
        Serial.println("Time synced via BLE");
        uiRefreshRequested = true; // Refresh LED

        // [新增] 相对时间回溯修复

        // bootTime = CurrentSyncedTime - CurrentMillis
        uint32_t bootTime = t - (millis() / 1000);
        bool needsSave = false;

        for (int i = 0; i < historyCount; i++) {
          // 如果时间戳看起来很小 (比如是 1970年 + 几小时)，说明是未同步时记录的
          if (allGroupsHistory[i].timestamp < 1600000000) {
            // 修复时间 = 开机时刻 + 记录时的开机时长
            allGroupsHistory[i].timestamp =
                bootTime + (allGroupsHistory[i].recordMillis / 1000);
            needsSave = true;
          }
        }

        if (needsSave) {
          saveData();
        }
        // [Stream Protocol] Auto-blast removed specific to page request
      }
    }
  }
};

// Moved here for ConfigCallbacks access
enum GameState {
  STATE_PLAYING,
  STATE_SUMMARY_GROUP,
  STATE_SUMMARY_FINAL,
  STATE_SETTINGS
};
GameState currentState = STATE_PLAYING;
void drawPlayingUI(); // Forward declaration

class ConfigCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();
    if (value.length() > 0) {
      char type = value.charAt(0);
      int val = value.substring(2).toInt();

      if (type == 'B') {
        // Brightness (5-100)
        if (val < 5)
          val = 5;
        if (val > 100)
          val = 100;
        currentBrightness = val;
        matrix.setBrightness(currentBrightness);
        saveBrightness();
        requestDisplayUpdate(); // [Fix] Immediate refresh
        Serial.printf("BLE Set Brightness: %d\n", val);
      } else if (type == 'V') {
        // Volume (0-100)
        if (val < 0)
          val = 0;
        if (val > 100)
          val = 100;
        currentVolume = val;
        audio.setVolume(currentVolume);

        // Save volume directly to prefs
        prefs.begin("pixelcaddy", false);
        prefs.putInt("vol", currentVolume);
        prefs.end();

        audio.playBeep(); // [New] Feedback beep
        Serial.printf("BLE Set Volume: %d\n", val);
      } else if (type == 'M') {
        // Mode switch: equivalent to Green+Normal combo key
        char mode = value.charAt(2);
        if (mode == 'C') {
          isAutoRecordEnabled = true;
        } else if (mode == 'N') {
          isAutoRecordEnabled = false;
        }
        // Show CAM ON/OFF on LED (same as combo key)
        xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY);
        matrix.fillScreen(0);
        matrix.setTextColor(isAutoRecordEnabled ? C_GREEN : C_RED);
        matrix.setCursor(2, 6);
        matrix.print("CAM");
        matrix.setCursor(2, 13);
        matrix.print(isAutoRecordEnabled ? "ON" : "OFF");
        if (isAutoRecordEnabled)
          matrix.drawPixel(0, 0, matrix.Color(0, 50, 50));
        matrix.show();
        xSemaphoreGiveRecursive(displayMutex);
        playSound(isAutoRecordEnabled ? 5 : 2);
        Serial.printf("BLE Mode: CAM %s\n", isAutoRecordEnabled ? "ON" : "OFF");
        // Show for 1 second then redraw
        delay(1000);
        if (currentState == STATE_PLAYING)
          drawPlayingUI();
      }
    }
  }

  void onRead(BLECharacteristic *pCharacteristic) {
    // Return config: B=brightness, V=volume, D=connected devices, C=camera mode
    String configStr = "B:" + String(currentBrightness) +
                       ",V:" + String(currentVolume) +
                       ",D:" + String(deviceConnectedCount) +
                       ",C:" + String(isAutoRecordEnabled ? 1 : 0);
    pCharacteristic->setValue(configStr.c_str());
    Serial.printf("BLE Read Config: %s\n", configStr.c_str());
  }
};

void sendRecord(int index, GroupRecord &r) {
  if (deviceConnectedCount == 0)
    return; // Re-enabled check (optional, but good practice if bypass isn't
            // needed)

  String payload = "{\"i\":" + String(index) + ",\"g\":" + String(r.good) +
                   ",\"n\":" + String(r.normal) + ",\"b\":" + String(r.bad) +
                   ",\"ts\":" + String(r.timestamp) + "}";

  // Serial.print("TX:"); Serial.println(payload); // Optional Log

  pHistoryCharacteristic->setValue((uint8_t *)payload.c_str(),
                                   payload.length());
  pHistoryCharacteristic->notify();
}

// New Paged Sender
void sendHistoryPage(int page) {
  Serial.print("Sending Page ");
  Serial.println(page);
  playSound(4); // [DEBUG] Success Beep

  if (historyCount == 0) {
    Serial.println("No history to send.");
    return;
  }

  // Page 0 = Latest 10 records
  // Page 1 = Previous 10 records etc.
  int recordsPerPage = 10;
  int startOffset = page * recordsPerPage;

  if (startOffset >= historyCount) {
    Serial.println("Page out of range.");
    return;
  }

  int endOffset = startOffset + recordsPerPage;
  if (endOffset > historyCount)
    endOffset = historyCount;

  // Ring Buffer Traversal (Backwards from Head-1)
  // i=0 is newest, i=historyCount-1 is oldest
  for (int i = startOffset; i < endOffset; i++) {
    // logical index i maps to physical index in ring buffer
    int pIdx = (historyHead - 1 - i + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;

    // Send record. Use (historyCount - i) as the stable ID (#1 = first game)
    sendRecord(historyCount - i, allGroupsHistory[pIdx]);
    delay(50); // Small delay between packets
  }
}

void updateHistoryBLE() {
  // Only send the latest record
  if (deviceConnectedCount == 0)
    return;
  int latestIdx = (historyHead - 1 + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
  sendRecord(latestIdx + 1, allGroupsHistory[latestIdx]);
}

// GameState moved above ConfigCallbacks

// 结算显示控制
unsigned long summaryTimer = 0;
int summaryPage = 0;               // 0:Good, 1:Normal, 2:Bad
const int SUMMARY_INTERVAL = 2000; // 2秒切换

// 数据 (Mark 4: 增加 Normal/Bad 持久化)
int totalShots = 0;
int totalGood = 0;
int totalNormal = 0; // [新增]
int totalBad = 0;    // [新增]

// 小组临时数据
int groupShots = 0;
int groupGoodCount = 0;
int groupNormalCount = 0;
int groupBadCount = 0; // [新增] 方便统计
uint8_t groupHistory[10];
int currentGroupIdx = 0;
int groupResults[10];

// 长按与防抖
unsigned long lastActivityTime = 0;
const unsigned long SLEEP_TIMEOUT = 10 * 60 * 1000;
unsigned long pressTimeGood = 0;
unsigned long pressTimeNormal = 0;
unsigned long pressTimeBad = 0;
bool longPressHandledGood = false;
bool longPressHandledNormal = false;
bool longPressHandledBad = false;
const int LONG_PRESS_DURATION = 2000; // 改为2秒触发
const int BRT_ADJUST_INTERVAL = 1000; // 每1秒调整一次
unsigned long lastBrtAdjustTime = 0;  // 上次调整亮度的时间
int lastStateGood = HIGH;
int lastStateNormal = HIGH;
int lastStateBad = HIGH;
unsigned long lastTriggerTime = 0;
const int DEBOUNCE_LOCKOUT = 80;
unsigned long lastScoreTime = 0;
const int SCORE_COOLDOWN = 1000;

// ================= 5. 数据存取 =================
void loadData() {
  prefs.begin("pixelcaddy", false);
  totalShots = prefs.getInt("total", 0);
  totalGood = prefs.getInt("good", 0);
  totalNormal = prefs.getInt("normal", 0);
  totalBad = prefs.getInt("bad", 0);
  currentBrightness = prefs.getInt("brt", 20);
  currentVolume = prefs.getInt("vol", 30); // [新增] 读取音量
  currentGroupIdx = prefs.getInt("groupIdx", 0);
  groupShots = prefs.getInt("groupShots", 0);
  groupGoodCount = prefs.getInt("groupGood", 0);
  groupNormalCount = prefs.getInt("groupNormal", 0);
  groupBadCount = prefs.getInt("groupBad", 0);
  currentState = (GameState)prefs.getInt("state", STATE_PLAYING);
  // [修复] 防止开机进入设置状态
  if (currentState == STATE_SETTINGS) {
    currentState = STATE_PLAYING;
  }

  // Load UI arrays
  prefs.getBytes("ui_hist", groupHistory, sizeof(groupHistory));
  prefs.getBytes("ui_res", groupResults, sizeof(groupResults));

  // Load Ring Buffer History
  historyCount = prefs.getInt("h_cnt", 0);
  historyHead = prefs.getInt("h_head", 0);
  prefs.getBytes("all_hist", allGroupsHistory, sizeof(allGroupsHistory));

  prefs.end();

  // [新增] 应用加载的音量设置
  audio.setVolume(currentVolume);
}

void saveData() {
  prefs.begin("pixelcaddy", false);
  prefs.putInt("total", totalShots);
  prefs.putInt("good", totalGood);
  prefs.putInt("normal", totalNormal);
  prefs.putInt("bad", totalBad);
  prefs.putInt("brt", currentBrightness); // [新增] 保存亮度
  prefs.putInt("vol", currentVolume);     // [新增] 保存音量
  prefs.putInt("groupIdx", currentGroupIdx);
  prefs.putInt("groupShots", groupShots);
  prefs.putInt("groupGood", groupGoodCount);
  prefs.putInt("groupNormal", groupNormalCount);
  prefs.putInt("groupBad", groupBadCount);
  prefs.putInt("state", (int)currentState);

  // Save UI arrays
  prefs.putBytes("ui_hist", groupHistory, sizeof(groupHistory));
  prefs.putBytes("ui_res", groupResults, sizeof(groupResults));

  // Save Ring Buffer History
  prefs.putInt("h_cnt", historyCount);
  prefs.putInt("h_head", historyHead);
  prefs.putBytes("all_hist", allGroupsHistory, sizeof(allGroupsHistory));

  prefs.end();
}

void saveBrightness() {
  prefs.begin("pixelcaddy", false);
  prefs.putInt("brt", currentBrightness);
  prefs.end();
}

void clearData() {
  prefs.begin("pixelcaddy", false);
  prefs.clear();
  prefs.end();
}

// ================= 电池电量监测 (函数) =================

// 读取电池电压 (单位: mV) - 带滤波
int readBatteryVoltage() {
  // ESP32-S3 ADC: 12-bit (0-4095), 参考电压约 3.3V
  // 电压分压 1:1，实际电压 = ADC电压 × 2

  // [滤波] 采样 8 次取平均值
  long sum = 0;
  const int samples = 8;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(PIN_BATTERY);
    delayMicroseconds(500); // 0.5ms 间隔
  }
  int adcValue = sum / samples;

  // ADC电压 = adcValue / 4095 * 3300 mV
  // 电池电压 = ADC电压 * 2 (因为 1:1 分压)
  int batteryMV = (adcValue * 3300 * 2) / 4095;
  return batteryMV;
}

// 转换电压为百分比 - 5 位滑动窗口滤波
// [校准] 实际满电约 4040mV，空电约 3300mV
int mvHistory[5] = {0, 0, 0, 0, 0}; // 5 位 FIFO 队列
int mvHistoryIndex = 0;
bool mvHistoryFilled = false;

int getBatteryPercent() {
  int mv = readBatteryVoltage();

  // [FIFO] 先入先出存储电压值
  mvHistory[mvHistoryIndex] = mv;
  mvHistoryIndex = (mvHistoryIndex + 1) % 5;
  if (mvHistoryIndex == 0)
    mvHistoryFilled = true;

  // 计算平均值
  int count = mvHistoryFilled ? 5 : mvHistoryIndex;
  if (count == 0)
    count = 1; // 防止除零
  long sum = 0;
  for (int i = 0; i < count; i++) {
    sum += mvHistory[i];
  }
  int avgMv = sum / count;

  // [调试] 输出原始电压值
  static unsigned long lastDebugTime = 0;
  if (millis() - lastDebugTime > 10000) { // 每10秒输出一次
    lastDebugTime = millis();
    Serial.printf("[BATT DEBUG] Raw mV: %d, Avg mV: %d\n", mv, avgMv);
  }

  // [校准] 调整满电阈值为 4040mV (实测满电 4043mV)
  int percent;
  if (avgMv >= 4040)
    percent = 100;
  else if (avgMv <= 3300)
    percent = 0;
  else
    percent = (avgMv - 3300) * 100 / 740; // 740 = 4040 - 3300

  return percent;
}

// 更新电池 BLE 特征值
void updateBatteryBLE() {
  if (deviceConnectedCount == 0 || pBatteryCharacteristic == NULL)
    return;

  int percent = getBatteryPercent();
  if (percent == lastBatteryPercent)
    return; // 未变化则不发送

  lastBatteryPercent = percent;
  String payload = String(percent);
  pBatteryCharacteristic->setValue((uint8_t *)payload.c_str(),
                                   payload.length());
  pBatteryCharacteristic->notify();
  Serial.printf("Battery: %d%%\n", percent);
}

// ================= 6. 辅助功能 (Audio Wrapper) =================
// Replaces old blocking playSound with non-blocking calls
void playSound(int type) {
  if (type == 4) {        // Group Complete / Success
    audio.playMario();    // [UPGRADE] Mario Theme!
  } else if (type == 5) { // Victory / 1-UP
    audio.play1UP();
  } else if (type == 1) { // Good
    audio.playBeep();
  } else if (type == 3) { // Bad
    audio.playBad();
  } else { // Normal / Default
    audio.playBeep();
  }
}

int calculateGroupColorType(int good, int normal) {
  int score = (good * 2) + (normal * 1);
  if (score >= 16)
    return 3;
  if (score >= 9)
    return 2;
  return 1;
}

uint16_t getColorFromType(int type) {
  switch (type) {
  case 1:
    return C_RED;
  case 2:
    return C_YELLOW;
  case 3:
    return C_GREEN;
  default:
    return C_DIM;
  }
}

// ================= [S3 DUAL-CORE] LED 刷新任务与辅助函数 =================

// [新增] 软件功耗限制 (Software Current Limiter)
// 如果缓冲区总亮度对应的电流超过限制 (如 2000mA)，自动按比例降低所有像素亮度
void enforcePowerLimit() {
  uint8_t *pixels = matrix.getPixels();
  uint32_t totalSum = 0;
  uint16_t numBytes = 16 * 16 * 3; // 256 pixels * 3 colors

  // 1. 统计当前缓冲区的所有亮度值
  for (uint16_t i = 0; i < numBytes; i++) {
    totalSum += pixels[i];
  }

  // 2. 估算电流 (mA)
  // 假设全白 (765) = 60mA -> 1个单位值 ≈ 0.0784mA
  // 加上 ESP32 基础功耗约 100mA
  float estimatedCurrent = (totalSum * 0.0784) + 100;

  const float MAX_CURRENT_MA = 2000.0; // 限制在 2000mA (安全值)

  // 3. 如果超标，计算缩放比例并应用
  if (estimatedCurrent > MAX_CURRENT_MA) {
    float scale = MAX_CURRENT_MA / estimatedCurrent;
    // Serial.printf("[PWR] Limit triggered! Est: %.0fmA, Scale: %.2f\n",
    // estimatedCurrent, scale);
    for (uint16_t i = 0; i < numBytes; i++) {
      pixels[i] = (uint8_t)(pixels[i] * scale);
    }
  }
}

// LED 刷新任务 (运行在 Core 0，专门负责 matrix.show())
void ledRefreshTask(void *parameter) {
  Serial.println("[LED Task] Started on Core 0");

  // [Fix] Initialize matrix on the SAME CORE that calls show()
  matrix.begin();
  delay(100);

  while (true) {
    // 检查是否需要刷新
    if (displayNeedsUpdate) {
      // 获取互斥锁 [Recursive]
      if (xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY) == pdTRUE) {
        // 执行刷新
        matrix.show();
        displayNeedsUpdate = false;

        // 释放互斥锁 [Recursive]
        xSemaphoreGiveRecursive(displayMutex);
      }
    }

    // 短暂延时，避免 CPU 占用过高
    vTaskDelay(pdMS_TO_TICKS(5)); // 5ms 检查一次
  }
}

// 请求显示刷新的安全接口
void requestDisplayUpdate() {
  if (xSemaphoreTakeRecursive(displayMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    displayNeedsUpdate = true;
    xSemaphoreGiveRecursive(displayMutex);
  }
}

// ================= 7. UI 绘制逻辑 (分状态) =================

// 7.1 游戏进行中界面
void drawPlayingUI() {
  xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); // [Lock]

  matrix.fillScreen(0);
  matrix.drawPixel(2, 0, C_WHITE);
  for (int i = 0; i < 10; i++) {
    int x = 3 + i;
    uint16_t color = C_DIM;
    if (i < currentGroupIdx)
      color = getColorFromType(groupResults[i]);
    else if (i == currentGroupIdx && currentGroupIdx < 10)
      color = C_BLUE;
    matrix.drawPixel(x, 0, color);
  }
  matrix.drawPixel(13, 0, C_WHITE);

  matrix.setTextColor(C_GREEN);
  int goodX = (totalGood < 10) ? 4 : 1;
  matrix.setCursor(goodX, 7);
  matrix.print(totalGood);
  matrix.setTextColor(C_BLUE);
  int shotX = (totalShots < 10) ? 12 : 9;
  matrix.setCursor(shotX, 7);
  matrix.print(totalShots);

  int score = 0;
  if (totalShots > 0) {
    score = (totalGood * 100 + totalNormal * 50) / totalShots;
  }

  matrix.setTextColor(C_WHITE);
  if (score == 100) {
    matrix.setCursor(3, 13);
    matrix.print(100);
  } else {
    int scoreX = (score >= 10) ? 4 : 7;
    matrix.setCursor(scoreX, 13);
    matrix.print(score);
  }

  if (isAutoRecordEnabled) {
    matrix.drawPixel(0, 0, matrix.Color(0, 50, 50)); // Cyan (Dim)
  }

  for (int i = 0; i < 10; i++) {
    int x = 3 + i;
    uint16_t color = C_DIM;
    if (i < groupShots) {
      if (groupHistory[i] == 1)
        color = C_GREEN;
      else if (groupHistory[i] == 2)
        color = C_YELLOW;
      else if (groupHistory[i] == 3)
        color = C_RED;
    }
    matrix.drawPixel(x, 14, color);
  }
  matrix.drawPixel(2, 14, C_WHITE);
  matrix.drawPixel(13, 14, C_WHITE);

  if (isAutoRecordEnabled) {
    long elapsed = 0;
    if (camSequenceStartTime > 0) {
      elapsed = millis() - camSequenceStartTime;
    }
    int firstPixelOn = 0;
    if (camSequenceStartTime > 0) {
      firstPixelOn = elapsed / 1000;
      if (firstPixelOn > 12)
        firstPixelOn = 12;
    }
    for (int i = 0; i < 12; i++) {
      int x = 2 + i;
      if (i >= firstPixelOn) {
        if (i < 3)
          matrix.drawPixel(x, 15, C_YELLOW);
        else
          matrix.drawPixel(x, 15, C_GREEN);
      } else {
        matrix.drawPixel(x, 15, 0); // Off
      }
    }
  } else {
    // [新增] 电量显示条：中间10个LED (列3-12)，每个代表10%
    int batteryPct = getBatteryPercent();
    int litLeds = batteryPct / 10; // 0-10 个灯亮
    for (int i = 0; i < 10; i++) {
      int x = 3 + i; // 列 3-12
      if (i < litLeds) {
        matrix.drawPixel(x, 15, C_GREEN); // 有电 = 绿色
      } else {
        matrix.drawPixel(x, 15, C_RED); // 没电 = 红色
      }
    }
    // 两侧留空 (列 0-2 和 13-15)
    for (int i = 0; i < 3; i++) {
      matrix.drawPixel(i, 15, 0);
      matrix.drawPixel(13 + i, 15, 0);
    }
  }

  if (isBleEnabled) {
    uint32_t statusColor = matrix.Color(0, 0, 50);
    if (deviceConnectedCount > 0) {
      if (isTimeSynced) {
        statusColor = C_BLUE;
      } else {
        statusColor = matrix.Color(0, 0, 150);
      }
    }
    matrix.drawPixel(15, 0, statusColor);
  }
  enforcePowerLimit(); // [新增] 强制检查功耗
  requestDisplayUpdate();
  xSemaphoreGiveRecursive(displayMutex); // [Unlock]
}

// 7.2 小组结算界面
void drawGroupSummary() {
  xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); // [Lock]

  matrix.fillScreen(0);
  matrix.setTextColor(C_GREEN);
  int goodX = (groupGoodCount < 10) ? 6 : 3;
  matrix.setCursor(goodX, 6);
  matrix.print(groupGoodCount);

  matrix.setTextColor(C_YELLOW);
  int normX = (groupNormalCount < 10) ? 2 : 0;
  matrix.setCursor(normX, 13);
  matrix.print(groupNormalCount);

  matrix.setTextColor(C_RED);
  int badX = (groupBadCount < 10) ? 10 : 8;
  matrix.setCursor(badX, 13);
  matrix.print(groupBadCount);

  enforcePowerLimit(); // [新增] 强制检查功耗
  requestDisplayUpdate();

  xSemaphoreGiveRecursive(displayMutex); // [Unlock]
}

// 7.3 全场结算界面
void drawFinalSummary() {
  xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); // [Lock]

  matrix.fillScreen(0);
  int count = 0;
  float percent = 0.0;
  uint16_t color = C_WHITE;

  if (summaryPage == 0) {
    count = totalGood;
    if (totalShots > 0)
      percent = (float)totalGood / totalShots * 100.0;
    color = C_GREEN;
  } else if (summaryPage == 1) {
    count = totalNormal;
    if (totalShots > 0)
      percent = (float)totalNormal / totalShots * 100.0;
    color = C_YELLOW;
  } else {
    count = totalBad;
    if (totalShots > 0)
      percent = (float)totalBad / totalShots * 100.0;
    color = C_RED;
  }

  matrix.setTextColor(color);
  int xNum = (count >= 100) ? 1 : ((count >= 10) ? 4 : 7);
  matrix.setCursor(xNum, 6);
  matrix.print(count);

  matrix.setTextColor(C_WHITE);
  int xPer = 0;
  matrix.setCursor(xPer, 14);
  if (percent == 100.0) {
    matrix.print("100");
  } else {
    matrix.print(percent, 1);
  }

  enforcePowerLimit(); // [新增] 强制检查功耗
  requestDisplayUpdate();
  xSemaphoreGiveRecursive(displayMutex); // [Unlock]
}

// 7.4 设置菜单界面
void drawSettingsUI() {
  xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); // [Lock]
  matrix.fillScreen(0);

  int currentValue = 0;
  uint16_t iconColor = C_YELLOW;

  if (settingsMode == 0) {
    // 亮度模式：显示太阳图标
    currentValue = currentBrightness;
    iconColor = C_YELLOW;
    // 太阳图标 (6x6, 从 (5,1) 开始)
    matrix.drawPixel(7, 1, iconColor);  // 顶
    matrix.drawPixel(7, 7, iconColor);  // 底
    matrix.drawPixel(4, 4, iconColor);  // 左
    matrix.drawPixel(10, 4, iconColor); // 右
    matrix.drawPixel(5, 2, iconColor);  // 左上
    matrix.drawPixel(9, 2, iconColor);  // 右上
    matrix.drawPixel(5, 6, iconColor);  // 左下
    matrix.drawPixel(9, 6, iconColor);  // 右下
    // 中心圆
    matrix.fillCircle(7, 4, 2, iconColor);
  } else {
    // 音量模式：显示喇叭图标
    currentValue = currentVolume;
    iconColor = C_BLUE;
    // 喇叭图标 (从 (4,2) 开始)
    matrix.drawPixel(5, 4, iconColor);      // 喇叭尖
    matrix.fillRect(6, 3, 2, 3, iconColor); // 喇叭身
    matrix.drawLine(8, 2, 8, 6, iconColor); // 喇叭口
    // 声波
    matrix.drawPixel(10, 3, iconColor);
    matrix.drawPixel(10, 5, iconColor);
    matrix.drawPixel(11, 4, iconColor);
  }

  // 显示数值 (行 9-13)
  matrix.setTextColor(C_WHITE);
  matrix.setCursor(2, 13);
  char buf[4];
  sprintf(buf, "%3d", currentValue);
  matrix.print(buf);

  // 底部进度条 (列 3-12，共10格)
  int litLeds = currentValue / 10;
  for (int i = 0; i < 10; i++) {
    int x = 3 + i;
    if (i < litLeds) {
      matrix.drawPixel(x, 15, C_GREEN);
    } else {
      matrix.drawPixel(x, 15, C_RED);
    }
  }

  enforcePowerLimit();
  requestDisplayUpdate();
  xSemaphoreGiveRecursive(displayMutex); // [Unlock]
}

void animateSurge(uint16_t c) {
  xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); // [Lock]
  drawPlayingUI();
  matrix.drawRect(1, 1, 14, 14, c);

  enforcePowerLimit(); // [新增] 强制检查功耗
  requestDisplayUpdate();
  xSemaphoreGiveRecursive(displayMutex); // [Unlock]

  delay(80);

  xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); // [Lock]
  drawPlayingUI();
  matrix.drawRect(0, 0, 16, 16, c);

  enforcePowerLimit(); // [新增] 强制检查功耗
  requestDisplayUpdate();
  xSemaphoreGiveRecursive(displayMutex); // [Unlock]

  delay(150);
}

void checkGroupCompletion() {
  if (groupShots >= 10) {
    delay(200);
    int colorType = calculateGroupColorType(groupGoodCount, groupNormalCount);
    if (currentGroupIdx < 10) {
      groupResults[currentGroupIdx] = colorType;
      int writeIdx = historyHead;
      allGroupsHistory[writeIdx].good = groupGoodCount;
      allGroupsHistory[writeIdx].normal = groupNormalCount;
      allGroupsHistory[writeIdx].bad = groupBadCount;

      time_t now;
      time(&now);
      allGroupsHistory[writeIdx].timestamp = (uint32_t)now;
      allGroupsHistory[writeIdx].recordMillis = millis();

      historyHead = (historyHead + 1) % MAX_HISTORY_SIZE;
      if (historyCount < MAX_HISTORY_SIZE) {
        historyCount++;
      }
      saveData();
      updateHistoryBLE();
    }
    playSound(4);
    currentState = STATE_SUMMARY_GROUP;
    summaryTimer = millis();
    summaryPage = 0;
    drawGroupSummary();
  }
}

void triggerShot(int type) {
  if (millis() - lastScoreTime < SCORE_COOLDOWN)
    return;
  lastScoreTime = millis();
  if (groupShots >= 10)
    return;

  totalShots++;
  groupShots++;

  if (type == 1) { // Good
    totalGood++;
    groupGoodCount++;
    groupHistory[groupShots - 1] = 1;
    animateSurge(C_GREEN);
    playSound(1);
  } else if (type == 2) { // Normal
    totalNormal++;
    groupNormalCount++;
    groupHistory[groupShots - 1] = 2;
    animateSurge(C_YELLOW);
    playSound(2);
  } else if (type == 3) { // Bad
    totalBad++;
    groupBadCount++;
    groupHistory[groupShots - 1] = 3;
    animateSurge(C_RED);
    playSound(3);
  }
  saveData();

  // [新增] 实时推送当前组数据
  if (deviceConnectedCount > 0) {
    String livePayload = "{\"live\":true,\"i\":" + String(currentGroupIdx + 1) +
                         ",\"g\":" + String(groupGoodCount) +
                         ",\"n\":" + String(groupNormalCount) +
                         ",\"b\":" + String(groupBadCount) +
                         ",\"s\":" + String(groupShots) + "}";
    pHistoryCharacteristic->setValue((uint8_t *)livePayload.c_str(),
                                     livePayload.length());
    pHistoryCharacteristic->notify();
  }

  checkGroupCompletion();
  if (currentState == STATE_PLAYING) {
    drawPlayingUI();
  }
}

void triggerUndo() {
  if (groupShots <= 0)
    return;
  int lastType = groupHistory[groupShots - 1];
  totalShots--;
  groupShots--;

  if (lastType == 1) {
    totalGood--;
    groupGoodCount--;
  } else if (lastType == 2) {
    totalNormal--;
    groupNormalCount--;
  } else if (lastType == 3) {
    totalBad--;
    groupBadCount--;
  }
  groupHistory[groupShots] = 0;
  saveData();

  // [新增] 撤销后实时推送
  if (deviceConnectedCount > 0) {
    String livePayload = "{\"live\":true,\"i\":" + String(currentGroupIdx + 1) +
                         ",\"g\":" + String(groupGoodCount) +
                         ",\"n\":" + String(groupNormalCount) +
                         ",\"b\":" + String(groupBadCount) +
                         ",\"s\":" + String(groupShots) + "}";
    pHistoryCharacteristic->setValue((uint8_t *)livePayload.c_str(),
                                     livePayload.length());
    pHistoryCharacteristic->notify();
  }

  xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); // [Lock]
  matrix.drawRect(0, 0, 16, 16, C_BLUE);
  requestDisplayUpdate();
  xSemaphoreGiveRecursive(displayMutex); // [Unlock]

  playSound(8);
  delay(200);
  drawPlayingUI();
}

void changeBrightness(int delta) {
  currentBrightness += delta;

  // 软件限制：5-100
  if (currentBrightness > BRT_MAX)
    currentBrightness = BRT_MAX;
  if (currentBrightness < 5)
    currentBrightness = 5;

  saveBrightness();

  xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); // [Lock]
  matrix.setBrightness(currentBrightness);              // 直接使用 0-100 范围
  xSemaphoreGiveRecursive(displayMutex);                // [Unlock]

  playSound(6);

  matrix.fillScreen(0);
  matrix.setTextColor(C_WHITE);
  int xPos =
      (currentBrightness >= 100) ? 1 : ((currentBrightness >= 10) ? 4 : 7);
  matrix.setCursor(xPos, 10);
  matrix.print(currentBrightness);
  requestDisplayUpdate(); // [S3 DUAL-CORE]
  delay(100);             // 缩短延迟以支持连续调整
}

void resetGame() {
  playSound(6);
  matrix.fillScreen(C_BLUE);
  requestDisplayUpdate(); // [S3 DUAL-CORE]
  totalShots = 0;
  totalGood = 0;
  totalNormal = 0;
  totalBad = 0;
  groupShots = 0;
  groupGoodCount = 0;
  groupNormalCount = 0;
  groupBadCount = 0;
  currentGroupIdx = 0;
  for (int i = 0; i < 10; i++) {
    groupHistory[i] = 0;
    groupResults[i] = 0;
  }
  clearData();
  delay(1000);
  currentState = STATE_PLAYING;
  drawPlayingUI();
  playSound(7);
}

void triggerCameraSequence() {
  if (!isAutoRecordEnabled)
    return;
  camSequenceStartTime = millis();
  hasSentStart = false;
  hasSentStop = false;
  drawPlayingUI();
}

void checkSleepTimeout() {
  if (!isScreenSaver && (millis() - lastActivityTime > SLEEP_TIMEOUT)) {
    isScreenSaver = true;
    saveData();
    matrix.fillScreen(0);
    requestDisplayUpdate(); // [S3 DUAL-CORE]
  }
}

void setupOTA() {
  matrix.fillScreen(0);
  matrix.setTextColor(C_BLUE);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int timeout = 0;
  // Increase timeout to 15 seconds (30 * 500ms)
  while (WiFi.status() != WL_CONNECTED && timeout < 30) {
    delay(500);
    matrix.drawPixel(0, 0, (timeout % 2 == 0) ? C_BLUE : 0);
    matrix.show();
    timeout++;
  }
  matrix.fillScreen(0);

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("pixelcaddy")) {
      Serial.println("mDNS started");
    }
    String fullIP = WiFi.localIP().toString();
    ipSuffix = fullIP.substring(fullIP.lastIndexOf('.') + 1);
    matrix.setTextColor(C_BLUE);
    matrix.setCursor(4, 6);
    matrix.print("IP");
    int xPos =
        (ipSuffix.length() == 1) ? 7 : ((ipSuffix.length() == 2) ? 4 : 1);
    matrix.setTextColor(C_GREEN);
    matrix.setCursor(xPos, 13);
    matrix.print(ipSuffix);
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Pixel Caddy OTA", "12345678");
    matrix.setTextColor(C_RED);
    matrix.setCursor(4, 9);
    matrix.print("AP");
  }
  matrix.show(); // [OTA Mode] Direct show (no dual-core in OTA)
  ElegantOTA.begin(&server);
  server.begin();
  playSound(5);
}

void setup() {
  pinMode(PIN_BTN_GOOD, INPUT_PULLUP);
  pinMode(PIN_BTN_NORMAL, INPUT_PULLUP);
  pinMode(PIN_BTN_BAD, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  Serial.begin(115200);

  // [Audio] 初始化蜂鸣器
  audio.begin();
  // 音量将在 loadData() 后设置

  // [Moved to Core 0 Task] matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(20);
  matrix.setRotation(1);
  matrix.setFont(&TomThumb);

  if (digitalRead(PIN_BTN_GOOD) == LOW && digitalRead(PIN_BTN_BAD) == LOW) {
    isOTAMode = true;
    setupOTA();
    return;
  }

  loadData();
  matrix.setBrightness(currentBrightness);
  updateHistoryBLE();

  BLEDevice::init("Pixel Caddy");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  // [修复] 增加句柄数量以支持3个特征 (默认15不够，需要至少30)
  BLEService *pService = pServer->createService(BLEUUID(SERVICE_UUID), 30);

  pHistoryCharacteristic = pService->createCharacteristic(
      CHAR_HISTORY_UUID, BLECharacteristic::PROPERTY_READ |
                             BLECharacteristic::PROPERTY_NOTIFY |
                             BLECharacteristic::PROPERTY_INDICATE |
                             BLECharacteristic::PROPERTY_WRITE |
                             BLECharacteristic::PROPERTY_WRITE_NR);
  pHistoryCharacteristic->addDescriptor(new BLE2902());
  pHistoryCharacteristic->setCallbacks(new HistoryCallbacks());

  pTimeCharacteristic = pService->createCharacteristic(
      CHAR_TIME_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pTimeCharacteristic->setCallbacks(new TimeCallbacks());

  // [新增] 电池电量特征值
  pBatteryCharacteristic = pService->createCharacteristic(
      CHAR_BATTERY_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pBatteryCharacteristic->addDescriptor(new BLE2902());

  // [New] Firmware Version Characteristic (Read Only)
  pVersionCharacteristic = pService->createCharacteristic(
      CHAR_VERSION_UUID, BLECharacteristic::PROPERTY_READ);
  pVersionCharacteristic->setValue(FIRMWARE_VERSION);

  // [New] Config Characteristic (Read/Write)
  pConfigCharacteristic = pService->createCharacteristic(
      CHAR_CONFIG_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pConfigCharacteristic->setCallbacks(new ConfigCallbacks());

  // [诊断] 立即读取一次电池电量，不再显示 999
  int bootBattery = getBatteryPercent();
  String initVal = String(bootBattery);
  pBatteryCharacteristic->setValue((uint8_t *)initVal.c_str(),
                                   initVal.length());

  pService->start();

  pHidDevice = new BLEHIDDevice(pServer);
  inputKeyboard = pHidDevice->inputReport(1);
  pHidDevice->manufacturer()->setValue("Espressif");
  pHidDevice->pnp(0x02, 0xe502, 0xa111, 0x0210);
  pHidDevice->hidInfo(0x00, 0x01);

  BLESecurity *pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
  pSecurity->setCapability(ESP_IO_CAP_NONE);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  const uint8_t reportMap[] = {
      0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
      0x75, 0x08, 0x81, 0x03, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
      0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0};

  pHidDevice->reportMap((uint8_t *)reportMap, sizeof(reportMap));
  pHidDevice->startServices();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  BLEAdvertisementData oAdvData = BLEAdvertisementData();
  oAdvData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  oAdvData.setAppearance(0x03C1);
  oAdvData.setCompleteServices(BLEUUID(uint16_t(0x1812)));
  pAdvertising->setAdvertisementData(oAdvData);
  pAdvertising->addServiceUUID(SERVICE_UUID);

  BLEAdvertisementData oScanResponseData = BLEAdvertisementData();
  oScanResponseData.setName("Pixel Caddy");
  oScanResponseData.setCompleteServices(BLEUUID(SERVICE_UUID));
  pAdvertising->setScanResponseData(oScanResponseData);

  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  // [S3 DUAL-CORE] Initialize mutex and LED refresh task
  // [Fix] Use RECURSIVE Mutex to allow nested locking from draw functions
  displayMutex = xSemaphoreCreateRecursiveMutex();
  if (displayMutex == NULL) {
    Serial.println("[ERROR] Failed to create display mutex!");
  }

  // Create LED refresh task on Core 1 (Avoid BLE interrupts on Core 0)
  xTaskCreatePinnedToCore(ledRefreshTask, // Task function
                          "LED_Refresh",  // Task name
                          4096,           // Stack size
                          NULL,           // Parameters
                          2,              // Priority (higher than default)
                          &ledTaskHandle, // Task handle
                          1 // [Fix] Pin to Core 1 (BLE is on Core 0)
  );

  matrix.fillScreen(0);
  matrix.setTextColor(C_GREEN);
  matrix.setCursor(4, 10);
  matrix.print("GO");
  requestDisplayUpdate(); // [S3 DUAL-CORE]
  delay(500);

  if (currentState == STATE_PLAYING) {
    if (groupShots >= 10) {
      checkGroupCompletion();
    } else {
      drawPlayingUI();
    }
  } else if (currentState == STATE_SUMMARY_GROUP) {
    drawGroupSummary();
  } else if (currentState == STATE_SUMMARY_FINAL) {
    drawFinalSummary();
  }
  lastActivityTime = millis();
}

void loop() {
  // [Audio] 更新音频播放器 (非阻塞)
  audio.update();

  // [电池] 定期更新电池电量 (每30秒)
  if (millis() - lastBatteryUpdate > BATTERY_UPDATE_INTERVAL) {
    lastBatteryUpdate = millis();
    updateBatteryBLE();
  }

  if (isOTAMode) {
    ElegantOTA.loop();
    return;
  }

  if (wipeRequested) {
    wipeRequested = false;
    historyHead = 0;
    historyCount = 0;
    totalShots = 0;
    totalGood = 0;
    totalNormal = 0;
    totalBad = 0;
    currentGroupIdx = 0;
    groupShots = 0;
    for (int i = 0; i < MAX_HISTORY_SIZE; i++) {
      memset(&allGroupsHistory[i], 0, sizeof(GroupRecord));
    }
    for (int i = 0; i < 10; i++) {
      groupResults[i] = 0;
    }
    saveData();
    playSound(6);
    if (currentState == STATE_PLAYING) {
      drawPlayingUI();
    }
  }

  if (requestedPage >= 0) {
    delay(500);
    sendHistoryPage(requestedPage);
    requestedPage = -1;
  }

  checkSleepTimeout();

  int rGood = digitalRead(PIN_BTN_GOOD);
  int rNorm = digitalRead(PIN_BTN_NORMAL);
  int rBad = digitalRead(PIN_BTN_BAD);
  unsigned long now = millis();
  bool anyKeyPressed = (rGood == LOW || rNorm == LOW || rBad == LOW);

  if (rNorm == LOW && rBad == LOW) {
    isBleEnabled = !isBleEnabled;
    if (isBleEnabled) {
      BLEDevice::startAdvertising();
      playSound(3);
    } else {
      BLEDevice::getAdvertising()->stop();
      playSound(2);
    }
    if (currentState == STATE_PLAYING) {
      drawPlayingUI();
    }
    while (digitalRead(PIN_BTN_NORMAL) == LOW ||
           digitalRead(PIN_BTN_BAD) == LOW)
      delay(10);
    lastStateNormal = HIGH;
    lastStateBad = HIGH;
    lastTriggerTime = millis();
    return;
  }

  if (rGood == LOW && rBad == LOW) {
    resetGame();
    while (digitalRead(PIN_BTN_GOOD) == LOW || digitalRead(PIN_BTN_BAD) == LOW)
      delay(10);
    lastStateGood = HIGH;
    lastStateNormal = HIGH;
    lastStateBad = HIGH;
    lastTriggerTime = millis();
    if (isScreenSaver)
      isScreenSaver = false;
    return;
  }

  if (rGood == LOW && rNorm == LOW) {
    isAutoRecordEnabled = !isAutoRecordEnabled;

    xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY); // [Lock]
    matrix.fillScreen(0);
    matrix.setTextColor(isAutoRecordEnabled ? C_GREEN : C_RED);
    matrix.setCursor(2, 6);
    matrix.print("CAM");
    matrix.setCursor(2, 13);
    matrix.print(isAutoRecordEnabled ? "ON" : "OFF");
    if (isAutoRecordEnabled)
      matrix.drawPixel(0, 0, matrix.Color(0, 50, 50));
    matrix
        .show(); // Note: direct show() is okay if we hold the lock, but
                 // requestDisplayUpdate is better. However, loop() might use
                 // show() directly for immediate blocking feedback. But since
                 // ledRefreshTask is running, better to use
                 // requestDisplayUpdate OR just hold lock and show. Current
                 // code uses matrix.show(). Since we hold the lock, it's safe!
    xSemaphoreGiveRecursive(displayMutex); // [Unlock]

    playSound(isAutoRecordEnabled ? 5 : 2);
    while (digitalRead(PIN_BTN_GOOD) == LOW ||
           digitalRead(PIN_BTN_NORMAL) == LOW)
      delay(10);
    lastStateGood = HIGH;
    lastStateNormal = HIGH;
    lastTriggerTime = millis();
    if (currentState == STATE_PLAYING)
      drawPlayingUI();
    return;
  }

  if (anyKeyPressed) {
    lastActivityTime = now;
    if (isScreenSaver) {
      isScreenSaver = false;
      if (currentState == STATE_PLAYING)
        drawPlayingUI();
      else if (currentState == STATE_SUMMARY_GROUP)
        drawGroupSummary();
      else if (currentState == STATE_SUMMARY_FINAL)
        drawFinalSummary();
      return;
    }
  }

  if (currentState == STATE_PLAYING) {
    if (currentGroupIdx >= 10 && groupShots == 0)
      return;
    if (now - lastTriggerTime > DEBOUNCE_LOCKOUT) {
      if (rGood == LOW && lastStateGood == HIGH) {
        lastTriggerTime = now;
        pressTimeGood = now;
        longPressHandledGood = false;
      }
      // [移除] 原长按增加亮度逻辑已移至设置菜单
      if (rGood == HIGH && lastStateGood == LOW) {
        lastTriggerTime = now;
        if (!longPressHandledGood) {
          triggerShot(1);
          triggerCameraSequence();
        }
      }

      if (rNorm == LOW && lastStateNormal == HIGH) {
        lastTriggerTime = now;
        pressTimeNormal = now;
        longPressHandledNormal = false;
      }
      if (rNorm == LOW && !longPressHandledNormal &&
          (now - pressTimeNormal > LONG_PRESS_DURATION)) {
        triggerUndo();
        longPressHandledNormal = true;
      }
      if (rNorm == HIGH && lastStateNormal == LOW) {
        lastTriggerTime = now;
        if (!longPressHandledNormal) {
          triggerShot(2);
          triggerCameraSequence();
        }
      }

      if (rBad == LOW && lastStateBad == HIGH) {
        lastTriggerTime = now;
        pressTimeBad = now;
        longPressHandledBad = false;
      }
      // [修改] 长按红键3秒进入设置菜单
      if (rBad == LOW && !longPressHandledBad &&
          (now - pressTimeBad > 3000)) { // 3秒进入设置
        currentState = STATE_SETTINGS;
        settingsMode = 0; // 默认亮度模式
        playSound(5);
        drawSettingsUI();
        longPressHandledBad = true;
        // 等待松开按键
        while (digitalRead(PIN_BTN_BAD) == LOW)
          delay(10);
        lastStateBad = HIGH;
        return;
      }
      if (rBad == HIGH && lastStateBad == LOW) {
        lastTriggerTime = now;
        if (!longPressHandledBad) {
          triggerShot(3);
          triggerCameraSequence();
        }
      }
      lastStateGood = rGood;
      lastStateNormal = rNorm;
      lastStateBad = rBad;
    }
  } else if (currentState == STATE_SUMMARY_GROUP) {
    bool btnPressed = false;
    if ((rGood == LOW && lastStateGood == HIGH) ||
        (rNorm == LOW && lastStateNormal == HIGH) ||
        (rBad == LOW && lastStateBad == HIGH)) {
      btnPressed = true;
    }
    lastStateGood = rGood;
    lastStateNormal = rNorm;
    lastStateBad = rBad;

    if (btnPressed) {
      playSound(7);
      unsigned long releaseStart = millis();
      while (digitalRead(PIN_BTN_GOOD) == LOW ||
             digitalRead(PIN_BTN_NORMAL) == LOW ||
             digitalRead(PIN_BTN_BAD) == LOW) {
        delay(10);
        if (millis() - releaseStart > 2000)
          break;
      }
      lastStateGood = HIGH;
      lastStateNormal = HIGH;
      lastStateBad = HIGH;
      lastTriggerTime = millis();
      currentGroupIdx++;
      groupShots = 0;
      groupGoodCount = 0;
      groupNormalCount = 0;
      groupBadCount = 0;
      for (int i = 0; i < 10; i++)
        groupHistory[i] = 0;

      if (currentGroupIdx >= 10) {
        currentState = STATE_SUMMARY_FINAL;
        summaryPage = 0;
        summaryTimer = millis();
        drawFinalSummary();
      } else {
        currentState = STATE_PLAYING;
        drawPlayingUI();
      }
    }
  } else if (currentState == STATE_SUMMARY_FINAL) {
    if (now - summaryTimer > SUMMARY_INTERVAL) {
      summaryTimer = now;
      summaryPage = (summaryPage + 1) % 3;
      drawFinalSummary();
    }
    lastStateBad = rBad;
  } else if (currentState == STATE_SETTINGS) {
    // 设置菜单按键处理
    static unsigned long settingsPressTimeGood = 0;
    static bool settingsLongPressGood = false;
    static unsigned long settingsLastActionTime = 0; // [新增] 冷却计时器
    const unsigned long SETTINGS_COOLDOWN = 500;     // 500ms 冷却时间

    // 绿键：短按切换模式，长按3秒退出
    if (rGood == LOW && lastStateGood == HIGH) {
      settingsPressTimeGood = now;
      settingsLongPressGood = false;
    }
    if (rGood == LOW && !settingsLongPressGood &&
        (now - settingsPressTimeGood > 3000)) {
      // 长按3秒退出设置
      settingsLongPressGood = true;
      saveData(); // 保存设置
      playSound(5);
      currentState = STATE_PLAYING;
      drawPlayingUI();
      while (digitalRead(PIN_BTN_GOOD) == LOW)
        delay(10);
      lastStateGood = HIGH;
      return;
    }
    if (rGood == HIGH && lastStateGood == LOW) {
      unsigned long pressDuration = now - settingsPressTimeGood;
      // [冷却] 检查是否过了冷却时间
      if (!settingsLongPressGood && pressDuration < 1000 &&
          (now - settingsLastActionTime > SETTINGS_COOLDOWN)) {
        // 短按切换模式
        settingsMode = (settingsMode + 1) % SETTINGS_MODE_COUNT;
        playSound(1);
        drawSettingsUI();
        settingsLastActionTime = now; // 记录操作时间
      }
    }

    // [冷却] 只有过了冷却时间才响应
    if (now - settingsLastActionTime > SETTINGS_COOLDOWN) {
      // 黄键：增加数值
      if (rNorm == LOW && lastStateNormal == HIGH) {
        if (settingsMode == 0) {
          currentBrightness += SETTING_STEP;
          if (currentBrightness > BRT_MAX)
            currentBrightness = BRT_MAX;
          matrix.setBrightness(currentBrightness);
        } else {
          currentVolume += SETTING_STEP;
          if (currentVolume > VOL_MAX)
            currentVolume = VOL_MAX;
          audio.setVolume(currentVolume);
        }
        playSound(1);
        drawSettingsUI();
        settingsLastActionTime = now; // 记录操作时间
      }

      // 红键：减少数值
      if (rBad == LOW && lastStateBad == HIGH) {
        if (settingsMode == 0) {
          currentBrightness -= SETTING_STEP;
          if (currentBrightness < BRT_MIN)
            currentBrightness = BRT_MIN;
          matrix.setBrightness(currentBrightness);
        } else {
          currentVolume -= SETTING_STEP;
          if (currentVolume < VOL_MIN)
            currentVolume = VOL_MIN;
          audio.setVolume(currentVolume);
        }
        playSound(1);
        drawSettingsUI();
        settingsLastActionTime = now; // 记录操作时间
      }
    }

    lastStateGood = rGood;
    lastStateNormal = rNorm;
    lastStateBad = rBad;
  }

  if (uiRefreshRequested) {
    uiRefreshRequested = false;
    if (currentState == STATE_PLAYING) {
      drawPlayingUI();
    } else if (currentState == STATE_SUMMARY_GROUP) {
      drawGroupSummary();
    } else if (currentState == STATE_SUMMARY_FINAL) {
      drawFinalSummary();
    }
  }

  if (advertisingRestartRequested) {
    advertisingRestartRequested = false;
    if (deviceConnectedCount < 3) {
      Serial.println("Restarting Advertising for Multi-Connect...");
      delay(10);
      pServer->getAdvertising()->start();
    }
  }

  if (isAutoRecordEnabled && camSequenceStartTime > 0) {
    long elapsed = millis() - camSequenceStartTime;
    if (elapsed >= CAM_PRE_DELAY && !hasSentStart) {
      sendHIDKey(0x28);
      hasSentStart = true;
      playSound(1);
    }
    if (elapsed >= (CAM_PRE_DELAY + CAM_REC_DURATION) && !hasSentStop) {
      sendHIDKey(0x28);
      hasSentStop = true;
      camSequenceStartTime = 0;
      playSound(2);
      drawPlayingUI();
    }
    static int lastDisplayedSeconds = -1;
    int currentSeconds = elapsed / 1000;
    if (currentSeconds != lastDisplayedSeconds) {
      lastDisplayedSeconds = currentSeconds;
      drawPlayingUI();
    }
  }

  // [NEW] Update Audio Engine
  audio.update();
}
