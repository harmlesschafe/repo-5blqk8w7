/*
 * 题目19：智能温室通风控制系统
 * 平台：Arduino Uno（Wokwi 仿真）
 * 硬件：DHT22 温湿度传感器、SG90 舵机、旋钮电位器、按键、
 *       0.96 寸 OLED（SSD1306，128x64，I2C）、共阴极 RGB LED
 *
 * 功能：
 *  1. DHT22 读取温湿度，OLED 实时显示环境数据和当前模式；
 *  2. 自动模式：温度超过阈值时舵机打开通风窗，否则关闭；
 *  3. 手动模式：旋钮电位器控制舵机角度；按键切换手动/自动模式；
 *  4. RGB LED 显示状态：正常-绿色，通风-蓝色，温度过高-红色。
 */
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Servo.h>

// ---------------- 引脚定义 ----------------
const uint8_t BUTTON_PIN = 2;    // 模式切换按键，另一端接 GND（使用内部上拉）
const uint8_t LED_B_PIN  = 3;    // RGB LED 蓝色通道（串 220Ω 电阻）
const uint8_t LED_G_PIN  = 5;    // RGB LED 绿色通道（串 220Ω 电阻）
const uint8_t LED_R_PIN  = 6;    // RGB LED 红色通道（串 220Ω 电阻）
const uint8_t DHT_PIN    = 7;    // DHT22 数据脚
const uint8_t SERVO_PIN  = 9;    // 舵机信号线
const uint8_t POT_PIN    = A0;   // 旋钮电位器中间脚
// OLED：SDA -> A4，SCL -> A5（硬件 I2C）

// ---------------- 参数设置 ----------------
const float TEMP_VENT_ON  = 28.0;  // 自动模式：温度高于此值打开通风窗
const float TEMP_VENT_OFF = 27.0;  // 自动模式：温度低于此值才关闭（1℃回差，防止舵机在阈值附近反复动作）
const float TEMP_HIGH     = 35.0;  // 温度过高报警阈值（红灯）

const int VENT_CLOSED = 0;         // 通风窗关闭时的舵机角度
const int VENT_OPEN   = 90;        // 通风窗全开时的舵机角度
const int VENT_MIN_OPEN = 5;       // 开度大于该角度才算“正在通风”

const unsigned long DHT_INTERVAL  = 2000;  // DHT22 采样周期（DHT22 要求 ≥2 s）
const unsigned long OLED_INTERVAL = 200;   // OLED 刷新周期
const unsigned long DEBOUNCE_MS   = 50;    // 按键消抖时间

// ---------------- 类型与全局变量 ----------------
enum Mode   { MODE_AUTO, MODE_MANUAL };            // 工作模式
enum Status { ST_NORMAL, ST_VENT, ST_HOT };        // 系统状态（决定 RGB 颜色）

Adafruit_SSD1306 display(128, 64, &Wire, -1);
DHT dht(DHT_PIN, DHT22);
Servo ventServo;

Mode  mode = MODE_AUTO;      // 上电默认自动模式
float temperature = NAN;     // 最近一次有效温度
float humidity    = NAN;     // 最近一次有效湿度
bool  sensorOk    = false;   // 最近一次读取是否成功
bool  autoVentOpen = false;  // 自动模式下通风窗的开关状态
int   servoAngle  = VENT_CLOSED;
int   knobPercent = 0;       // 电位器位置 0~100%

unsigned long lastDhtTime  = 0;
unsigned long lastOledTime = 0;

// ---------------- 按键检测（消抖 + 下降沿） ----------------
// 每按下一次只返回一次 true
bool buttonPressed() {
  static bool stableState = HIGH;          // 消抖后的稳定电平
  static bool lastReading = HIGH;          // 上一次读到的原始电平
  static unsigned long lastChange = 0;     // 原始电平最近一次变化的时刻

  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) {            // 电平有变化（可能是抖动），重新计时
    lastReading = reading;
    lastChange = millis();
  }
  if (millis() - lastChange > DEBOUNCE_MS && reading != stableState) {
    stableState = reading;                 // 电平稳定超过 50 ms，才认为状态真正改变
    return stableState == LOW;             // 由高变低 = 按下
  }
  return false;
}

// ---------------- 读取 DHT22 ----------------
void readSensor() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {              // 读取失败：保留上一次的有效数据
    sensorOk = false;
    return;
  }
  humidity = h;
  temperature = t;
  sensorOk = true;
}

// 电位器读 8 次取平均，减小抖动
int readKnob() {
  long sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogRead(POT_PIN);
  return sum / 8;
}

// ---------------- 通风窗（舵机）控制 ----------------
void updateVent() {
  int raw = readKnob();
  knobPercent = map(raw, 0, 1023, 0, 100);

  int target;
  if (mode == MODE_AUTO) {
    // 带回差的阈值判断：>28℃ 打开，<27℃ 关闭，中间保持原状态
    if (sensorOk) {
      if (!autoVentOpen && temperature > TEMP_VENT_ON)  autoVentOpen = true;
      if (autoVentOpen  && temperature < TEMP_VENT_OFF) autoVentOpen = false;
    }
    target = autoVentOpen ? VENT_OPEN : VENT_CLOSED;
  } else {
    // 手动模式：电位器 0~1023 映射为 0~90°
    target = map(raw, 0, 1023, VENT_CLOSED, VENT_OPEN);
  }

  if (target != servoAngle) {              // 角度变化时才写舵机
    servoAngle = target;
    ventServo.write(servoAngle);
  }
}

