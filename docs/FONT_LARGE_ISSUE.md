# 横幅中文花字问题 —— 根因记录与解决路径

日期：2026-08-29
设备：ESP32-S3 角色名片系统（PASS）
表现：横幅（角色交换卡片弹出）中的部分汉字显示为花字/乱位；
     固定个别字（试 U+8BD5、随 U+968F）始终损坏，其他字正常；
     16px 详情页/菜单文字全部正常；30px 横幅大字部分损坏；
     屏幕左侧菜单/角色名（leftcol）此前也曾一度全部消失。

## 一、现象特征（关键分界）

- 只在 **30px 字库**（banner 大字号）部分字损坏，且**损坏字固定**
- 16px 字库（菜单/详情页）全部正常
- 详情页/菜单中的相同汉字完全正常（如"试"在 16px 正常、30px 花）
- canvas 直接解包显示的字样与横幅上"如出一辙"（用户确认）
- 换用无压缩（no-compress）30px bin 后数据显示但仍异常（后续回退）
- 损坏字的位置"永远一样"（每次开机固定）

## 二、排查过程（已排除项）

1. **SD 卡上的字体文件**：FNV-1a x4 段校验 + PC 侧 md5 对比 → A 板 SD 上
   role_name_30.bin 与 PC assets 逐字节一致（已排除文件损坏）
2. **lv_font_conv 生成的 bin 格式**：PC 侧完整解析（cmap 55 表 / stride 16 /
   loca u32 / glyf 位流）→ 所有字形 gid/bbox/adv 合理
3. **PC 侧 RLE 解压模拟**：从同一 bin 解出 PC 版字形数据（正常字形模板）→
   与设备端打印的 `dec`（解压输出）不同（如试=PC 80 1f ff f3... vs 设备 07 ee...）
4. **自写解析器 vs LVGL 官方 lv_font_loader.c**：逐行对比（bit_iterator /
   read_bits / read_label / load_cmaps_tables / load_glyph 两遍循环 /
   load_kern / parse_bin_font）→ 每行等价（已排除算法差异）
5. **FATFS/GPT 偏移**：之前已修，排除
6. **keep 白名单机制**：当前 names=NULL 全量加载（keep=NULL），排除

## 三、★ 根本原因（铁证）

**LVGL 8.4 的字体描述符 `lv_font_fmt_txt_glyph_dsc_t` 默认使用
`bitmap_index : 20` 位域 —— 最大 1MB 位图索引上限。**

30px GB2312 全集字库的压缩位图数据（glyph_bitmap 池）累计约 **1.8MB**，
**超过 1MB**，因此当 bitmap_index 累计量 > 1048576（2^20）时：
- bitmap_index 溢出回卷（回卷 0x100000）
- 中后段所有字形的 bitmap_index 指向错误位图区域 → 显示花字
- 16px 字库累计仅 729KB < 1MB，不溢出 → 全部正常
- 30px 只有 gid 下标 < 约 4000 的字形（bitmap_index < 1MB）正常，
  之后（如"试"gid=6018）全部错位固定损坏

### 精确验证（设备端探针 vs PC 仿真）

| 字形 | PC 正确累计 bitmap_index | 设备打印 | 差 |
|------|--------------------------|----------|-----|
| g4000 | 896613   | 896613   | 0（未溢出）|
| g5000 | 1160214  | 111638   | -1048576（回卷）|
| g5500 | 1283205  | 234629   | -1048576（回卷）|
| g6018 ("试") | 1417274 | 368698 | -1048576（回卷）|

每个差都是 **精确 1048576 = 2^20** —— 铁证：20-bit bitmap_index 溢出。

### 设备端辅助证据

- 设备探针 `gdsc[6018].bitmap_index = 368698`（已被回卷截断）
- 设备探针 `bitmap[U+8BD5] firstRow = 07 ee...`（读到的是错误偏移的数据）
- PC 端从文件读取的原始数据正常（0x4000 处为正确 RLE 字节）
- 探针 seq（LOADSEQ/LOADSEQB）在 g4000 之前与 PC 完全一致，之后每
  1000 字出现一次"回卷"级差异；g1000-3000 全部一致（未过 1MB 上限）

## 四、解决方案

### 方案（最终采用）

在 `include/lv_conf.h` 启用 LVGL 官方大字体开关：

```c
#define LV_FONT_FMT_TXT_LARGE 1
```

作用：将 `lv_font_fmt_txt_glyph_dsc_t` 切换为官方提供的 32-bit 版
（bitmap_index 32-bit，最大 4GB；box_w/box_h 升级为 u16；
ofs_x/ofs_y 升级为 i16），消除 1MB 上限。字库 bin 格式完全兼容
（lv_font_conv 1.5.3 生成时即按此设计，无需重新生成 bin）。

### 关键注意（踩坑记录）

**LV_FONT_FMT_TXT_LARGE 是全局结构体宏**，影响所有字体（内嵌
Bebas/role_name/uname/menu 等 .c 字库 + SD 字库）的数据布局。
必须 **全量 clean 重编**（`pio run -t clean && pio run`），否则增量编译
可能残留部分旧布局目标文件 → 新旧混编 → **leftcol / 角色名等
内嵌字库全部错乱不显示**（用户发现的"leftcol 也消失"正是此坑）。

## 五、修复后验证

- 全量重编 177.99s + 烧录 A 板成功
- 重启后：leftcol、角色名全部恢复；横幅中文（"试"/"随"等）全部正常
- 用户确认"文字问题解决了"

## 六、涉事文件

- `include/lv_conf.h`：+ `#define LV_FONT_FMT_TXT_LARGE 1`（含注释说明）
- `src/ui/sd_fonts.c`：自写内存版 bin 解析器（load_glyph 等），与官方
  lv_font_loader.c 等价（未改动，保留探针 LOG）
- `src/ui/ui_core_screen.c`：banner 名称与用户ID 左对齐（x 66→76）
- 观察用探针（可清理）：load_glyph 的 LOADSEQ/LOADSEQB 打印、
  banner font/fnt30/fnt16 字段打印、glyph30R 原始字节打印
- `.pio/libdeps/esp32-s3-devkitc-1/lvgl/src/font/lv_font_fmt_txt.h`：
  仅确认宏分支存在，未改库源码（官方宏直接提供 32-bit 版）

## 七、遗留可选优化

- 30px 字库 1.8MB 常驻 PSRAM（约 3.7MB 总字库内存，另有 16px 729KB）；
  如需保留更多内存，可改按需子集加载（切换角色时只加载当前角色名
  相关字——sd_fonts_load_for_ex 的 names 参数已支持，当前用 NULL 全量）
- 探针日志（W 级）确认无误后可以删除或降级

---
（本文件由开发会话记录生成，保留以便后续开发参考）
