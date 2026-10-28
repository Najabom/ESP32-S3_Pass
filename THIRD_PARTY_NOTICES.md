# Third-Party Notices

本项目包含或依赖以下第三方组件。各组件版权归其作者所有，适用各自许可证。

## 字体

### Source Han Sans SC Medium

- 文件：`fonts/SourceHanSansSC-Medium.otf`
- 版权：Copyright 2014-2025 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.
- 许可证：SIL Open Font License, Version 1.1
- 用途：生成 `src/ui/fonts/` 中的部分 LVGL 点阵字库，以及 `assets/sd_fonts/` 中的 SD 卡字库。

### Bebas Neue

- 文件：`fonts/BebasNeue.otf`
- 版权：Copyright © 2010 by Dharma Type.
- 许可证：SIL Open Font License, Version 1.1
- 用途：生成 `src/ui/fonts/bebas_*.c` 点阵字库。

OFL 1.1 全文见 [`LICENSES/OFL-1.1.txt`](LICENSES/OFL-1.1.txt)。上述字体文件未被修改；由其生成的点阵字库属于 OFL 允许的嵌入/派生使用，并按 OFL 1.1 分发，不改变本项目代码的 MIT 许可。

## 固件依赖

### LVGL

- 版本：8.4.x（通过 PlatformIO `lvgl/lvgl` 获取）
- 许可证：MIT
- 版权：Copyright (c) 2021 LVGL Kft
- 全文：[`LICENSES/LVGL-MIT.txt`](LICENSES/LVGL-MIT.txt)

### Espressif esp_new_jpeg

- 版本：1.0.2
- 许可证：Espressif MIT License
- 版权：Copyright (c) 2024 Espressif Systems (Shanghai) CO., LTD
- 全文：[`LICENSES/esp_new_jpeg-MIT.txt`](LICENSES/esp_new_jpeg-MIT.txt)

### ESP-IDF

- 版本：6.0.x
- 许可证：Apache License 2.0
- 版权：Copyright 2015-2026 Espressif Systems (Shanghai) CO., LTD
- 全文：[`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt)

## 说明

本仓库不包含 LVGL、ESP-IDF 或 `esp_new_jpeg` 的完整源代码；它们由 PlatformIO / ESP-IDF 组件管理器在构建时获取。若需再分发二进制，请同时遵守上述组件的许可证要求。
