// 电脑端单元测试：g++ -std=c++17 -I../src host_test.cpp -o host_test && ./host_test
#include <cstdio>
#include "rgb_command.h"

static long map_(long x, long a, long b, long c, long d) { return (x - a) * (d - c) / (b - a) + c; }  // 同 Arduino map()

static int fails = 0;
static void check(const char *payload, bool expectOk, RgbState start, RgbState expect) {
  char cmd[32];
  RgbState s = start;
  normalizeCommand((const uint8_t *)payload, strlen(payload), cmd, sizeof(cmd));
  bool ok = parseRgbCommand(cmd, s);
  bool pass = ok == expectOk && s.r == expect.r && s.g == expect.g && s.b == expect.b;
  if (!pass) fails++;
  printf("%-4s %-10s -> %-9s R=%d G=%d B=%d\n", pass ? "PASS" : "FAIL", payload,
         ok ? "accepted" : "rejected", s.r, s.g, s.b);
}

int main() {
  RgbState off{false, false, false}, red{true, false, false};
  check("RED", true, off, {true, false, false});
  check("GREEN", true, off, {false, true, false});
  check("BLUE", true, off, {false, false, true});
  check("WHITE", true, off, {true, true, true});
  check("OFF", true, red, off);
  check("red", true, off, red);        // 大小写不敏感
  check("  Blue  ", true, off, {false, false, true});
  check("YELLOW", false, red, red);    // 未知命令保持原状态
  check("红色", false, red, red);
  check("", false, red, red);

  // 按 length 读取：payload 后面紧跟其他字节也不影响
  const char buf[] = "GREENXXXX";
  char cmd[32];
  RgbState s = off;
  normalizeCommand((const uint8_t *)buf, 5, cmd, sizeof(cmd));
  bool ok = parseRgbCommand(cmd, s) && s.g;
  printf("%-4s length=5 of \"GREENXXXX\" -> %s\n", ok ? "PASS" : "FAIL", cmd);
  if (!ok) fails++;

  int raws[] = {0, 2048, 4095};
  for (int raw : raws) printf("ADC raw=%4d -> {\"raw\":%d,\"percent\":%ld}\n", raw, raw, map_(raw, 0, 4095, 0, 100));

  printf(fails ? "\n%d FAILED\n" : "\nALL TESTS PASSED\n", fails);
  return fails != 0;
}
