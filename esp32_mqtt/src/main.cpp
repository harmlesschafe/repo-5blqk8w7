/*
 * 第七次课 ESP32 与 MQTT 双向通信练习
 *  - GPIO34 电位器 -> 每 1 s 向 .../pot 发布 JSON {"raw":xxxx,"percent":xx}（QoS 0，Retain false）
 *  - 订阅 .../rgb/set，接收 RED / GREEN / BLUE / WHITE / OFF 控制共阴极 RGB LED
 *  - 选做：.../status 主题，连接后发布保留消息 online，遗嘱为保留消息 offline
 *
 * 使用前只需把 STUDENT_ID 改为本人完整学号。
 */
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "rgb_command.h"

// ---------------- 个人化配置 ----------------
#define STUDENT_ID "20260101"  // TODO: 替换为本人完整学号

#define TOPIC_ROOT "iot/lesson7/esp32/" STUDENT_ID
const char *TOPIC_POT    = TOPIC_ROOT "/pot";
const char *TOPIC_RGB    = TOPIC_ROOT "/rgb/set";
const char *TOPIC_STATUS = TOPIC_ROOT "/status";   // 选做任务
const char *DEVICE_ID    = "esp32-" STUDENT_ID;     // MQTTX 使用 mqttx-学号，二者不同

// 第五部分故障演示开关：0 = 正常；1 = 订阅 Topic 少写一个层级（故障：Topic 写错一个层级）
#define FAULT_DEMO 0

// ---------------- 网络配置 ----------------
const char *WIFI_SSID     = "Wokwi-GUEST";
const char *WIFI_PASSWORD = "";
const char *MQTT_BROKER   = "broker.emqx.io";
const uint16_t MQTT_PORT  = 1883;

// ---------------- 引脚 ----------------
const int PIN_POT = 34;
const int PIN_R   = 25;
const int PIN_G   = 26;
const int PIN_B   = 27;

const unsigned long PUBLISH_INTERVAL_MS = 1000;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
RgbState rgb = {false, false, false};
unsigned long lastPublish = 0;

void applyRgb(const RgbState &s) {
  // 共阴极：HIGH 点亮
  digitalWrite(PIN_R, s.r ? HIGH : LOW);
  digitalWrite(PIN_G, s.g ? HIGH : LOW);
  digitalWrite(PIN_B, s.b ? HIGH : LOW);
}

void onMessage(char *topic, byte *payload, unsigned int length) {
  // 必须按 length 读取 payload，不能当作 C 字符串直接使用
  char cmd[32];
  normalizeCommand(payload, length, cmd, sizeof(cmd));

  Serial.printf("Message arrived [%s] (%u bytes): %.*s\n", topic, length, (int)length, (const char *)payload);

  if (strcmp(topic, TOPIC_RGB) != 0) {
    Serial.println("Ignored: topic mismatch");
    return;
  }

  RgbState next = rgb;
  if (parseRgbCommand(cmd, next)) {
    rgb = next;
    applyRgb(rgb);
    Serial.printf("Command accepted: %s -> R=%d G=%d B=%d\n", cmd, rgb.r, rgb.g, rgb.b);
  } else {
    // 未知命令（如 YELLOW 或中文）：保持原灯光状态
    Serial.printf("Command rejected: \"%.*s\" (state unchanged R=%d G=%d B=%d)\n",
                  (int)length, (const char *)payload, rgb.r, rgb.g, rgb.b);
  }
}

void connectWiFi() {
  Serial.printf("Connecting to WiFi %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
}

void connectMqtt() {
  while (!mqtt.connected()) {
    Serial.printf("Connecting to MQTT %s:%u as %s ... ", MQTT_BROKER, MQTT_PORT, DEVICE_ID);
    // 选做：遗嘱 offline（QoS 0，Retain true）
    if (mqtt.connect(DEVICE_ID, nullptr, nullptr, TOPIC_STATUS, 0, true, "offline")) {
      Serial.println("MQTT connected");
      mqtt.publish(TOPIC_STATUS, "online", true);
      Serial.printf("Status published: %s = online (retain)\n", TOPIC_STATUS);

      // 每次（重新）连接成功后都必须重新订阅
#if FAULT_DEMO == 1
      const char *subTopic = "iot/lesson7/" STUDENT_ID "/rgb/set";  // 故障：缺少 esp32 层级
#else
      const char *subTopic = TOPIC_RGB;
#endif
      bool ok = mqtt.subscribe(subTopic, 0);
      Serial.printf("Subscribe %s (QoS 0): %s\n", subTopic, ok ? "OK" : "FAILED");
    } else {
      Serial.printf("failed, rc=%d, retry in 2 s\n", mqtt.state());
      delay(2000);
    }
  }
}

void publishPot() {
  int raw = analogRead(PIN_POT);
  int percent = map(raw, 0, 4095, 0, 100);
  char json[48];
  snprintf(json, sizeof(json), "{\"raw\":%d,\"percent\":%d}", raw, percent);
  // QoS 0（PubSubClient 发布固定为 QoS 0），Retain false
  bool ok = mqtt.publish(TOPIC_POT, json, false);
  Serial.printf("%s %s -> %s\n", ok ? "Publish sent (QoS 0)" : "Publish FAILED", TOPIC_POT, json);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  applyRgb(rgb);
  analogReadResolution(12);

  Serial.println();
  Serial.printf("Device ID : %s\n", DEVICE_ID);
  Serial.printf("Pot topic : %s\n", TOPIC_POT);
  Serial.printf("RGB topic : %s\n", TOPIC_RGB);

  connectWiFi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMessage);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL_MS) {
    lastPublish = now;
    publishPot();
  }
}