// ---------------- 状态判断与 RGB 指示 ----------------
Status getStatus() {
  if (sensorOk && temperature >= TEMP_HIGH) return ST_HOT;   // 温度过高优先级最高
  if (servoAngle > VENT_MIN_OPEN)           return ST_VENT;  // 通风窗处于打开状态
  return ST_NORMAL;
}

void setRGB(bool r, bool g, bool b) {      // 共阴极：HIGH 点亮
  digitalWrite(LED_R_PIN, r);
  digitalWrite(LED_G_PIN, g);
  digitalWrite(LED_B_PIN, b);
}

void updateLed(Status st) {
  switch (st) {
    case ST_HOT:    setRGB(1, 0, 0); break;  // 红：温度过高
    case ST_VENT:   setRGB(0, 0, 1); break;  // 蓝：正在通风
    case ST_NORMAL: setRGB(0, 1, 0); break;  // 绿：正常
  }
}

// ---------------- OLED 显示 ----------------
void updateDisplay(Status st) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // 第 1 行：标题 + 右上角反色模式标签
  display.setCursor(0, 1);
  display.print(F("GREENHOUSE"));
  const char *tag = (mode == MODE_AUTO) ? "AUTO" : "MANUAL";
  int tagW = strlen(tag) * 6 + 4;
  display.fillRect(128 - tagW, 0, tagW, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(128 - tagW + 2, 1);
  display.print(tag);
  display.setTextColor(SSD1306_WHITE);
  display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

  // 温度、湿度
  display.setCursor(0, 16);
  display.print(F("Temp : "));
  if (isnan(temperature)) display.print(F("--.-"));
  else                    display.print(temperature, 1);
  display.write(248);                      // ° 符号（CP437 编码）
  display.print('C');

  display.setCursor(0, 26);
  display.print(F("Humi : "));
  if (isnan(humidity)) display.print(F("--.-"));
  else                 display.print(humidity, 1);
  display.print(F(" %"));

  // 通风窗角度 + 开度条
  display.setCursor(0, 36);
  display.print(F("Vent : "));
  display.print(servoAngle);
  display.write(248);
  display.drawRect(78, 36, 50, 8, SSD1306_WHITE);
  display.fillRect(78, 36, map(servoAngle, VENT_CLOSED, VENT_OPEN, 0, 50), 8, SSD1306_WHITE);

  // 自动模式显示阈值，手动模式显示旋钮位置
  display.setCursor(0, 46);
  if (mode == MODE_AUTO) {
    display.print(F("Limit: "));
    display.print(TEMP_VENT_ON, 1);
    display.write(248);
    display.print('C');
  } else {
    display.print(F("Knob : "));
    display.print(knobPercent);
    display.print(F(" %"));
  }

  // 最后一行：系统状态
  display.setCursor(0, 56);
  display.print(F("State: "));
  if (!sensorOk)            display.print(F("SENSOR ERR"));
  else if (st == ST_HOT)    display.print(F("TOO HOT!"));
  else if (st == ST_VENT)   display.print(F("VENTING"));
  else                      display.print(F("NORMAL"));

  display.display();
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);

  dht.begin();
  ventServo.attach(SERVO_PIN);
  ventServo.write(VENT_CLOSED);            // 上电先关闭通风窗

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED init failed"));
    while (true);                          // OLED 初始化失败则停机
  }
  display.cp437(true);                     // 使用标准 CP437 字符表（° = 248）
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 16);
  display.print(F("Greenhouse"));
  display.setTextSize(1);
  display.setCursor(31, 44);
  display.print(F("Starting..."));
  display.display();
  delay(1000);

  readSensor();                            // 先读一次，避免开机前 2 s 无数据
  lastDhtTime = millis();
}

void loop() {
  unsigned long now = millis();

  // 1. 按键：切换手动 / 自动模式
  if (buttonPressed()) {
    mode = (mode == MODE_AUTO) ? MODE_MANUAL : MODE_AUTO;
    if (mode == MODE_AUTO) autoVentOpen = (servoAngle > VENT_MIN_OPEN);  // 切回自动时沿用当前开关状态
    Serial.println(mode == MODE_AUTO ? F("Mode -> AUTO") : F("Mode -> MANUAL"));
  }

  // 2. 每 2 s 读取一次温湿度
  if (now - lastDhtTime >= DHT_INTERVAL) {
    lastDhtTime = now;
    readSensor();
    Serial.print(F("T=")); Serial.print(temperature);
    Serial.print(F("C  H=")); Serial.print(humidity);
    Serial.print(F("%  Vent=")); Serial.println(servoAngle);
  }

  // 3. 控制通风窗，4. 更新 RGB 状态灯
  updateVent();
  Status st = getStatus();
  updateLed(st);

  // 5. 每 200 ms 刷新一次 OLED
  if (now - lastOledTime >= OLED_INTERVAL) {
    lastOledTime = now;
    updateDisplay(st);
  }
}
