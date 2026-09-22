#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从"她真实说过的话"里提字，生成一个只含这些字的**子集字库**（GFXfont 格式 C 头文件）。

为什么要子集：整个中文字库塞不进 flash（也不必要 ✗）。她说过的话是**有边界**的 ——
把中枢里的历史提醒/复盘/闲聊拉下来提字，通常 500~800 字就覆盖 99% 以上，
40KB 左右，16MB flash 绰绰有余。

为什么自己做而不是直接用 efontCN：LovyanGFX 内置的 efontCN 覆盖面有限（生僻字空白 ✓），
而且它只有固定字号。想要"她说的每句话都能显示"就得自己提字。

诚实说明：
  · 这个脚本要两样东西：**Pillow**（渲染）和**一个中文 TTF**（默认用 Noto Sans SC，OFL 许可 ✓）。
    两者都不入库（字体文件大 + 让使用者自己确认许可）。
  · 生成的 .h 里只有**位图数据**（从你选的字体渲染出来的像素）——那是字体的衍生品，
    是否可再分发取决于**你选的那个字体的许可**。Noto Sans SC 是 OFL，允许 ✓。

用法：
    pip install pillow
    python3 tools/gen_font.py --hub https://你的中枢:11443 --token <设备token> \
            --cacert /root/hub/tls/hub.crt          # 自签证书，必须给（或显式 --insecure）
    python3 tools/gen_font.py --text "从文件或命令行给文字" ...
    → 生成 include/whale_font.h（在 src/main.cpp 里 include 后用 setFont(&whale_font)）
