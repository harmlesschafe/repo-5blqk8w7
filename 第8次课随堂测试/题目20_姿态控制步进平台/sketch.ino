/*
 * 题目20：姿态控制步进平台
 * 平台：Arduino Uno（Wokwi 仿真）
 * 硬件：MPU6050 六轴传感器、42 步进电机（200 步/圈）+ A4988 驱动、
 *       按键、无源蜂鸣器、0.96 寸 OLED（SSD1306，128x64，I2C）
 *
 * 功能：
 *  1. MPU6050 检测 X 轴倾斜方向，控制步进电机正转 / 反转 / 停止；
 *  2. 倾斜角越大转速越快，±5° 以内为死区，电机不动，防止误动作；
 *  3. 按键控制系统启停，停止状态下步进电机不转动（不发脉冲 + 关闭驱动）；
 *  4. OLED 显示倾斜角、转动方向、转速和运行状态，角度过大时蜂鸣器报警。
 */
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- 引脚定义 ----------------
const uint8_t DIR_PIN    = 3;    // A4988 DIR：高电平顺时针，低电平逆时针
const uint8_t STEP_PIN   = 4;    // A4988 STEP：每个上升沿走一步
const uint8_t EN_PIN     = 5;    // A4988 ENABLE：低电平使能，高电平关闭驱动
const uint8_t BUZZER_PIN = 7;    // 无源蜂鸣器正极（负极接 GND）
const uint8_t BUTTON_PIN = 12;   // 启停按键，另一端接 GND（使用内部上拉）
// MPU6050 与 OLED 共用 I2C 总线：SDA -> A4，SCL -> A5
const uint8_t MPU_ADDR   = 0x68; // MPU6050 地址（AD0 接地）
const uint8_t OLED_ADDR  = 0x3C; // OLED 地址

// ---------------- 参数设置 ----------------
const float DEAD_ZONE   = 5.0;   // 死区：|角度| < 5° 时电机不转
const float MAX_ANGLE   = 45.0;  // 角度达到 45° 时转速达到最大
const float ALARM_ANGLE = 45.0;  // |角度| > 45° 时蜂鸣器报警
const unsigned int MIN_SPEED = 50;   // 最低转速（步/秒）= 15 rpm
const unsigned int MAX_SPEED = 800;  // 最高转速（步/秒）= 240 rpm
const unsigned int STEPS_PER_REV = 200;  // 1.8° 步进电机，整步模式 200 步/圈

const float FILTER_K = 0.3;               // 角度一阶低通滤波系数（越小越平滑）
const unsigned long MPU_INTERVAL  = 20;   // 姿态采样周期 20 ms
const unsigned long OLED_INTERVAL = 200;  // OLED 刷新周期 200 ms
const unsigned long BEEP_INTERVAL = 200;  // 报警时蜂鸣器 200 ms 响 / 200 ms 停
const unsigned long DEBOUNCE_MS   = 50;   // 按键消抖时间

// ---------------- 全局变量 ----------------
Adafruit_SSD1306 display(128, 64, &Wire, -1);

bool  running   = false;   // 系统运行状态，上电默认停止，按键启动
float angleX    = 0;       // 滤波后的 X 轴倾斜角（°）
bool  mpuOk     = false;
int8_t direction = 0;      // 1 = 顺时针 CW，-1 = 逆时针 CCW，0 = 停止
unsigned int speedSps = 0; // 当前转速（步/秒）
bool  alarmOn   = false;

unsigned long lastMpuTime = 0, lastOledTime = 0, lastBeepTime = 0;
bool beepState = false;

// ================= 步进脉冲：定时器1 中断 =================
// 用定时器1产生步进脉冲，OLED 刷新等耗时操作不会让电机卡顿。
// 定时器1：CTC 模式，64 分频 -> 计数频率 16MHz / 64 = 250 kHz
void stepTimerInit() {
  noInterrupts();
  TCCR1A = 0;
  TCCR1B = _BV(WGM12) | _BV(CS11) | _BV(CS10);  // CTC 模式，64 分频
  TCNT1  = 0;
  OCR1A  = 4999;
  TIMSK1 = 0;                                    // 先不开中断
  interrupts();
}

