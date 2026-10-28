# tools/gen_fonts.py - PlatformIO 预构建脚本（extra_scripts = pre:tools/gen_fonts.py）
# 自动生成 LVGL 位图字库（lv_font_conv，RLE 压缩）：
#   bebas_56.c / bebas_48.c / bebas_40.c     BebasNeue，全大写+标点符号集，
#                                            左立牌 "ROLE PASS" 字号阶梯
#   role_name_30/24/18/14.c                   Source Han Sans SC，角色名（ASCII 兜底）
#   uname_18/14.c                           Source Han Sans SC，用户名（ASCII）
#   hint_24.c                               Source Han Sans SC，开机 SD 状态提示
#                                           （"请插入SD卡"/"SD卡无内容"，仅提示字）
#
# 角色名已迁移到 SD 卡（文件夹名），中文字形由 SD 字库提供
#（tools/gen_sd_fonts.py 生成 /sdcard/fonts/role_name_{30,16}.bin，挂载时载入
# PSRAM）；内嵌 role_name_* 只作 SD 缺失时的 ASCII 兜底，故字符集固定为
# ASCII 可打印字符，不再解析 roles.c（roles.c 已无静态角色表）。
#
# 可调整内容：
#   - BEBAS_SIZES = (56, 48, 40)：左立牌字号阶梯。运行时代码
#     (ui_core_screen.c) 会实测文本宽度选最大可放入 LEFTCOL_MAX_W 的字号；
#     增删阶梯时两处必须同步（ui_core_screen.c 的 bebas_sizes[]）。
#   - LEFTCOL_TEXT："ROLE PASS" 立牌文本，改动后自动收录新字形。
#   - LEFTCOL_MAX_W 300：旋转后纵向可用像素（屏高 320 减去 y 偏移余量），
#     改小则可能选不到大字号，改大可能超出屏幕（运行时日志可查实测值）。
#   - ROLE_NAME_SIZES (30, 24, 18, 14)：名片横幅角色名字号阶梯（px，ASCII
#     兜底；SD 字库加载成功时改用 SD 的 30/16 两档，见 sd_fonts.c）。
#     横幅运行时按 160px 可用区实测文本宽度选号；底部面板角色名固定
#     用 30px 档。
#   - UNAME_SIZES (18, 14)：横幅用户名（对端可配置昵称）字号阶梯（px）。
#     用户名限 ASCII 字符集（见 UNAME_SYMBOLS），运行时同样按 160px
#     实测选号（18px 最多约 8 字符，更长自动落到 14px）。
#   - UID_SIZE 12：ID 数字的字号（px）。
#     字库是位图字体，字号变了要重新生成，运行时引用也需同步改。
#   - 内容未变时脚本跳过重写（"unchanged"），避免无谓的重编译。
#
# 历史：bebas_32 与 pixel_16（开机画面标题）随 ui_boot_anim 删除；
#       开机画面已改为无 LVGL 的裸终端（src/bsp/boot_term.c，用
#       LVGL 内置 UnSCII 8x16 数据渲染，不再需要独立像素字库）。

import os
import shutil
import subprocess
import sys
from pathlib import Path

Import("env")

ROOT = Path(env.subst("$PROJECT_DIR")).resolve()
OUT_DIR = ROOT / "src" / "ui" / "fonts"

# 字体源文件（TTF/OTF）路径；换字体文件后重新构建即生效
BEBAS_FILE = ROOT / "fonts" / "BebasNeue.otf"
HAN_FILE = ROOT / "fonts" / "SourceHanSansSC-Medium.otf"
LEFTCOL_TEXT = "ROLE PASS"
LEFTCOL_MAX_W = 300        # 旋转后纵向可用空间（屏高 320，y 偏移 4）
# bebas 符号集：全 ASCII 可打印（leftcol 自定义文本可含 ' - ! ? 等英文标点）。
BEBAS_SYMBOLS = "".join(chr(c) for c in range(0x20, 0x7F))
BEBAS_SIZES = (56, 48, 40)  # 左立牌字号阶梯（运行时实测选号）
ROLE_NAME_SIZES = (30, 24, 18, 14)  # 角色名字号阶梯（见文件头注释）
UNAME_SIZES = (18, 14)        # 横幅用户名字号阶梯（见文件头注释）
# 用户名：全 ASCII 可打印（撇号/引号等英文标点也正常显示）+ 弯引号等
UNAME_SYMBOLS = "".join(chr(c) for c in range(0x20, 0x7F)) + "‘’“”…—·～"
UID_SIZE = 12          # 名片横幅 ID / 详情页 mac 显示（20 位数字 + hex 字母）
UID_SYMBOLS = "".join(chr(c) for c in range(0x20, 0x7F))   # 全 ASCII 可打印（mac 含 a-f）
HINT_SIZE = 24         # 开机 SD 状态提示（"请插入SD卡"/"SD卡无内容"/"SD卡无法识别"）字号
HINT_SYMBOLS = "请插入无内容SD卡法识别 "   # 提示所需全部字形（含 S/D/空格）

