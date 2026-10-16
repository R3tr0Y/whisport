#!/usr/bin/env bash
# 生成中文字形子集字体（whisport_cjk_14.c）——LVGL 14px，4bpp，字频前 N 字
#
# 产物：firmware/main/whisport_cjk_14.c（约 1.2MB 源码 / ~340KB 二进制数据）
# 在 ui.c 中经 montserrat_14 的 RAM 副本 .fallback 链挂载（勿直接写 const 字体，会 Store access fault）
#
# 依赖：node/npx（lv_font_conv）、python3（wordfreq，装进临时 venv）
# 字体：Noto Sans SC Regular（SIL OFL 许可，可随固件分发）
set -euo pipefail

CHARS=${1:-3500}                     # 采用字频前 N 个汉字
OUT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FONT_OTF=/tmp/NotoSansSC-Regular.otf
VENV=/tmp/fqenv

if [ ! -f "$FONT_OTF" ]; then
  echo "下载 Noto Sans SC (OFL)..."
  curl -sSL -o "$FONT_OTF" \
    "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf"
fi

if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV" && "$VENV/bin/pip" install -q wordfreq
fi

"$VENV/bin/python" - "$CHARS" > /tmp/sym.txt <<'PY'
import sys
from wordfreq import top_n_list
n = int(sys.argv[1])
freq = [w for w in top_n_list('zh', 200000) if len(w) == 1 and 0x4E00 <= ord(w) <= 0x9FFF][:n]
extra = ''.join(chr(c) for c in list(range(0x20, 0x7F)) + [0xB7] +
                list(range(0x2000, 0x2070)) + list(range(0x3000, 0x3040)) +
                list(range(0xFF01, 0xFF5F)))
sys.stdout.write(extra + ''.join(freq))
PY

npx --yes lv_font_conv@1.5.2 --font "$FONT_OTF" --size 14 --bpp 4 --format lvgl \
    --symbols "$(cat /tmp/sym.txt)" -o "$OUT_DIR/main/whisport_cjk_14.c"

# 修正 LVGL 头路径（lv_font_conv 生成的是 lvgl/lvgl.h，本工程用 lvgl.h）
sed -i 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' "$OUT_DIR/main/whisport_cjk_14.c"
echo "已生成: $OUT_DIR/main/whisport_cjk_14.c（$CHARS 字）"