// 设置步进频率（步/秒），0 表示停止发脉冲
void setStepRate(unsigned int sps) {
  static unsigned int current = 0xFFFF;
  if (sps == current) return;                    // 转速没变就不重复设置
  current = sps;
  if (sps == 0) {
    TIMSK1 &= ~_BV(OCIE1A);                      // 关闭比较中断 -> 不再产生脉冲
    return;
  }
  uint16_t top = 250000UL / sps - 1;             // 例：800 步/秒 -> OCR1A = 311
  noInterrupts();
  OCR1A = top;
  if (TCNT1 > top) TCNT1 = 0;                    // 防止计数器已越过新比较值而多等一整轮
  TIMSK1 |= _BV(OCIE1A);
  interrupts();
}

// 比较匹配中断：输出一个 STEP 脉冲（A4988 要求高电平 ≥1 µs）
ISR(TIMER1_COMPA_vect) {
  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(2);
  digitalWrite(STEP_PIN, LOW);
}

// ================= MPU6050 =================
void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void mpuInit() {
  mpuWrite(0x6B, 0x00);   // PWR_MGMT_1：清除休眠位，唤醒传感器
  mpuWrite(0x1C, 0x08);   // ACCEL_CONFIG：量程 ±4g，灵敏度 8192 LSB/g
}

int16_t readWord() {      // 先读高字节，再读低字节
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  return (int16_t)(((uint16_t)hi << 8) | lo);
}

// 读取加速度并计算 X 轴倾斜角，成功返回 true
bool readTilt(float &angle) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);                              // ACCEL_XOUT_H 起始地址
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)6) != 6) return false;
  float ax = readWord() / 8192.0;                // 单位：g
  float ay = readWord() / 8192.0;
  float az = readWord() / 8192.0;
  // X 轴与水平面的夹角：X 轴抬起为正，压低为负，范围 -90°~+90°
  angle = atan2(ax, sqrt(ay * ay + az * az)) * 180.0 / PI;
  return true;
}

// ================= 按键检测（消抖 + 下降沿） =================
bool buttonPressed() {
  static bool stableState = HIGH;
  static bool lastReading = HIGH;
  static unsigned long lastChange = 0;

  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) {          // 电平变化（可能是抖动），重新计时
    lastReading = reading;
    lastChange = millis();
  }
  if (millis() - lastChange > DEBOUNCE_MS && reading != stableState) {
    stableState = reading;               // 稳定超过 50 ms 才确认
    return stableState == LOW;           // 由高变低 = 按下
  }
  return false;
}

// ================= 控制逻辑 =================
// 根据角度计算转速：死区内为 0，5°~45° 线性对应 MIN_SPEED~MAX_SPEED
unsigned int calcSpeed(float absAngle) {
  if (absAngle < DEAD_ZONE) return 0;
  if (absAngle > MAX_ANGLE) absAngle = MAX_ANGLE;
  return MIN_SPEED + (unsigned int)((absAngle - DEAD_ZONE) * (MAX_SPEED - MIN_SPEED)
                                    / (MAX_ANGLE - DEAD_ZONE));
}

void updateMotor() {
  int newDir = 0;
  unsigned int newSpeed = 0;

  if (running && mpuOk) {
    newSpeed = calcSpeed(fabs(angleX));
    if (newSpeed > 0) newDir = (angleX > 0) ? 1 : -1;   // 正角度正转，负角度反转
  }

  if (newDir != 0 && newDir != direction) {
    digitalWrite(DIR_PIN, newDir > 0 ? HIGH : LOW);     // 先改方向再发脉冲
  }
  direction = newDir;
  speedSps  = newSpeed;
  setStepRate(speedSps);
}

void updateBuzzer() {
  alarmOn = mpuOk && fabs(angleX) > ALARM_ANGLE;
  if (!alarmOn) {
    if (beepState) { noTone(BUZZER_PIN); beepState = false; }
    return;
  }
  // 报警：200 ms 响、200 ms 停的间歇声（用 millis 计时，不阻塞）
  if (millis() - lastBeepTime >= BEEP_INTERVAL) {
    lastBeepTime = millis();
    beepState = !beepState;
    if (beepState) tone(BUZZER_PIN, 2000);
    else           noTone(BUZZER_PIN);
  }
}

