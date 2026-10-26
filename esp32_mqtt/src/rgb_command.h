#pragma once
// RGB 命令解析（纯逻辑，不依赖 Arduino，便于在电脑上单元测试）
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

struct RgbState {
  bool r;
  bool g;
  bool b;
};

// 按 length 读取 payload（payload 不保证以 '\0' 结尾），去掉首尾空白并转大写。
// 结果写入 out（容量 outSize），返回写入的字符数。
inline size_t normalizeCommand(const uint8_t *payload, size_t length, char *out, size_t outSize) {
  size_t start = 0, end = length;
  while (start < end && isspace(payload[start])) start++;
  while (end > start && isspace(payload[end - 1])) end--;
  size_t n = 0;
  for (size_t i = start; i < end && n + 1 < outSize; i++) {
    out[n++] = (char)toupper(payload[i]);
  }
  out[n] = '\0';
  return n;
}

// 合法命令返回 true 并写入 state；未知命令返回 false，state 保持不变。
inline bool parseRgbCommand(const char *cmd, RgbState &state) {
  if (strcmp(cmd, "RED") == 0)   { state = {true,  false, false}; return true; }
  if (strcmp(cmd, "GREEN") == 0) { state = {false, true,  false}; return true; }
  if (strcmp(cmd, "BLUE") == 0)  { state = {false, false, true }; return true; }
  if (strcmp(cmd, "WHITE") == 0) { state = {true,  true,  true }; return true; }
  if (strcmp(cmd, "OFF") == 0)   { state = {false, false, false}; return true; }
  return false;
}
