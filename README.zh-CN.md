<!--
Copyright (c) Meta Platforms, Inc. and affiliates.
Copyright (c) 2026 ledienbien-ai (translation and fork-specific text)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

<p align="center">
  <a href="https://dbrobot.vn/"><img src="doc/image/db-robot-logo.png" width="160" alt="DB_ROBOT"></a>
</p>
<p align="center">
  开发者: <b>ledienbien-ai</b> · 主页: <a href="https://dbrobot.vn/">https://dbrobot.vn/</a>
</p>

# 适用于 ESP32-S3 设备的 Muse Gadgets

[English](README.md) | [Tiếng Việt](README.vi.md) | **简体中文** | [日本語](README.ja.md) | [한국어](README.ko.md)

> 这是 [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
> 的非官方社区分支，与 Meta、Waveshare（微雪）及其他任何开发板厂商都没有关联，
> 也未获得它们的认可。译文与[英文版](README.md)不一致时，以英文版为准。

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/images/muse-gadgets-dark.png">
    <img src=".github/images/muse-gadgets-light.png" width="900" alt="Muse gadget：Waveshare 圆形 AMOLED、M5Stack StickS3、Muse Home Link、树莓派和 Seeed reTerminal 墨水屏">
  </picture>
</p>

Muse gadget 是可以自己动手制作的开源设备：给市面上现成的 ESP32 开发板烧录程序，
或者配置一台树莓派，然后把 Muse 接到你的屏幕、按键、传感器和执行器上。本分支为
ESP32 Device SDK 增加上游尚未支持的 ESP32-S3 开发板，SDK 的其余部分与上游保持
一致。

这是极客为极客做的项目，纯属好玩。烧录自定义固件可能让开发板变砖并失去保修，
风险自负！

## 本分支中的开发板

每块板子都运行完整的屏幕界面：动态头像、按住说话、触摸设置，以及显示 Muse 发来
的图片。

| 开发板 | 屏幕 | 名称 | Profile | 固件 |
|---|---|---|---|---|
| [Waveshare ESP32-S3-Touch-LCD-1.85C](#waveshare-esp32-s3-touch-lcd-185c) | 1.85 英寸圆形 360×360，触摸 | `s3lcd` | `waveshare-s3-185c` | 在线烧录固件：https://dbrobot.vn/firmware.html |
| [OSTB-3ST](#ostb-3st) | 1.83 英寸 296×240，触摸 | `ostb` | `ostb-3st` | 在线烧录固件：https://dbrobot.vn/firmware.html |
| [LCDWIKI 2.8inch ESP32-S3 Display](#lcdwiki-28inch-esp32-s3-display) | 2.8 英寸 320×240，ES3C28P 带触摸 | `lcd28` | `lcdwiki-s3-28` | 在线烧录固件：https://dbrobot.vn/firmware.html |
| [Espressif EchoEar](#espressif-echoear) | 1.85 英寸圆形 360×360，触摸 | `echoear` | `echoear` | 在线烧录固件：https://dbrobot.vn/firmware.html |

名称是 `tools/muse/board.sh` 对开发板的叫法；profile 是它的编译配置和编译目录的
名字。上游支持的开发板仍然保留，见
[`esp32/devices/README.md`](esp32/devices/README.md)（英文）。

在你的开发板上遇到问题？请在本仓库的
[Issues](https://github.com/ledienbien-ai/muse-gadget-sdk/issues) 中反馈。

## 越南语界面与语音回复

上面几块开发板的界面增加了两项功能。

<p align="center">
  <img src="doc/image/vietnamese-ui.png" width="720" alt="模拟器绘制的 360×360 越南语界面：就绪、正在朗读回复、关闭扬声器时的回复、错误、设置、语言页面、声音页面和配对码">
</p>

**越南语或英语界面。** 开发板默认以越南语启动。从 Muse 向左滑动，打开
**Settings > Language**（Cài đặt > Ngôn ngữ），选择 **English** 或
**Tiếng Việt**；Muse 会重启并切换到所选语言，之后一直保留。没有触摸屏的板子，
可通过串口发送 `>lang=en` 或 `>lang=vi`，或在
[`esp32/tools/muse/ble_setup.html`](esp32/tools/muse/ble_setup.html) 页面上选择。
越南语界面下，回复和你说的话会带声调符号显示；英语界面下则不带，因为英语字体
没有这些字母。

**朗读回复。** Muse 以文本回复设备。扬声器打开时，开发板把每条回复发送到
Google 翻译的文本转语音，并播放返回的音频，字幕随语音滚动：回复是越南语时用
越南语语音，否则用界面语言的语音。扬声器关闭时（长按主界面上的扬声器按钮，或
Settings > Sound），回复只以文字显示，不发送任何内容。

> 语音来自 Google 翻译“朗读”按钮背后的地址。它不需要密钥，但不是公开发布的
> 服务：Google 随时可能更改、限制或屏蔽它，而且每条被朗读的回复文本都会发送给
> Google。它没有响应时，回复会像以前一样以文字显示。用
> `CONFIG_MUSE_TTS_GOOGLE=n` 构建可去掉此功能，或在
> [`muse_tts.c`](esp32/components/muse/muse_tts.c) 中接入你自己的服务。
> `CONFIG_MUSE_LANG_DEFAULT_VI=n` 让开发板以英语启动。

代码：[`muse_lang.c`](esp32/components/muse/muse_lang.c)（界面文字），
[`muse_fonts.c`](esp32/components/muse/muse_fonts.c) 和
[`fonts/`](esp32/components/muse/fonts)（越南语字母），
[`muse_tts.c`](esp32/components/muse/muse_tts.c) 和
[`muse_tts_text.c`](esp32/components/muse/muse_tts_text.c)（语音）。

## Waveshare ESP32-S3-Touch-LCD-1.85C

<p align="center">
  <img src="doc/image/waveshare-s3-185c-device.jpg" width="300" alt="带外壳的 Waveshare ESP32-S3-Touch-LCD-1.85C">
  <img src="doc/image/waveshare-s3-185c-muse.jpg" width="300" alt="1.85C 上的 Muse 设置页面">
</p>
<p align="center">
  <img src="doc/image/Wareshare%20Touch%20LCD%201.85C.png" width="720" alt="模拟器绘制的 360 像素圆屏 Muse 界面：就绪、配对、聆听、思考、出错和说话">
</p>

| 部分 | 说明 |
|---|---|
| 芯片 | ESP32-S3R8，16 MB flash，8 MB 八线 PSRAM，原生 USB |
| 屏幕 | 1.85 英寸圆形 360×360 ST77916 LCD，QSPI 接口，PWM 背光 |
| 触摸 | CST816 |
| 音频（V1 板） | PCM5101 DAC 和数字 I2S 麦克风 |
| 音频（V2 板，“Rev2.0”） | ES8311 DAC 和 ES7210 ADC，双麦克风 |
| 按键 | BOOT：按住说话 |
| 电池 | 通过 GPIO8 上的分压电路测量电压 |

同一个固件支持两种音频版本：启动时会检测是否存在 ES8311。

已知限制：BOOT 是唯一的按键，所以屏幕按定时器熄屏，关机在 Settings 的 Power
页面里（开发板进入深度睡眠，按 BOOT 唤醒；真正断电要用拨动开关）。充电芯片的状态
没有接到任何引脚，所以只有连接电脑时才会显示“充电中”。

代码：[`board_waveshare_s3_185c.c`](esp32/components/muse/boards/board_waveshare_s3_185c.c)，
编译配置：[`sdkconfig.muse-waveshare-s3-185c`](esp32/devices/sdkconfig.muse-waveshare-s3-185c)。
硬件资料：[Waveshare 文档](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C)。

## OSTB-3ST

<p align="center">
  <img src="doc/image/ostb-3st-device.jpg" width="560" alt="OSTB-3ST 的正面和背面">
</p>
<p align="center">
  <img src="doc/image/ostb-3st.png" width="720" alt="模拟器绘制的 296×240 Muse 界面：就绪、配对、聆听、思考、出错和说话">
</p>
<p align="center">
  <img src="doc/image/ostb-3st-settings.png" width="720" alt="模拟器绘制的 296×240 设置页面">
</p>

根据这块开发板自带的 xiaozhi-esp32 固件源码（`ostb-xiaozhi-3st`）移植。界面按
296×240 的横屏排版。

| 部件 | 说明 |
|---|---|
| 芯片 | ESP32-S3，16 MB flash，8 MB 八线 PSRAM，原生 USB |
| 屏幕 | 1.83 英寸 240×296 NV3023 LCD，SPI 接口，横屏使用，PWM 背光 |
| 触摸 | CST816，没有中断引脚，靠轮询读取 |
| 音频 | ES8311 DAC 和 ES7210 ADC |
| 按键 | 位于顶部。**+**（音量加）：按住说话。**−**（音量减）：短按息屏，长按关机。中间的按键没有用到 |
| 电池 | 按原固件的 ADC 表换算电量，另有充电状态引脚 |

已知限制：4G 模块和 LED 没有用到。电池只显示电量，不显示电压。关机是拉高开发板
的关机引脚；接着 USB 时开发板可能不会断电，屏幕保持熄灭，按 + 键后重新启动。
电池充满后，只有连着电脑时开发板才知道自己接着 USB 电源。

代码：[`board_ostb_3st.c`](esp32/components/muse/boards/board_ostb_3st.c)，
编译配置：[`sdkconfig.muse-ostb-3st`](esp32/devices/sdkconfig.muse-ostb-3st)。

## LCDWIKI 2.8inch ESP32-S3 Display

<p align="center">
  <img src="doc/image/lcdwiki-s3-28-device.jpg" width="330" alt="LCDWIKI 2.8inch ESP32-S3 Display 正面">
  <img src="doc/image/lcdwiki-s3-28-back.jpg" width="330" alt="背面，标注了接口和按键">
</p>
<p align="center">
  <img src="doc/image/lcdwiki-s3-28.png" width="720" alt="模拟器绘制的 320×240 Muse 界面">
</p>

一款以多种名称销售的 2.8 英寸屏幕开发板。LCDWIKI 有两个型号：带电容触摸的
**ES3C28P** 和不带触摸的 **ES3N28P**。同一个固件支持两个型号：启动时会检测是否
存在触摸芯片。

| 部分 | 说明 |
|---|---|
| 芯片 | ESP32-S3R8，16 MB flash，8 MB 八线 PSRAM，原生 USB |
| 屏幕 | 2.8 英寸 240×320 ILI9341V LCD，SPI 接口，40 MHz，横屏使用，PWM 背光 |
| 触摸 | ES3C28P 为 FT6336G；ES3N28P 没有 |
| 音频 | ES8311 编解码器，一个麦克风，FM8002E 功放 |
| 按键 | BOOT：按住说话 |
| 电池 | 按原固件的 ADC 范围换算电量，接在 GPIO9 |

已知限制：BOOT 是唯一的按键，所以屏幕按定时器熄屏，关机在 Settings 的 Power
页面里（开发板进入深度睡眠，按 BOOT 唤醒）。ES3N28P 没有触摸，又只有一个按键，
所以设备上没有设置界面：只能按住说话，并通过 Muse 应用完成设置。电池只显示
电量，不显示电压；充电芯片的状态没有接到任何引脚，所以只有连接电脑时才会显示
“充电中”。SD 卡槽和 RGB LED 没有用到。

代码：[`board_lcdwiki_s3_28.c`](esp32/components/muse/boards/board_lcdwiki_s3_28.c)，
编译配置：[`sdkconfig.muse-lcdwiki-s3-28`](esp32/devices/sdkconfig.muse-lcdwiki-s3-28)。

## Espressif EchoEar

<p align="center">
  <img src="doc/image/echoear-muse.jpg" width="330" alt="EchoEar 上的 Muse 效果图，屏幕上是越南语回复：把模拟器的画面合成到设备图片里">
  <img src="doc/image/echoear-device.jpg" width="330" alt="第二张效果图，正在聆听">
</p>
<p align="center">
  <img src="doc/image/echoear-boards-front.webp" width="720" alt="EchoEar 的电路板和整机正面，带标注：ESP32-S3 模组、各连接器、麦克风、绿色 LED 和屏幕">
</p>
<p align="center">
  <img src="doc/image/echoear-boards-back.jpg" width="605" alt="电路板和整机背面，带标注：编解码器、功放、运动传感器、SD 卡槽、BOOT 和 RST 按键、磁吸连接器和电源开关">
</p>
<p align="center">
  <img src="doc/image/echoear.png" width="720" alt="模拟器绘制的 360×360 Muse 界面">
</p>

乐鑫的猫形语音开发套件“喵伴”，也以 ESP-VoCat 的名字销售。这块板有 v1.0 和 v1.2
两个版本，几个引脚的位置不同。同一个固件适用于两者：启动时自动识别版本并写入日志。

| 部件 | 详情 |
|---|---|
| 芯片 | v1.2 为 ESP32-S3-WROOM-1-N16R16VA（16 MB flash），v1.0 为 ESP32-S3-WROOM-2-N32R16V（32 MB flash）；16 MB 八线 PSRAM，原生 USB |
| 显示屏 | 1.85 英寸圆形 360×360 ST77916 LCD，QSPI 接口，PWM 背光 |
| 触摸 | CST816S |
| 音频 | ES8311 编解码器，NS4150B 功放，ES7210 接两个麦克风（使用其中一个） |
| 按键 | BOOT，在背面磁吸连接器旁；外壳下的触摸片（v1.2 两个，v1.0 一个）：按住任意一个说话 |
| 电池 | BQ27220 电量计：电量、电压和充电状态 |

已知限制：BOOT 是固件能读取的唯一按键，所以屏幕按定时器熄屏，在 Settings 的 Power
页面关机只是让开发板进入深度睡眠，按 BOOT 唤醒；真正断电要用板上的电源键。触摸片
和 BOOT 一样可以说话、唤醒屏幕，但不能确认与 Muse 应用的配对，也不能结束深度
睡眠：这两件事要按 BOOT。触摸片被连续按住 20 秒会被视为被遮盖而不是触摸，并被
松开。运动传感器、SD 卡槽和绿色 LED 没有使用。电量计由电池供电，所以没接
电池时没有电量显示。接在充电器（而不是电脑）上时，只有电流流入电池期间才显示
“充电中”。

代码：[`board_echoear.c`](esp32/components/muse/boards/board_echoear.c)，
构建配置：[`sdkconfig.muse-echoear`](esp32/devices/sdkconfig.muse-echoear)。

## 编译和烧录

每块开发板的步骤都一样。名称和 profile 见[上面的表格](#本分支中的开发板)。

1. 获取 [SDK token](https://gadgets.muse.ai/settings/sdk-tokens)，并阅读
   [Gadget SDK 条款](https://gadgets.muse.ai/sdk-terms)。每个 gadget 都需要
   token 才能配对。
2. 安装 ESP-IDF **v6.0.1**（见 [`esp32/README.md`](esp32/README.md)）。
3. 在 `esp32/` 目录下编译。macOS 或 Linux，使用开发板的名称：

   ```sh
   tools/muse/board.sh build ostb
   ```

   Windows，在 ESP-IDF PowerShell 中，使用开发板的 profile：

   ```powershell
   $P = "ostb-3st"
   idf.py -B build-muse-$P -DIDF_TARGET=esp32s3 `
     "-DSDKCONFIG=build-muse-$P/sdkconfig" `
     "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-$P" build
   ```

   用 `idf.py -B build-muse-$P menuconfig`
   （ESP32 Device SDK > Muse Gadgets SDK token）设置 token，然后重新编译。
4. 通过 USB-C 烧录，把 `PORT` 换成开发板的串口：

   ```powershell
   idf.py -B build-muse-$P -p PORT flash
   ```

   如果连接不上，让开发板进入下载模式后再试：按住 **BOOT**，点按 **RESET**，
   松开 **BOOT**。没有 RESET 键的开发板，按住 BOOT 的同时插上数据线。

   如果用网页烧录工具，先合并成一个文件，再写入地址 `0x0`：

   ```powershell
   idf.py -B build-muse-$P merge-bin -o muse-gadget-$P-merged.bin
   ```

   带有你的 SDK token 的固件会包含该 token，公开发布文件前请三思。
5. 在 Muse 应用中打开 **Settings > Devices > Developer mode**，添加名为
   `MuseGadget-XXXXXX` 的设备，并在提示时按下开发板的说话键（1.85C 和
   LCDWIKI 开发板和 EchoEar 是 BOOT，OSTB-3ST 是 + 键）。

## 添加其他设备

一块带屏幕、扬声器和麦克风的开发板，只需要一个驱动文件和几行注册代码。本分支中
的每块开发板都由以下部分组成：

1. `esp32/components/muse/boards/board_<id>.c` 填写 `muse_board_t`
   （`muse_board.h`）：屏幕、触摸、音频、按键、电池和电源。从与你的板子最接近的
   那一块改起。
2. `esp32/devices/sdkconfig.muse-<profile>` 保存编译配置：芯片、flash、PSRAM。
3. `esp32/components/muse/Kconfig`、`CMakeLists.txt` 和 `idf_component.yml`
   注册这块板子，以及它需要的驱动组件。
4. `esp32/tools/muse/board.sh`、`ports.py` 和 `avatar.py` 加上它的短名称。
5. 文档也要更新：每份 README 的表格里加一行、正文加一节，
   [`esp32/devices/README.md`](esp32/devices/README.md) 里加上相应的行，
   `doc/image/` 里放一张图片，并在 [`CREDITS.md`](CREDITS.md) 里写明参考来源及其
   许可证。

界面会自动适应圆屏和矩形屏。上面的图片来自 `esp32/simulator` 里的模拟器，把其中
`src/sim_board.c` 的屏幕尺寸改成开发板的尺寸即可：拿到板子之前就能看到新尺寸下的
效果。完整的步骤见 [`esp32/devices/AGENTS.md`](esp32/devices/AGENTS.md)（英文），
从收集开发板资料到检查结果都有说明。

## SDK 的其余部分

| | |
|---|---|
| [**ESP32 Device SDK**](esp32) | 把你的 ESP32 开发板接入 Muse。加一块屏幕显示图片，加上音频输入输出，或者接上其他传感器。 |
| [**Linux Device SDK**](linux) | 把闲置的树莓派或 Linux 主机变成 Muse gadget。加入你自己的命令，让 Muse 处理系统管理杂务或你的 Home Assistant。 |

ESP32 和 Linux gadget 通过 iOS 和 Android 上的 Muse 应用配对，入口在
Settings > Devices。先在那里打开 Developer mode，再查找名称以 “MuseGadget”
开头的设备。每个目录都有一份入门用的 `README.md` 和一份给编程助手看的
`AGENTS.md`（均为英文）。

## 社区

上游项目的社区在 [Discord](https://discord.gg/3bhjCkZdd6) 上交流。关于本分支
增加的开发板的问题，请在本仓库提交 issue。

## 许可证和致谢

- Muse Gadget SDK 的版权归 Meta Platforms, Inc. 及其关联公司所有，采用
  Apache License 2.0 许可，见 [`LICENSE`](LICENSE)。
- 本分支中的修改版权归 ledienbien-ai（2026）所有，采用相同的许可证。
- 1.85C 的移植参考了 Waveshare 的文档和
  [示例代码](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C)
  （Apache-2.0），以及 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
  中对应的开发板代码（MIT）。
- OSTB-3ST 的移植从该开发板的 xiaozhi-esp32 固件源码中取得引脚、屏幕初始化表
  和电池表，这些文件本身没有许可证声明；xiaozhi-esp32 采用 MIT 许可。
- LCDWIKI 2.8inch 的移植参考了 [LCDWIKI 的页面](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)、该开发板的 xiaozhi-esp32
  文件，以及 [esphome-es3c28p-light-panel](https://github.com/jvduuren/esphome-es3c28p-light-panel) 中公布的实测结果；没有复制其中的
  代码。
- 上游有两个文件保留各自的许可证：`minimp3.h`（CC0-1.0）和 `pixel_font.c`
  （BSD-2-Clause）。编译时下载的组件适用它们各自的许可证。
- DB_ROBOT 开机 logo 属于 ledienbien-ai，可以自由使用，采用与本分支其他修改相同
  的许可证。
- Apache 许可证不涵盖 [Jollybot 头像](esp32/avatar)，也不涵盖 Meta、Muse 和
  Waveshare 的名称与商标。

完整清单见 [`CREDITS.md`](CREDITS.md)（英文）：每个来源及其许可证、本分支新增或
修改的文件，以及后续版本需要保持的事项。
