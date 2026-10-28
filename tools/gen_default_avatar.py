# tools/gen_default_avatar.py - 生成内置默认头像头文件
#
# 用途：用户未在 cardstore 写 self.bin 时，ESP-NOW 交换发送的头像
#       以及名片横幅缺头像时的回退图，都用这个固件内置默认图。
#       生成 src/resources/default_avatar.h（static const uint16_t 数组，
#       64x64 RGB565，被 espnow_nfc.c 和 ui_core_screen.c 引用）。
#
# 可调整内容：
#   - W, H = 64：必须与 CARD_AVATAR_SIZE(64*64*2) 一致，改大要连带
#     协议/UI 缓冲/本文件三处同步，不建议。
#   - BG/FG/EDGE：底色、图案（对角线 X）、边框色。0xRRRR 是 RGB565
#     原值（无需字节交换——发送端 warm_swap 链路上会统一处理），
#     改色即改默认头像外观，运行 `python tools/gen_default_avatar.py`
#     重新生成后构建生效。
#
# 图案：蓝色底 + 白色对角 X + 蓝色描边，纯代码生成，无版权问题。

import os

W, H = 64, 64
BG = 0x18E3
FG = 0xFFFF
EDGE = 0x001F

px = [[BG] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        if x < 3 or x >= W - 3 or y < 3 or y >= H - 3:
            px[y][x] = EDGE
        elif abs(x - y) < 3 or abs(x + y - (W - 1)) < 3:
            px[y][x] = FG

out_path = os.path.join(os.path.dirname(__file__), "..", "src", "resources", "default_avatar.h")
os.makedirs(os.path.dirname(out_path), exist_ok=True)
with open(out_path, "w") as f:
    f.write("#ifndef DEFAULT_AVATAR_HEADER\n#define DEFAULT_AVATAR_HEADER\n\n")
    f.write("#include <stdint.h>\n\n")
    f.write("#define DEFAULT_AVATAR_W 64\n#define DEFAULT_AVATAR_H 64\n")
    f.write("#define DEFAULT_AVATAR_SIZE (DEFAULT_AVATAR_W * DEFAULT_AVATAR_H * 2)\n\n")
    f.write("static const uint16_t default_avatar_565[] = {\n")
    for y in range(H):
        row = ", ".join(f"0x{v:04X}" for v in px[y])
        f.write(f"    {row},\n")
    f.write("};\n\n#endif /* DEFAULT_AVATAR_HEADER */\n")
print("written:", out_path)