# ---- 触控菜单字库（ui_menu.c 用）----
# menu_20：菜单项/页标题/按钮；menu_16：列表行/正文/状态值。
# 字符集 = ASCII 可打印（数字/单位/标点等）+ 下方 CJK 全集。
# 注意：ui_menu.c 的硬编码文案只能使用本字符集内的字，加文案必须
#       同步加字到这里（pre 脚本自动重生成，改完即生效）。
MENU_SIZES = (20, 16)
MENU_CJK = ("角色选择角色名片簿蓝牙系统信息返回当前无加载失败暂无记录"
            "用户ID名字寄语时间序号表情包未连接已广播等待上传完成中"
            "空闲用字节总剩余版本运行日志级别调试警告错误SD状态挂载"
            "法识别存储芯片核心内存分区字库关闭打开切换手机电脑搜索本"
            "触控校准请依次点击屏幕四个角次上滑菜单或底边三次打开设备详")

# 中文弯引号等 GB2312 之外的常用符号（角色名/名片文本兜底字形），
# 与 gen_sd_fonts.py 的 EXTRA_SYMBOLS 保持一致。
EXTRA_SYMBOLS = "‘’“”…—·～"

NVX = shutil.which("npx") or shutil.which("npx.cmd")


def find_conv_cmd():
    """lv_font_conv 调用前缀：优先直接 [node, 全局包入口]，绕过 npx.cmd
    （role_name 字符集是 ASCII 可打印，含 &|<>" 等 cmd 元字符，经批处理
    转发会被错误解析）；找不到全局包才回退 npx。

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
    if not NVX:
        raise SystemExit("gen_fonts.py: node/lv_font_conv not found")
    return [NVX, "lv_font_conv"]


CONV_CMD = find_conv_cmd()


def conv(font_path, size, symbols, out_name):
    """调 lv_font_conv 生成单个字库：写临时文件→对比内容→有变化才替换。
    注意：--lv-font-name 生成的 C 符号就是裸名（如 bebas_56），
    代码里 LV_FONT_DECLARE(bebas_56) 与之对应。"""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    tmp = OUT_DIR / (out_name + ".c.tmp")
    final = OUT_DIR / (out_name + ".c")
    rel_font = font_path.relative_to(ROOT)
    rel_tmp = tmp.relative_to(ROOT)
    cmd = CONV_CMD + [
           "--font", rel_font.as_posix(),
           "--size", str(size),
           "--bpp", "4",
           "--symbols", symbols,
           "--format", "lvgl",
           "--lv-include", "lvgl.h",
           "--lv-font-name", out_name,
           "-o", rel_tmp.as_posix()]
    p = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stdout or "")
        sys.stderr.write(p.stderr or "")
        raise SystemExit("gen_fonts.py: lv_font_conv failed for %s" % out_name)
    if final.exists() and final.read_bytes() == tmp.read_bytes():
        tmp.unlink()
        print("gen_fonts: %s.c unchanged" % out_name)
    else:
        tmp.replace(final)
        print("gen_fonts: %s.c updated (%d bytes)" % (out_name, final.stat().st_size))


def main():
    # role_name 兜底字符集：ASCII 可打印（0x20-0x7E）+ 弯引号等常用符号。
    # 中文靠 SD 字库；这些符号 GB2312 也没有，兜底必须有。
    ascii_symbols = "".join(chr(c) for c in range(0x20, 0x7F)) + EXTRA_SYMBOLS

    for size in BEBAS_SIZES:
        conv(BEBAS_FILE, size, BEBAS_SYMBOLS, "bebas_%d" % size)
    for size in ROLE_NAME_SIZES:
        conv(HAN_FILE, size, ascii_symbols, "role_name_%d" % size)
    for size in UNAME_SIZES:
        conv(HAN_FILE, size, UNAME_SYMBOLS, "uname_%d" % size)
    conv(HAN_FILE, UID_SIZE, UID_SYMBOLS, "uid_12")
    conv(HAN_FILE, HINT_SIZE, HINT_SYMBOLS, "hint_24")
    # 电源状态 chip pwr_16：ASCII 可打印（"电量低 56%"等）+ 状态提示中文字
    # 注意：充电动画用 U+00B7 中点（·），必须一并收录，否则点动画缺字形
    PWR_SIZE = 16
    PWR_SYMBOLS = "".join(chr(c) for c in range(0x20, 0x7F)) + "充电中即将满量低耗尽休眠·"
    conv(HAN_FILE, PWR_SIZE, PWR_SYMBOLS, "pwr_16")
    for size in MENU_SIZES:
        conv(HAN_FILE, size, ascii_symbols + MENU_CJK, "menu_%d" % size)
    print("gen_fonts: role_name charset = ASCII printable + extra (%d glyphs)" % len(ascii_symbols))


main()
