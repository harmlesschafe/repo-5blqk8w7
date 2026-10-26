#!/usr/bin/env bash
# 用法：./personalize.sh 学号 姓名 班级
# 把工程中的示例学号 20260101 替换为本人学号，并重命名提交目录、生成压缩包。
set -euo pipefail
ID="$1"; NAME="$2"; CLASS="$3"
cd "$(dirname "$0")"
grep -rl 20260101 esp32_mqtt/src README.md screenshots | xargs sed -i "s/20260101/${ID}/g"
OUT="${CLASS}_${ID}_${NAME}_第七次课MQTT练习"
rm -rf "../${OUT}" && mkdir "../${OUT}"
cp -r esp32_mqtt build_logs screenshots README.md "../${OUT}/"
rm -rf "../${OUT}/esp32_mqtt/.pio"
(cd .. && zip -qr "${ID}-${NAME}-第七次课练习.zip" "${OUT}")
echo "生成：../${ID}-${NAME}-第七次课练习.zip"
