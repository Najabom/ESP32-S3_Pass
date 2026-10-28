# tools/gen_sd_fonts.py - SD 卡字库生成脚本（手动运行，不参与构建）
#
# 生成 GB2312 全集的 LVGL bin 字库（lv_font_conv --format bin，RLE 压缩，
# bpp4），拷贝到 SD 卡 /fonts/ 目录后由固件在挂载时一次性载入 PSRAM
# （见 src/ui/sd_fonts.c）。GB2312 全集 = 6763 个汉字 + 682 个全角符号，
# 中文使用场景的"实用全量"，两档字号总 PSRAM 占用约 2~3MB。
#
# 用法：
#   python tools/gen_sd_fonts.py
# 输出（把这两个文件拷到 SD 卡 /fonts/ 下即可）：
#   assets/sd_fonts/role_name_30.bin   30px：底部面板角色名 + 横幅首选
#   assets/sd_fonts/role_name_16.bin   16px：横幅长名回退档（160px 约放 9 汉字）
#
# 依赖：node + npm 全局 lv_font_conv（直接调 node 入口，绕过 npx.cmd：
#       --symbols 含 &|<>" 等 cmd 元字符且约 7.5K 字符逼近 8191 上限，
#       经批处理转发会被截断/解析炸）。

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "assets" / "sd_fonts"
HAN_FILE = ROOT / "fonts" / "SourceHanSansSC-Medium.otf"

SIZES = (30, 16)   # 与 src/ui/sd_fonts.h 的 SD_FONT_OP_LARGE/SMALL 对应


def find_conv_cmd():
    """lv_font_conv 调用前缀：优先 [node, 全局包入口]；回退 [npx, lv_font_conv]

    注意 Windows 上 subprocess(["npm", ...]) 解析不到 npm.cmd，必须
    用 shutil.which 拿到带扩展名的完整路径。"""
    node = shutil.which("node")
    if node:
        roots = []
        npm = shutil.which("npm") or shutil.which("npm.cmd")
        if npm:
            try:
                p = subprocess.run([npm, "root", "-g"],
                                   capture_output=True, text=True, check=True)
                roots.append(Path(p.stdout.strip()))
            except Exception:
                pass
        appdata = os.environ.get("APPDATA")
        if appdata:
            roots.append(Path(appdata) / "npm" / "node_modules")
        for root in roots:
            js = root / "lv_font_conv" / "lv_font_conv.js"
            if js.exists():
                return [node, str(js)]
    nvx = shutil.which("npx") or shutil.which("npx.cmd")
    if nvx:
        print("gen_sd_fonts: warn: 未找到全局 lv_font_conv，回退 npx"
              "（cmd 元字符/长度可能出问题）")
        return [nvx, "lv_font_conv"]
    raise SystemExit("gen_sd_fonts.py: node/lv_font_conv not found")


CONV_CMD = find_conv_cmd()


def gb2312_symbols():
    """GB2312 全集字符：高低字节合法区逐对 decode，跳过无效组合。

    高字节 0xA1-0xAF：全角符号区（标点/序号/假名/希腊/俄文/制表等）；
    高字节 0xB0-0xF7：6763 个汉字（一级+二级）。另补 ASCII 可打印字符。
    """
    chars = set(chr(c) for c in range(0x20, 0x7F))
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass   # 该区位的空洞（未分配字符）
    return "".join(sorted(chars))


# GB2312 未覆盖但中文输入法常用（GBK 扩展区/Unicode 扩展）的标点：
# 中文单双引号 ’“”、省略号 …、破折号 —、间隔号 ·、全角波浪～
# （lv_font_conv --symbols 上限 8191，每次仅新增少量安全）
EXTRA_SYMBOLS = "‘’“”…—·～"


def conv_bin(size, symbols):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = OUT_DIR / ("role_name_%d.bin" % size)
    if out.exists():
        out.unlink()
    cmd = CONV_CMD + [
           "--font", HAN_FILE.relative_to(ROOT).as_posix(),
           "--size", str(size),
           "--bpp", "4",
           "--symbols", symbols,
           "--format", "bin",
           "-o", str(out)]
    p = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stdout or "")
        sys.stderr.write(p.stderr or "")
        raise SystemExit("gen_sd_fonts.py: lv_font_conv failed for %dpx" % size)
    print("gen_sd_fonts: %s (%d bytes)" % (out.name, out.stat().st_size))


def main():
    symbols = "".join(sorted(set(gb2312_symbols()) | set(EXTRA_SYMBOLS)))
    print("gen_sd_fonts: %d glyphs (GB2312 full set + ASCII + %d EXTRA)" % (len(symbols), len(EXTRA_SYMBOLS)))
    for size in SIZES:
        conv_bin(size, symbols)
    print("gen_sd_fonts: 拷贝 %s 下的 .bin 到 SD 卡 /config/fonts/ 目录" % OUT_DIR)


main()