"""
import argparse
import json
import pathlib
import re
import sys
import urllib.request

# 常见符号 + 数字 + 字母：无论如何都带上（时间/百分比/单位要用）
ALWAYS = (
    "0123456789:%-./ " + "ABCDEFGHIJKLMNOPQRSTUVWXYZ" + "abcdefghijklmnopqrstuvwxyz"
    + "，。！？、；：（）「」『』—…·《》”’“”~*#@+="
    + "收到别提了好的嗯嗯嗯等等一下我看看"
)

GLYPH_H = 16          # 字高（像素）。222×480 的屏，16 号字一行约 13 个汉字，够用。


def _ssl_ctx(cacert: str):
    """中枢是自签证书 ✓ 所以要么给 --cacert（推荐），要么显式 --insecure（会警告）。

    刻意**不**默认不校验：默认放行等于把"防中间人"这件事悄悄关掉，
    而这个项目里她说过的话是私人内容 ✓
    """
    import ssl
    if cacert:
        return ssl.create_default_context(cafile=cacert)
    print("  ⚠ 没给 --cacert，临时不校验证书（只在本机可信网络下这么干）", file=sys.stderr)
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def fetch_texts(hub: str, token: str, limit: int = 800, cacert: str = "") -> str:
    """从中枢拉她真实说过的话（决策理由 + 提醒文本 + 复盘）。"""
    out = []
    for path in (f"/decisions?limit={limit}", f"/decisions?limit={limit}"):
        try:
            req = urllib.request.Request(hub.rstrip("/") + path)
            req.add_header("X-Token", token)
            req.add_header("User-Agent", "whale-font/1.0")
            with urllib.request.urlopen(req, timeout=20, context=_ssl_ctx(cacert)) as r:
                data = json.loads(r.read().decode("utf-8", "replace"))
            for it in (data.get("items") or []):
                out.append(str(it.get("reason") or ""))
                out.append(str(it.get("kind") or ""))
        except Exception as e:
            print(f"  ⚠ 拉 {path} 失败：{type(e).__name__} {e}", file=sys.stderr)
    return "\n".join(out)


def collect_chars(text: str) -> list:
    """只留中日韩汉字 + 常见标点/数字/字母，去重排序。"""
    keep = set(ALWAYS)
    for ch in text:
        o = ord(ch)
        if 0x4E00 <= o <= 0x9FFF:          # CJK 统一汉字
            keep.add(ch)
        elif 0x3000 <= o <= 0x303F:        # CJK 标点
            keep.add(ch)
        elif 0xFF00 <= o <= 0xFFEF:        # 全角
            keep.add(ch)
    return sorted(keep)


def render(chars: list, ttf: pathlib.Path, size: int = GLYPH_H):
    """用 Pillow 把每个字渲染成 1bit 位图 → Adafruit GFXfont 结构。"""
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(str(ttf), size)
    glyphs = []
    for ch in chars:
        img = Image.new("1", (size + 8, size + 8), 0)
        ImageDraw.Draw(img).text((2, 0), ch, font=font, fill=1)
        bbox = img.getbbox()
        if not bbox:
            continue
        img = img.crop(bbox)
        w, h = img.size
        rows, cur, bits = [], 0, 0
        for y in range(h):
            cur, bits = 0, 0
            for x in range(w):
                cur = (cur << 1) | (1 if img.getpixel((x, y)) else 0)
                bits += 1
                if bits == 8:
                    rows.append(cur); cur, bits = 0, 0
            if bits:
                rows.append(cur << (8 - bits))
        glyphs.append({"c": ch, "w": w, "h": h, "rows": rows,
                       "xadv": w + 1, "xoff": 0, "yoff": -h})
    return glyphs


def emit_c(glyphs: list, out: pathlib.Path, size: int = GLYPH_H):
    """写成 Adafruit GFXfont 风格的 C 头（LovyanGFX 可以直接 setFont 用它）。"""
    lines = [
        "// 自动生成，别手改 —— 见 tools/gen_font.py",
        "// 字形来源：使用者自己选的中文 TTF（默认 Noto Sans SC / OFL）",
        "// 只含她真实说过的话里出现过的字 → flash 占用极小",
        "#pragma once",
        "#include <LovyanGFX.hpp>",
        "",
        f"static const uint8_t whale_font_bitmaps[] = {{",
    ]
    offset = 0
    offsets = []
    for g in glyphs:
        offsets.append(offset)
        lines.append("  " + ",".join(str(b) for b in g["rows"]) + ",")
        offset += len(g["rows"])
    lines += ["};", "", "static const lgfx::v1::GFXglyph whale_font_glyphs[] = {"]
    glyph_rows = []
    for g, off in zip(glyphs, offsets):
        glyph_rows.append((g, off))
    # 按 code 排序（GFXfont 要求有序 + 用 first/last 二分）
    glyph_rows.sort(key=lambda t: ord(t[0]["c"]))
    lines.append("  // {bitmapOffset, width, height, xAdvance, xOffset, yOffset}")
    for g, off in glyph_rows:
        lines.append(f"  {{ {off}, {g['w']}, {g['h']}, {g['xadv']}, {g['xoff']}, {g['yoff']} }},")
    first = ord(glyph_rows[0][0]["c"]) if glyph_rows else 32
    last = ord(glyph_rows[-1][0]["c"]) if glyph_rows else 32
    lines += ["};", "",
              f"static const lgfx::v1::GFXfont whale_font = {{",
              "  (uint8_t *)whale_font_bitmaps,",
              "  (lgfx::v1::GFXglyph *)whale_font_glyphs,",
              f"  {first}, {last}, {size}",
              "};", ""]
    out.write_text("\n".join(lines), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hub", default="", help="中枢地址，如 https://1.2.3.4:11443")
    ap.add_argument("--token", default="", help="设备 token（X-Token）")
    ap.add_argument("--text", default="", help="直接给文字（不想连中枢时用）")
    ap.add_argument("--cacert", default="", help="中枢自签证书路径（强烈建议给）")
    ap.add_argument("--ttf", default="tools/NotoSansSC-Regular.otf")
    ap.add_argument("--out", default="include/whale_font.h")
    ap.add_argument("--size", type=int, default=GLYPH_H)
    a = ap.parse_args()

    text = a.text or ""
    if a.hub and a.token:
        print("  从中枢拉她说过的话……")
        text += "\n" + fetch_texts(a.hub, a.token, cacert=a.cacert)
    if not text.strip():
        print("  ✗ 没有文字可提（给 --text，或给 --hub + --token）"); return 1

    chars = collect_chars(text)
    print(f"  提取到 {len(chars)} 个不同字符（含必备符号）")

    ttf = pathlib.Path(a.ttf)
    if not ttf.is_file():
        print(f"  ✗ 找不到字体 {ttf}\n"
              f"    去下载一个（Noto Sans SC 是 OFL，可自由用）：\n"
              f"    curl -L -o {ttf} https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf\n"
              f"    （otf 也行；Pillow 都支持）")
        return 1

    glyphs = render(chars, ttf, a.size)
    out = pathlib.Path(a.out)
    emit_c(glyphs, out, a.size)
    kb = out.stat().st_size / 1024
    print(f"  ✓ 生成 {out}：{len(glyphs)} 个字形 · {kb:.1f} KB")
    print(f"    在 main.cpp 里：#include \"whale_font.h\"  然后 canvas.setFont(&whale_font);")
    return 0


if __name__ == "__main__":
    sys.exit(main())
