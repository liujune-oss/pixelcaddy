/*
 * Pixel Caddy - Mark 4 (Pro Analytics Edition)
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

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <Fonts/TomThumb.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

// ================= 1. 用户配置 =================
const char *ssid = "iamnotap";
const char *password = "55955666";

// ================= 2. 硬件引脚 =================
#define PIN_MATRIX 9     // D9 (GPIO 9)
#define PIN_BTN_GOOD 4   // D2
#define PIN_BTN_NORMAL 2 // D0
#define PIN_BTN_BAD 3    // D1
#define PIN_BUZZER 21    // D6 (GPIO 21)

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

// ================= BLE Objects & Logic =================
#define SERVICE_UUID "5fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_HISTORY_UUID                                                      \
  "beb5483e-36e1-4688-b7f5-ea07361b26ac" // Changed to 'ac' to bust cache again
#define CHAR_TIME_UUID "87a7d400-5343-4565-a9b7-1601b0034876"

BLEServer *pServer = NULL;
BLECharacteristic *pHistoryCharacteristic = NULL;
BLECharacteristic *pTimeCharacteristic = NULL;
int deviceConnectedCount = 0; // [Fix] Counter instead of bool
bool isBleEnabled = true;
bool oldDeviceConnected = false;
bool wipeRequested = false; // [New] Flag for remote wipe

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

// Forward Declarations
void saveData();
void sendHistoryPage(int page);

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
      Serial.println("Forced Notifications ON");
    }

    BLEDevice::startAdvertising(); // [Fix] Allow multiple connections
    Serial.print("Device Connected. Count: ");
    Serial.println(deviceConnectedCount);
  };
  void onDisconnect(BLEServer *pServer) {
    deviceConnectedCount--;
    if (deviceConnectedCount < 0)
      deviceConnectedCount = 0;
    BLEDevice::startAdvertising(); // Ensure advertising restarts
    Serial.print("Device Disconnected. Count: ");
    Serial.println(deviceConnectedCount);
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
    if (value.length() > 0) {
      long t = value.toInt(); // e.g. "1700000000"
      if (t > 1600000000) {
        struct timeval now = {.tv_sec = (time_t)t, .tv_usec = 0};
        settimeofday(&now, NULL);
        Serial.println("Time synced via BLE");

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
// New Paged Sender
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

// 状态机定义
enum GameState { STATE_PLAYING, STATE_SUMMARY_GROUP, STATE_SUMMARY_FINAL };
GameState currentState = STATE_PLAYING;

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

// 设置
int currentBrightness = 20;
const int BRT_STEP = 10;

// 长按与防抖
unsigned long lastActivityTime = 0;
const unsigned long SLEEP_TIMEOUT = 10 * 60 * 1000;
unsigned long pressTimeGood = 0;
unsigned long pressTimeNormal = 0;
unsigned long pressTimeBad = 0;
bool longPressHandledGood = false;
bool longPressHandledNormal = false;
bool longPressHandledBad = false;
const int LONG_PRESS_DURATION = 3000;
int lastStateGood = HIGH;
int lastStateNormal = HIGH;
int lastStateBad = HIGH;
unsigned long lastTriggerTime = 0;
const int DEBOUNCE_LOCKOUT = 80;
unsigned long lastScoreTime = 0;
const int SCORE_COOLDOWN = 1000;

// ================= 5. 数据存取 =================
// ================= 5. 数据存取 =================
void loadData() {
  prefs.begin("pixelcaddy", false);
  totalShots = prefs.getInt("total", 0);
  totalGood = prefs.getInt("good", 0);
  totalNormal = prefs.getInt("normal", 0);
  totalBad = prefs.getInt("bad", 0);
  currentBrightness = prefs.getInt("brt", 20);
  currentGroupIdx = prefs.getInt("groupIdx", 0);
  groupShots = prefs.getInt("groupShots", 0);
  groupGoodCount = prefs.getInt("groupGood", 0);
  groupNormalCount = prefs.getInt("groupNormal", 0);
  groupBadCount = prefs.getInt("groupBad", 0);
  currentState = (GameState)prefs.getInt("state", STATE_PLAYING);

  // Load UI arrays
  prefs.getBytes("ui_hist", groupHistory, sizeof(groupHistory));
  prefs.getBytes("ui_res", groupResults, sizeof(groupResults));

  // Load Ring Buffer History
  historyCount = prefs.getInt("h_cnt", 0);
  historyHead = prefs.getInt("h_head", 0);
  prefs.getBytes("all_hist", allGroupsHistory, sizeof(allGroupsHistory));

  prefs.end();
}

void saveData() {
  prefs.begin("pixelcaddy", false);
  prefs.putInt("total", totalShots);
  prefs.putInt("good", totalGood);
  prefs.putInt("normal", totalNormal);
  prefs.putInt("bad", totalBad);
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

// ================= 6. 辅助功能 =================
void playSound(int type) {
  int pin = PIN_BUZZER;
  // 简化的音效调用，保持原样
  if (type == 4) { // 组完成
    for (int k = 0; k < 3; k++) {
      for (int i = 0; i < 60; i++) {
        digitalWrite(pin, HIGH);
        delayMicroseconds(300);
        digitalWrite(pin, LOW);
        delayMicroseconds(300);
      }
      delay(80);
    }
  } else if (type == 5) { // 胜利
    for (int i = 0; i < 400; i++) {
      digitalWrite(pin, HIGH);
      delayMicroseconds(250);
      digitalWrite(pin, LOW);
      delayMicroseconds(250);
    }
  } else { // 简单的一声
    for (int i = 0; i < 100; i++) {
      digitalWrite(pin, HIGH);
      delayMicroseconds(200);
      digitalWrite(pin, LOW);
      delayMicroseconds(200);
    }
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

// ================= 7. UI 绘制逻辑 (分状态) =================

// 7.1 游戏进行中界面
void drawPlayingUI() {
  matrix.fillScreen(0);
  // 顶部进度条
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

  // 数字
  matrix.setTextColor(C_GREEN);
  int goodX = (totalGood < 10) ? 4 : 1;
  matrix.setCursor(goodX, 7);
  matrix.print(totalGood);
  matrix.setTextColor(C_BLUE);
  int shotX = (totalShots < 10) ? 12 : 9;
  matrix.setCursor(shotX, 7);
  matrix.print(totalShots);

  // 综合评分 (Weighted Score)
  // Good=100, Normal=50, Bad=0
  int score = 0;
  if (totalShots > 0) {
    score = (totalGood * 100 + totalNormal * 50) / totalShots;
  }

  matrix.setTextColor(C_WHITE);
  if (score == 100) {
    matrix.setCursor(3, 13);
    matrix.print(100);
  } else {
    // 显示整数分数
    int scoreX = (score >= 10) ? 4 : 7;
    matrix.setCursor(scoreX, 13);
    matrix.print(score);
  }

  // 底部本组详情
  matrix.drawPixel(2, 14, C_WHITE);
  matrix.drawPixel(2, 15, C_WHITE);
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
    matrix.drawPixel(x, 15, color);
  }
  matrix.drawPixel(13, 14, C_WHITE);
  matrix.drawPixel(13, 15, C_WHITE);

  // [Fix] 绘制蓝牙指示灯 (右上角 15,0)
  if (isBleEnabled) {
    matrix.drawPixel(15, 0, matrix.Color(0, 0, 125)); // Blue Dot
  }

  matrix.show();
}

// 7.2 小组结算界面
void drawGroupSummary() {
  matrix.fillScreen(0);

  // 第一行：Good
  matrix.setTextColor(C_GREEN);
  int goodX = (groupGoodCount < 10) ? 6 : 3;
  matrix.setCursor(goodX, 6);
  matrix.print(groupGoodCount);

  // 第二行：Normal 和 Bad
  matrix.setTextColor(C_YELLOW);
  int normX = (groupNormalCount < 10) ? 2 : 0;
  matrix.setCursor(normX, 13);
  matrix.print(groupNormalCount);

  matrix.setTextColor(C_RED);
  int badX = (groupBadCount < 10) ? 10 : 8;
  matrix.setCursor(badX, 13);
  matrix.print(groupBadCount);

  matrix.show();
}

// 7.3 全场结算界面 (带百分比)
void drawFinalSummary() {
  matrix.fillScreen(0);

  int count = 0;
  float percent = 0.0;
  uint16_t color = C_WHITE;

  if (summaryPage == 0) { // Total Good
    count = totalGood;
    if (totalShots > 0)
      percent = (float)totalGood / totalShots * 100.0;
    color = C_GREEN;
  } else if (summaryPage == 1) { // Total Normal
    count = totalNormal;
    if (totalShots > 0)
      percent = (float)totalNormal / totalShots * 100.0;
    color = C_YELLOW;
  } else { // Total Bad
    count = totalBad;
    if (totalShots > 0)
      percent = (float)totalBad / totalShots * 100.0;
    color = C_RED;
  }

  // 第一行：数量 (颜色)
  matrix.setTextColor(color);
  int xNum = (count >= 100) ? 1 : ((count >= 10) ? 4 : 7);
  matrix.setCursor(xNum, 6);
  matrix.print(count);

  // 第二行：百分比 (白色)
  matrix.setTextColor(C_WHITE);
  int xPer = 0; // 统一从最左开始
  matrix.setCursor(xPer, 14);
  if (percent == 100.0) {
    matrix.print("100");
  } else {
    matrix.print(percent, 1); // 显示1位小数
  }

  matrix.show();
}

// ================= 8. 逻辑控制 =================

// 处理动画 (边缘呼吸/Edge Breathing)
void animateSurge(uint16_t c) {
  // 1. 先显示最新的分数值 (不遮挡数据)
  drawPlayingUI();

  // 2. 绘制外圈边框 "呼吸" 效果
  // Frame 1: 内圈扩张
  matrix.drawRect(1, 1, 14, 14, c);
  matrix.show();
  delay(80);

  // Frame 2: 外圈高亮 (保持)
  drawPlayingUI(); // 清除内圈
  matrix.drawRect(0, 0, 16, 16, c);
  matrix.show();
  delay(150);

  // 动画结束会自动由外部逻辑刷新回正常界面
}

// 检查是否完成一组
void checkGroupCompletion() {
  if (groupShots >= 10) {
    delay(200);
    // 1. 记录本组结果颜色
    int colorType = calculateGroupColorType(groupGoodCount, groupNormalCount);
    if (currentGroupIdx < 10) {
      groupResults[currentGroupIdx] = colorType;

      // [Update] 写入环形缓冲区 (Ring Buffer)
      int writeIdx = historyHead;

      allGroupsHistory[writeIdx].good = groupGoodCount;
      allGroupsHistory[writeIdx].normal = groupNormalCount;
      allGroupsHistory[writeIdx].bad = groupBadCount;

      time_t now;
      time(&now);
      allGroupsHistory[writeIdx].timestamp = (uint32_t)now;
      allGroupsHistory[writeIdx].recordMillis = millis(); // 记录此时的开机时长

      // 更新指针
      historyHead = (historyHead + 1) % MAX_HISTORY_SIZE;
      if (historyCount < MAX_HISTORY_SIZE) {
        historyCount++;
      }

      saveData();         // 保存
      updateHistoryBLE(); // 更新蓝牙
    }

    // 2. 播放音效
    playSound(4);

    // 3. 切换状态 -> 小组结算
    currentState = STATE_SUMMARY_GROUP;
    summaryTimer = millis();
    summaryPage = 0;

    // 4. 立即刷新屏幕
    drawGroupSummary();
  }
}

// 记分触发器
void triggerShot(int type) {
  if (millis() - lastScoreTime < SCORE_COOLDOWN)
    return;
  lastScoreTime = millis();

  // [Fix] Safety Guard: Prevent >10 shots
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

  // 检查是否打完了这组
  checkGroupCompletion();

  // 如果未切换状态，刷新界面
  if (currentState == STATE_PLAYING) {
    drawPlayingUI();
  }
}

// 撤销
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

  matrix.drawRect(0, 0, 16, 16, C_BLUE);
  matrix.show();
  playSound(8);
  delay(200);
  drawPlayingUI();
}

// 亮度调节
void changeBrightness(int delta) {
  currentBrightness += delta;
  if (currentBrightness > 100)
    currentBrightness = 100;
  if (currentBrightness < 5)
    currentBrightness = 5;
  saveBrightness();
  matrix.setBrightness(currentBrightness);
  playSound(6);
  // 简单闪烁提示
  matrix.fillScreen(C_WHITE);
  matrix.show();
  delay(100);
  if (currentState == STATE_PLAYING)
    drawPlayingUI();
}

// 重置
void resetGame() {
  playSound(6);
  matrix.fillScreen(C_BLUE);
  matrix.show();
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
  currentState = STATE_PLAYING; // 强制回游戏模式
  drawPlayingUI();
  playSound(7);
}

// ================= 9. 屏保 =================
void checkSleepTimeout() {
  if (!isScreenSaver && (millis() - lastActivityTime > SLEEP_TIMEOUT)) {
    isScreenSaver = true;
    saveData();
    matrix.fillScreen(0);
    matrix.show();
  }
}

// ================= 10. OTA =================
void setupOTA() {
  matrix.fillScreen(0);
  matrix.setTextColor(C_BLUE);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 20) {
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
  matrix.show();
  ElegantOTA.begin(&server);
  server.begin();
  playSound(5);
}

// ================= 11. Setup =================
void setup() {
  pinMode(PIN_BTN_GOOD, INPUT_PULLUP);
  pinMode(PIN_BTN_NORMAL, INPUT_PULLUP);
  pinMode(PIN_BTN_BAD, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  Serial.begin(115200);

  matrix.begin();
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
  updateHistoryBLE(); // [新增] 初始化时准备蓝牙数据

  // [新增] BLE 初始化
  BLEDevice::init("Pixel Caddy");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // History Characteristic (Read/Notify)
  // History Characteristic (Read/Notify/Write ->
  // Read/Notify/Indicate/Write/WriteNR)
  pHistoryCharacteristic = pService->createCharacteristic(
      CHAR_HISTORY_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY |
          BLECharacteristic::PROPERTY_INDICATE |
          BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR); // [Fix] Allow No-Response
  pHistoryCharacteristic->addDescriptor(new BLE2902());
  pHistoryCharacteristic->setCallbacks(new HistoryCallbacks());

  // Time Characteristic (Write / WriteNR)
  pTimeCharacteristic = pService->createCharacteristic(
      CHAR_TIME_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pTimeCharacteristic->setCallbacks(new TimeCallbacks());

  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  // Default advertising interval is ~100ms which is much lighter on CPU than
  // 0x06 pAdvertising->setMinPreferred(0x06);
  // pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE Started");

  if (currentState == STATE_PLAYING) {
    drawPlayingUI();
  }
  lastActivityTime = millis();
}

// ================= 12. Loop (核心修改) =================
void loop() {
  if (isOTAMode) {
    ElegantOTA.loop();
    return;
  }

  // [New] Handle Wipe Request
  if (wipeRequested) {
    wipeRequested = false;
    Serial.println("Wiping Data...");

    // 1. Clear Memory
    historyHead = 0;
    historyCount = 0;
    totalShots = 0;
    totalGood = 0;
    totalNormal = 0;
    totalBad = 0;
    currentGroupIdx = 0;
    groupShots = 0;

    // Clear Arrays
    for (int i = 0; i < MAX_HISTORY_SIZE; i++) {
      memset(&allGroupsHistory[i], 0, sizeof(GroupRecord));
    }
    for (int i = 0; i < 10; i++) {
      groupResults[i] = 0;
    }

    // 2. Save Empty State
    saveData();

    // 3. Feedback
    playSound(6); // Special beep
    Serial.println("Data Wiped Successfully");

    // 4. Force UI Refresh
    if (currentState == STATE_PLAYING) {
      drawPlayingUI();
    }
  }

  // [New] Process Paged History Request (Non-blocking)
  if (requestedPage >= 0) {
    // [DEADLOCK FIX] Wait for Write Response to clear before Indicating
    // Prevents browser from choking on simultaneous ACKs
    delay(500); // Re-enabled: Essential for stable Web Bluetooth operation
    sendHistoryPage(requestedPage);
    requestedPage = -1;
  }

  // 1. 屏保检查
  checkSleepTimeout();

  // 2. 读取按键
  int rGood = digitalRead(PIN_BTN_GOOD);
  int rNorm = digitalRead(PIN_BTN_NORMAL);
  int rBad = digitalRead(PIN_BTN_BAD);
  unsigned long now = millis();
  bool anyKeyPressed = (rGood == LOW || rNorm == LOW || rBad == LOW);

  // [新增] 蓝牙开关组合键 (Normal + Bad)
  if (rNorm == LOW && rBad == LOW) {
    // 切换状态
    isBleEnabled = !isBleEnabled;

    if (isBleEnabled) {
      BLEDevice::startAdvertising();
      playSound(3); // 开提示音
    } else {
      BLEDevice::getAdvertising()->stop();
      // 简单处理：停止广播，若要断开现有连接可添加 pServer->disconnect(0)
      playSound(2); // 关提示音
    }

    // 刷新界面 (更新右上角蓝点)
    if (currentState == STATE_PLAYING) {
      drawPlayingUI();
    }

    // 防误触：等待释放
    while (digitalRead(PIN_BTN_NORMAL) == LOW ||
           digitalRead(PIN_BTN_BAD) == LOW)
      delay(10);

    // 重置按键状态，防止触发单球计数
    lastStateNormal = HIGH;
    lastStateBad = HIGH;
    lastTriggerTime = millis();
    return;
  }

  // 3. 智能复位 (全局有效)
  if (rGood == LOW && rBad == LOW) {
    resetGame();
    // Wait for buttons to be released
    while (digitalRead(PIN_BTN_GOOD) == LOW || digitalRead(PIN_BTN_BAD) == LOW)
      delay(10);

    // Fix: Synchronize button states to prevent phantom triggers on release
    lastStateGood = HIGH;
    lastStateNormal = HIGH;
    lastStateBad = HIGH;

    lastTriggerTime = millis();
    if (isScreenSaver)
      isScreenSaver = false;
    return;
  }

  // 4. 唤醒逻辑
  if (anyKeyPressed) {
    lastActivityTime = now;
    if (isScreenSaver) {
      isScreenSaver = false;
      // 恢复显示当前状态
      if (currentState == STATE_PLAYING)
        drawPlayingUI();
      else if (currentState == STATE_SUMMARY_GROUP)
        drawGroupSummary();
      else if (currentState == STATE_SUMMARY_FINAL)
        drawFinalSummary();

      // 注意：如果是唤醒，我们应该消耗掉这次按键，防止误触发记分
      // Consume the wake-up event
      return;
    }
  }

  // 5. 状态机逻辑
  if (currentState == STATE_PLAYING) {
    // ------ 游戏模式 ------

    // 只有打满10组后，这里会稍微挡一下，防止额外记分
    if (currentGroupIdx >= 10 && groupShots == 0)
      return;

    if (now - lastTriggerTime > DEBOUNCE_LOCKOUT) {
      // 绿键
      if (rGood == LOW && lastStateGood == HIGH) {
        lastTriggerTime = now;
        pressTimeGood = now;
        longPressHandledGood = false;
      }
      if (rGood == LOW && !longPressHandledGood &&
          (now - pressTimeGood > LONG_PRESS_DURATION)) {
        changeBrightness(BRT_STEP);
        longPressHandledGood = true;
      }
      if (rGood == HIGH && lastStateGood == LOW) {
        lastTriggerTime = now;
        if (!longPressHandledGood)
          triggerShot(1);
      }

      // 黄键
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
        if (!longPressHandledNormal)
          triggerShot(2);
      }

      // 红键
      if (rBad == LOW && lastStateBad == HIGH) {
        lastTriggerTime = now;
        pressTimeBad = now;
        longPressHandledBad = false;
      }
      if (rBad == LOW && !longPressHandledBad &&
          (now - pressTimeBad > LONG_PRESS_DURATION)) {
        changeBrightness(-BRT_STEP);
        longPressHandledBad = true;
      }
      if (rBad == HIGH && lastStateBad == LOW) {
        lastTriggerTime = now;
        if (!longPressHandledBad)
          triggerShot(3);
      }

      lastStateGood = rGood;
      lastStateNormal = rNorm;
      lastStateBad = rBad;
    }

  } else if (currentState == STATE_SUMMARY_GROUP) {
    // ------ 小组结算模式 ------

    // 任意键退出
    // 检测按键按下的一瞬间 (Falling Edge)
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
      playSound(7); // 提示音

      // [Fix] Wait for button release to prevent accidental trigger in next
      // state
      unsigned long releaseStart = millis();
      while (digitalRead(PIN_BTN_GOOD) == LOW ||
             digitalRead(PIN_BTN_NORMAL) == LOW ||
             digitalRead(PIN_BTN_BAD) == LOW) {
        delay(10);
        // Failsafe exit after 2 seconds (in case of hardware stuck)
        if (millis() - releaseStart > 2000)
          break;
      }

      // [CRITICAL FIX] Sync last states to HIGH so Playing Mode doesn't see a
      // release edge
      lastStateGood = HIGH;
      lastStateNormal = HIGH;
      lastStateBad = HIGH;

      // Update timer to ensure next state ignores initial noise
      lastTriggerTime = millis();

      // 退出结算逻辑
      currentGroupIdx++; // 正式进入下一组

      // 清空小组数据
      groupShots = 0;
      groupGoodCount = 0;
      groupNormalCount = 0;
      groupBadCount = 0;
      for (int i = 0; i < 10; i++)
        groupHistory[i] = 0;

      if (currentGroupIdx >= 10) {
        // 如果打完10组了，进入全场结算
        currentState = STATE_SUMMARY_FINAL;
        summaryPage = 0;
        summaryTimer = millis();
        drawFinalSummary();
      } else {
        // 否则回到游戏继续打
        currentState = STATE_PLAYING;
        drawPlayingUI();
      }
    }

  } else if (currentState == STATE_SUMMARY_FINAL) {
    // ------ 全场结算模式 ------

    // 1. 自动轮播 (2秒) Total Stats
    if (now - summaryTimer > SUMMARY_INTERVAL) {
      summaryTimer = now;
      summaryPage = (summaryPage + 1) % 3;
      drawFinalSummary();
    }

    // 在全场结算模式下，按键目前设计为无反应（只能看）
    // 直到用户使用 "智能复位" (红+绿) 重新开始比赛
    // 只是为了防止死锁，更新按键状态
    lastStateGood = rGood;
    lastStateNormal = rNorm;
    lastStateBad = rBad;
  }
}