// ================= OLED 显示 =================
void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // 第 1 行：标题 + 右上角反色运行状态
  display.setCursor(0, 1);
  display.print(F("TILT PLATFORM"));
  const char *tag = running ? "RUN" : "STOP";
  int tagW = strlen(tag) * 6 + 4;
  display.fillRect(128 - tagW, 0, tagW, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(128 - tagW + 2, 1);
  display.print(tag);
  display.setTextColor(SSD1306_WHITE);
  display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

  // 倾斜角
  display.setCursor(0, 16);
  display.print(F("Angle: "));
  if (!mpuOk) {
    display.print(F("MPU ERR"));
  } else {
    if (angleX >= 0) display.print('+');
    display.print(angleX, 1);
    display.write(248);                  // ° 符号（CP437 编码）
  }

  // 转动方向
  display.setCursor(0, 26);
  display.print(F("Dir  : "));
  if (!running)            display.print(F("--"));
  else if (direction > 0)  display.print(F("CW  >>"));
  else if (direction < 0)  display.print(F("CCW <<"));
  else                     display.print(F("IN DEAD ZONE"));

  // 转速：步/秒换算为 rpm
  display.setCursor(0, 36);
  display.print(F("Speed: "));
  display.print((unsigned long)speedSps * 60 / STEPS_PER_REV);
  display.print(F(" rpm"));

  // 倾斜指示条：中线为 0°，左右两端为 ±60°，两条短竖线标出死区
  const int barY = 46, barH = 7, cx = 64;
  display.drawRect(0, barY, 128, barH, SSD1306_WHITE);
  int dz = (int)(DEAD_ZONE * 63 / 60);
  display.drawFastVLine(cx - dz, barY - 2, barH + 4, SSD1306_WHITE);
  display.drawFastVLine(cx + dz, barY - 2, barH + 4, SSD1306_WHITE);
  float a = constrain(angleX, -60.0, 60.0);
  int len = (int)(a * 63 / 60);
  if (len > 0) display.fillRect(cx, barY + 2, len, barH - 4, SSD1306_WHITE);
  if (len < 0) display.fillRect(cx + len, barY + 2, -len, barH - 4, SSD1306_WHITE);

  // 最后一行：报警 / 正常
  if (alarmOn) {
    display.fillRect(0, 55, 128, 9, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(13, 56);
    display.print(F("!! TILT ALARM !!"));
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.setCursor(0, 56);
    display.print(F("Status: "));
    display.print(running ? F("RUNNING") : F("STOPPED"));
  }

  display.display();
}

// ================= setup / loop =================
void setup() {
  Serial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(EN_PIN, HIGH);            // 上电为停止状态，先关闭驱动

  Wire.begin();
  mpuInit();
  stepTimerInit();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED init failed"));
    while (true);
  }
  display.cp437(true);                   // 使用标准 CP437 字符表（° = 248）
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 12);
  display.print(F("Tilt Step"));
  display.setCursor(16, 32);
  display.print(F("Platform"));
  display.display();
  delay(1000);
}

void loop() {
  unsigned long now = millis();

  // 1. 按键：启动 / 停止
  if (buttonPressed()) {
    running = !running;
    digitalWrite(EN_PIN, running ? LOW : HIGH);   // 停止时关闭驱动，电机不会被误驱动
    Serial.println(running ? F("System RUN") : F("System STOP"));
  }

  // 2. 每 20 ms 读取一次姿态，并做一阶低通滤波
  if (now - lastMpuTime >= MPU_INTERVAL) {
    lastMpuTime = now;
    float raw;
    mpuOk = readTilt(raw);
    if (mpuOk) angleX = angleX + FILTER_K * (raw - angleX);
  }

  // 3. 根据角度和运行状态控制电机，4. 角度过大报警
  updateMotor();
  updateBuzzer();

  // 5. 每 200 ms 刷新 OLED 并输出串口调试信息
  if (now - lastOledTime >= OLED_INTERVAL) {
    lastOledTime = now;
    updateDisplay();
    Serial.print(F("Angle=")); Serial.print(angleX, 1);
    Serial.print(F("  Dir=")); Serial.print((int)direction);
    Serial.print(F("  Speed=")); Serial.println(speedSps);
  }
}
