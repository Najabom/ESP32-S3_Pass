# ESP32-S3_Pass

PASS-II 便携通行证设备固件。基于 ESP32-S3，使用 LVGL 绘制界面，从 SD 卡播放角色 MJPEG 动画，并通过 BLE 与 ESP-NOW 完成配置和名片交换。

> 当前版本：**V1.0**

## 功能

- 240×320 ST7789 屏幕 + XPT2046 触摸，LVGL 8 界面
- SD 卡角色目录：`loop.mjpeg` 循环动画、`intro.mjpeg` 入场动画、`portrait.bin` 头像
- BLE 配置与文件传输：角色切换、UID / 寄语 / 用户名、时间同步、IP 过滤、固件信息
- ESP-NOW 名片交换：接收对方名片、保存交换记录与卡片图片
- DS3231 RTC 走时，BLE 校时
- 电池电压监测、USB 插入检测、低电量提示与自动休眠
- 板载按键/触摸菜单、启动动画、角色选择、名片夹

## 硬件

目标硬件为 **ESP32-S3 N16R8**（16 MB Flash + 8 MB Octal PSRAM）核心板与 PCB v6.2：

| 功能 | 引脚 |
| --- | --- |
| LCD SPI2 SCK / MOSI / CS / DC / RST / BLK | GPIO 11 / 14 / 13 / 12 / 10 / 9 |
| microSD SPI3 CS / MOSI / MISO / SCK | GPIO 7 / 6 / 4 / 5 |
| 触摸 XPT2046 T_CLK / T_DIN / T_DO / T_CS / T_IRQ | GPIO 42 / 41 / 40 / 39 / 38 |
| DS3231 I2C SDA / SCL | GPIO 8 / 18 |
| DS3231 32K / SQW | GPIO 16 / 17 |
| USB 插入检测 | GPIO 1 |
| 电池电压采样（1/2 分压） | GPIO 2 (ADC1_CH1) |

> 引脚定义集中在 `src/bsp/pins_pass.h`。旧版开发板接线与 PCB v6.2 不同，烧录前请确认硬件版本。

## 构建

依赖：

- [PlatformIO Core](https://platformio.org/)（推荐）
- Node.js + `lv_font_conv`：`npm install -g lv_font_conv`
- Python 3（PlatformIO 自带环境即可）

```bash
pio run
pio run -t upload
```

构建前会自动执行 `tools/gen_fonts.py`，从 `fonts/` 生成 LVGL 字库到 `src/ui/fonts/`。如果修改了字库字符集或字体文件，请重新构建；若出现字体结构布局变化导致的花屏/缺字，执行一次完整清理：

```bash
pio run -t clean
pio run
```

SD 卡字库（`assets/sd_fonts/*.bin`）不参与固件编译，需要单独复制到 SD 卡；修改后可用：

```bash
python tools/gen_sd_fonts.py
```

> 分区表在 `partitions.csv`。修改分区后必须先整片擦除：`pio run -t erase`，再重新烧录。

### 合并烧录镜像

Release 中的 `firmware_merged.bin` 已按 16 MB 填充，可直接从 `0x0` 烧录（适用于 esptool-js 等 Web 烧录器）。自行生成时：

```bash
esptool.py --chip esp32s3 merge_bin \
  -o firmware_merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB --fill-flash-size 16MB \
  0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

镜像布局：bootloader `0x0`、分区表 `0x8000`、应用 `0x10000`，其余填充 `0xFF`。

## SD 卡目录

```text
/config/current.txt              当前角色目录名
/config/ip.txt                   ESP-NOW 产品 IP 过滤（可空，空 = 全通过）
/config/bt.txt                   BLE 名称/广播开关（可选）
/config/nvs_backup.txt           固件自动生成的设置备份
/config/fonts/role_name_30.bin   30px 角色名字库（可选）
/config/fonts/role_name_16.bin   16px 角色名字库（可选）
/<角色名>/loop.mjpeg              必需：循环动画
/<角色名>/intro.mjpeg             可选：入场动画
/<角色名>/portrait.bin            可选：64×64 RGB565 头像
/<角色名>/ui_text.txt             可选：角色界面文本
/cards.txt                       收到的名片记录
/cards/card_<mac>.bin            收到的名片原始数据
```

`loop.mjpeg` / `intro.mjpeg` 为单文件上限 3 MB 的 MJPEG；`portrait.bin` 为 64×64 RGB565、8192 字节。SD 卡角色目录名上限 63 个 UTF-8 字节；通过 BLE 上传时角色名上限 48 字节。`ui_text.txt` 支持 `leftcol=`（≤13 字符，ASCII）、`sub_text=`（≤40 字符，ASCII）和 `op_name=`（角色名，可中文）；留空则对应元素不显示。

## BLE 协议

设备广播 16-bit Service Data UUID `0xFFE0`，内容 `"AP"`；连接后可读取 `0xFFF5` 校验值 `"PSUP"`。

主要服务：

- `0x180A` 设备信息：`0x2A28` 软件版本
- `0xFFE0` 设置：当前角色、界面文本、IP 配置、日志级别、重载、UID、寄语、用户名、时间、设备名、清空交换记录
- `0xFFF0` SD 卡：角色列表、文件读取、分块上传、上传状态、删除角色

配套 PC 工具：[ESP32-S3_Pass_Tool](https://github.com/Najabom/ESP32-S3_Pass_Tool)。

## 字体与第三方组件

界面字体由 **Source Han Sans SC Medium** 与 **Bebas Neue** 生成，二者均以 SIL Open Font License 1.1 发布；生成的点阵字库同样按 OFL 1.1 分发。详见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) 和 [`LICENSES/OFL-1.1.txt`](LICENSES/OFL-1.1.txt)。

其他组件：

- LVGL 8.4（MIT）
- Espressif `esp_new_jpeg` 1.0.2（Espressif MIT）
- ESP-IDF 6.0（Apache-2.0）

## 目录结构

```text
src/bsp/           显示、触摸、SD、RTC、电源
src/mjpeg_player/  MJPEG 播放器
src/nfc/           BLE、ESP-NOW、协议、名片、配置备份、校时
src/ui/            LVGL 界面、角色、名片夹、字库
include/           版本、LVGL 配置
tools/             字库/头像生成脚本
assets/sd_fonts/   SD 卡字库
fonts/             字体源文件
docs/              开发文档
```

## 许可证

代码以 [MIT License](LICENSE) 发布。字体与第三方组件许可见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。

## 声明

本项目为独立的个人硬件项目，不隶属于任何游戏、公司或商业组织，也不包含任何游戏素材。角色动画、名称与头像由使用者自行提供；请确保你拥有相应素材的使用权。固件按“现状”提供，不对硬件损坏、电池安全或数据丢失作任何担保。
