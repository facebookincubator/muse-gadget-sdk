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
  Tác giả phát triển: <b>ledienbien-ai</b> · Trang chủ: <a href="https://dbrobot.vn/">https://dbrobot.vn/</a>
</p>

# Muse Gadgets cho thiết bị ESP32-S3

[English](README.md) | **Tiếng Việt** | [简体中文](README.zh-CN.md) | [日本語](README.ja.md) | [한국어](README.ko.md)

> Đây là bản fork không chính thức do cộng đồng thực hiện từ
> [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk).
> Bản này không liên kết với Meta, Waveshare hay bất kỳ hãng làm bo nào, và
> không được các bên đó bảo trợ. Nếu bản dịch khác với
> [bản tiếng Anh](README.md), bản tiếng Anh là bản chuẩn.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/images/muse-gadgets-dark.png">
    <img src=".github/images/muse-gadgets-light.png" width="900" alt="Các Muse gadget: màn AMOLED tròn của Waveshare, M5Stack StickS3, Muse Home Link, Raspberry Pi và màn e-ink Seeed reTerminal">
  </picture>
</p>

Muse gadget là những thiết bị mã nguồn mở do bạn tự làm: nạp chương trình cho
một bo ESP32 có sẵn trên thị trường hoặc cài đặt một chiếc Raspberry Pi, rồi
kết nối Muse với màn hình, nút bấm, cảm biến và cơ cấu chấp hành của bạn. Bản
fork này thêm vào ESP32 Device SDK những bo ESP32-S3 mà bản gốc chưa hỗ trợ,
còn phần còn lại của SDK giữ nguyên như bản gốc.

Dự án do dân vọc vạch làm cho dân vọc vạch, chỉ để cho vui. Nạp firmware tùy
biến có thể làm hỏng bo và mất bảo hành. Bạn tự chịu rủi ro!

## Các bo trong bản fork này

Mỗi bo chạy đầy đủ giao diện trên màn hình: avatar động, nhấn giữ để nói, cài
đặt bằng cảm ứng và hiển thị ảnh do Muse gửi.

| Bo | Màn hình | Tên | Profile | Firmware |
|---|---|---|---|---|
| [Waveshare ESP32-S3-Touch-LCD-1.85C](#waveshare-esp32-s3-touch-lcd-185c) | Tròn 1,85" 360×360, cảm ứng | `s3lcd` | `waveshare-s3-185c` | Nạp firmware online tại: https://dbrobot.vn/firmware.html |
| [OSTB-3ST](#ostb-3st) | 1,83" 296×240, cảm ứng | `ostb` | `ostb-3st` | Nạp firmware online tại: https://dbrobot.vn/firmware.html |
| [LCDWIKI 2.8inch ESP32-S3 Display](#lcdwiki-28inch-esp32-s3-display) | 2,8" 320×240, bản ES3C28P có cảm ứng | `lcd28` | `lcdwiki-s3-28` | Nạp firmware online tại: https://dbrobot.vn/firmware.html |
| [Espressif EchoEar](#espressif-echoear) | Tròn 1,85" 360×360, cảm ứng | `echoear` | `echoear` | Nạp firmware online tại: https://dbrobot.vn/firmware.html |

Tên là cách `tools/muse/board.sh` gọi bo; profile là tên của cấu hình build và
thư mục build. Các bo mà bản gốc hỗ trợ vẫn còn nguyên, liệt kê trong
[`esp32/devices/README.md`](esp32/devices/README.md) (tiếng Anh).

Gặp lỗi trên bo của bạn? Hãy báo trong mục
[Issues](https://github.com/ledienbien-ai/muse-gadget-sdk/issues) của repo này.

## Màn hình tiếng Việt và trả lời bằng giọng nói

Các bo ở trên có thêm hai thứ trong giao diện.

<p align="center">
  <img src="doc/image/vietnamese-ui.png" width="720" alt="Giao diện tiếng Việt ở 360×360, vẽ bằng trình mô phỏng: sẵn sàng, đang đọc câu trả lời, câu trả lời khi tắt loa, báo lỗi, cài đặt, trang Ngôn ngữ, trang Âm thanh và mã ghép đôi">
</p>

**Màn hình tiếng Việt hoặc tiếng Anh.** Các bo khởi động bằng tiếng Việt. Vuốt
sang trái từ Muse rồi mở **Cài đặt > Ngôn ngữ** (Settings > Language) để chọn
**English** hoặc **Tiếng Việt**; Muse khởi động lại sang ngôn ngữ đó và nhớ lựa
chọn. Với bo không có cảm ứng, gửi `>lang=en` hoặc `>lang=vi` qua cổng serial,
hoặc chọn trên trang
[`esp32/tools/muse/ble_setup.html`](esp32/tools/muse/ble_setup.html). Ở tiếng
Việt, câu trả lời và lời bạn nói hiện có dấu đầy đủ. Ở tiếng Anh chúng hiện
không dấu, vì phông chữ tiếng Anh không có chữ có dấu.

**Đọc câu trả lời thành tiếng.** Muse trả lời thiết bị bằng văn bản. Khi bật
loa, bo gửi từng câu trả lời tới dịch vụ đọc văn bản của Google Dịch rồi phát
âm thanh nhận về, phụ đề chạy theo lời đọc: giọng tiếng Việt nếu câu trả lời
viết bằng tiếng Việt, còn lại đọc bằng giọng của ngôn ngữ màn hình. Khi tắt loa
(giữ nút loa trên màn hình chính, hoặc Cài đặt > Âm thanh), câu trả lời chỉ
hiện chữ và không có gì được gửi đi.

> Giọng đọc lấy từ địa chỉ đứng sau nút "nghe" của Google Dịch. Nó không cần
> khóa API, nhưng không phải dịch vụ được công bố chính thức: Google có thể
> thay đổi, giới hạn hoặc chặn bất cứ lúc nào, và văn bản của mỗi câu trả lời
> được đọc sẽ gửi tới Google. Khi nó không trả lời, câu trả lời hiện bằng chữ
> như trước. Build với `CONFIG_MUSE_TTS_GOOGLE=n` để bỏ tính năng này, hoặc đặt
> dịch vụ của riêng bạn vào [`muse_tts.c`](esp32/components/muse/muse_tts.c).
> `CONFIG_MUSE_LANG_DEFAULT_VI=n` cho bo khởi động bằng tiếng Anh.

Mã nguồn: [`muse_lang.c`](esp32/components/muse/muse_lang.c) (các câu chữ),
[`muse_fonts.c`](esp32/components/muse/muse_fonts.c) và
[`fonts/`](esp32/components/muse/fonts) (chữ tiếng Việt),
[`muse_tts.c`](esp32/components/muse/muse_tts.c) và
[`muse_tts_text.c`](esp32/components/muse/muse_tts_text.c) (giọng nói).

## Waveshare ESP32-S3-Touch-LCD-1.85C

<p align="center">
  <img src="doc/image/waveshare-s3-185c-device.jpg" width="300" alt="Waveshare ESP32-S3-Touch-LCD-1.85C trong vỏ máy">
  <img src="doc/image/waveshare-s3-185c-muse.jpg" width="300" alt="Trang cài đặt của Muse trên bo 1.85C">
</p>
<p align="center">
  <img src="doc/image/Wareshare%20Touch%20LCD%201.85C.png" width="720" alt="Giao diện Muse trên màn tròn 360 px, do trình mô phỏng vẽ: sẵn sàng, ghép đôi, đang nghe, đang nghĩ, lỗi và đang nói">
</p>

| Thành phần | Chi tiết |
|---|---|
| Chip | ESP32-S3R8, flash 16 MB, PSRAM octal 8 MB, USB gốc |
| Màn hình | LCD tròn 1,85" 360×360 ST77916 qua QSPI, đèn nền PWM |
| Cảm ứng | CST816 |
| Âm thanh, bo V1 | DAC PCM5101 và micro số I2S |
| Âm thanh, bo V2 ("Rev2.0") | DAC ES8311 và ADC ES7210 với hai micro |
| Nút | BOOT: nhấn giữ để nói |
| Pin | Đo điện áp qua cầu chia trên GPIO8 |

Một firmware dùng cho cả hai phiên bản âm thanh: lúc khởi động nó dò xem có
ES8311 hay không.

Giới hạn đã biết: BOOT là nút duy nhất, nên màn hình tự tắt theo hẹn giờ và
việc tắt máy nằm ở trang Power trong Settings (bo vào deep sleep cho tới khi
nhấn BOOT; công tắc trượt mới ngắt nguồn). Trạng thái của mạch sạc không nối về
chân nào, nên bo chỉ báo "đang sạc" khi đang cắm vào máy tính.

Mã của bo: [`board_waveshare_s3_185c.c`](esp32/components/muse/boards/board_waveshare_s3_185c.c),
cấu hình build: [`sdkconfig.muse-waveshare-s3-185c`](esp32/devices/sdkconfig.muse-waveshare-s3-185c).
Tài liệu phần cứng: [trang của Waveshare](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C).

## OSTB-3ST

<p align="center">
  <img src="doc/image/ostb-3st-device.jpg" width="560" alt="OSTB-3ST, mặt trước và mặt sau">
</p>
<p align="center">
  <img src="doc/image/ostb-3st.png" width="720" alt="Giao diện Muse ở 296×240, do trình mô phỏng vẽ: sẵn sàng, ghép đôi, đang nghe, đang nghĩ, lỗi và đang nói">
</p>
<p align="center">
  <img src="doc/image/ostb-3st-settings.png" width="720" alt="Các trang cài đặt ở 296×240, do trình mô phỏng vẽ">
</p>

Port từ mã nguồn firmware xiaozhi-esp32 của chính bo này (`ostb-xiaozhi-3st`).
Giao diện được bố trí cho màn ngang 296×240.

| Thành phần | Chi tiết |
|---|---|
| Chip | ESP32-S3, flash 16 MB, PSRAM octal 8 MB, USB gốc |
| Màn hình | LCD NV3023 1,83" 240×296 qua SPI, dùng theo chiều ngang, đèn nền PWM |
| Cảm ứng | CST816, đọc bằng cách hỏi liên tục vì không có chân ngắt |
| Âm thanh | DAC ES8311 và ADC ES7210 |
| Phím | Nằm ở cạnh trên. **+** (tăng âm lượng): giữ để nói. **−** (giảm âm lượng): bấm để tắt màn hình, giữ để tắt nguồn. Phím ở giữa không được dùng |
| Pin | Mức pin theo bảng ADC của firmware gốc, và chân trạng thái của mạch sạc |

Giới hạn đã biết: modem 4G và đèn LED không được dùng. Pin chỉ hiện mức, không
hiện điện áp. Tắt nguồn là kéo chân tắt nguồn của bo; khi cắm USB bo có thể vẫn
chạy, màn hình tắt cho đến khi bấm phím +. Khi pin đã đầy, bo chỉ biết mình
đang cắm USB lúc được nối với máy tính.

Mã của bo: [`board_ostb_3st.c`](esp32/components/muse/boards/board_ostb_3st.c),
cấu hình build: [`sdkconfig.muse-ostb-3st`](esp32/devices/sdkconfig.muse-ostb-3st).

## LCDWIKI 2.8inch ESP32-S3 Display

<p align="center">
  <img src="doc/image/lcdwiki-s3-28-device.jpg" width="330" alt="LCDWIKI 2.8inch ESP32-S3 Display, mặt trước">
  <img src="doc/image/lcdwiki-s3-28-back.jpg" width="330" alt="Mặt sau, có chú thích các cổng và nút">
</p>
<p align="center">
  <img src="doc/image/lcdwiki-s3-28.png" width="720" alt="Giao diện Muse ở 320×240, do trình mô phỏng vẽ">
</p>

Bo màn hình 2,8" được bán dưới nhiều tên khác nhau. LCDWIKI làm hai phiên bản:
**ES3C28P** có cảm ứng điện dung và **ES3N28P** không có cảm ứng. Một firmware
dùng cho cả hai: lúc khởi động nó dò xem có chip cảm ứng hay không.

| Thành phần | Chi tiết |
|---|---|
| Chip | ESP32-S3R8, flash 16 MB, PSRAM octal 8 MB, USB gốc |
| Màn hình | LCD ILI9341V 2,8" 240×320 qua SPI ở 40 MHz, dùng theo chiều ngang, đèn nền PWM |
| Cảm ứng | FT6336G trên bản ES3C28P; bản ES3N28P không có |
| Âm thanh | Codec ES8311 với một micro, ampli loa FM8002E |
| Nút | BOOT: nhấn giữ để nói |
| Pin | Mức pin theo dải ADC của firmware gốc, trên GPIO9 |

Giới hạn đã biết: BOOT là nút duy nhất, nên màn hình tự tắt theo hẹn giờ và
việc tắt máy nằm ở trang Power trong Settings (bo vào deep sleep cho tới khi
nhấn BOOT). Trên bản ES3N28P, không có cảm ứng và chỉ có một nút, nên không có
phần cài đặt trên thiết bị: bo chỉ nhấn giữ để nói và được thiết lập từ ứng
dụng Muse. Pin chỉ hiện mức, không hiện điện áp, và trạng thái của mạch sạc
không nối về chân nào, nên bo chỉ báo "đang sạc" khi đang cắm vào máy tính. Khe
thẻ SD và đèn LED RGB không được dùng.

Mã của bo: [`board_lcdwiki_s3_28.c`](esp32/components/muse/boards/board_lcdwiki_s3_28.c),
cấu hình build: [`sdkconfig.muse-lcdwiki-s3-28`](esp32/devices/sdkconfig.muse-lcdwiki-s3-28).

## Espressif EchoEar

<p align="center">
  <img src="doc/image/echoear-muse.jpg" width="330" alt="Ảnh dựng Muse trên EchoEar, màn hình hiện câu trả lời tiếng Việt: màn hình của trình mô phỏng ghép vào ảnh thiết bị">
  <img src="doc/image/echoear-device.jpg" width="330" alt="Ảnh dựng thứ hai, đang nghe">
</p>
<p align="center">
  <img src="doc/image/echoear-boards-front.webp" width="720" alt="Các bo mạch của EchoEar và thiết bị nhìn từ phía trước, có chú thích: module ESP32-S3, các đầu nối, micro, LED xanh và màn hình">
</p>
<p align="center">
  <img src="doc/image/echoear-boards-back.jpg" width="605" alt="Các bo mạch và thiết bị nhìn từ phía sau, có chú thích: codec, ampli, cảm biến chuyển động, khe thẻ SD, nút BOOT và RST, đầu nối nam châm và công tắc nguồn">
</p>
<p align="center">
  <img src="doc/image/echoear.png" width="720" alt="Giao diện Muse ở 360×360, vẽ bằng trình mô phỏng">
</p>

Bộ kit giọng nói hình chú mèo của Espressif, còn được bán với tên ESP-VoCat.
Bo có hai phiên bản đang lưu hành, v1.0 và v1.2, khác nhau ở một số chân. Một
firmware dùng cho cả hai: nó tự nhận phiên bản lúc khởi động và ghi vào log.

| Thành phần | Chi tiết |
|---|---|
| Chip | ESP32-S3-WROOM-1-N16R16VA trên v1.2 (flash 16 MB), ESP32-S3-WROOM-2-N32R16V trên v1.0 (flash 32 MB); PSRAM 16 MB octal, USB gốc |
| Màn hình | LCD tròn 1,85" 360×360 ST77916 qua QSPI, đèn nền PWM |
| Cảm ứng | CST816S |
| Âm thanh | Codec ES8311, ampli loa NS4150B, ES7210 với hai micro (dùng một) |
| Nút | BOOT, ở mặt sau cạnh đầu nối nam châm, và các miếng cảm ứng dưới vỏ (v1.2 có hai, v1.0 có một): giữ cái nào cũng để nói |
| Pin | IC đo pin BQ27220: mức pin, điện áp và trạng thái sạc |

Giới hạn đã biết: BOOT là phím duy nhất firmware đọc được, nên màn hình tắt theo
hẹn giờ, và "tắt nguồn" ở trang Power trong Settings chỉ đưa bo vào ngủ sâu cho
tới khi nhấn BOOT; muốn cắt nguồn thật thì dùng phím nguồn của bo. Miếng cảm ứng
dùng để nói và đánh thức màn hình như BOOT, nhưng không xác nhận ghép đôi với
ứng dụng Muse và không đánh thức bo khỏi ngủ sâu: hai việc đó phải nhấn BOOT.
Miếng cảm ứng bị giữ 20 giây được coi là bị che chứ không phải đang chạm, và
được nhả ra. Cảm biến chuyển động, khe thẻ SD và LED xanh chưa được dùng.
IC đo pin chạy bằng chính viên pin, nên không gắn pin thì không có mức pin. Khi
cắm vào cục sạc (không phải máy tính), bo chỉ hiện "đang sạc" trong lúc còn dòng
chạy vào pin.

Mã nguồn: [`board_echoear.c`](esp32/components/muse/boards/board_echoear.c),
cấu hình build: [`sdkconfig.muse-echoear`](esp32/devices/sdkconfig.muse-echoear).

## Biên dịch và nạp

Các bước giống nhau cho mọi bo. Lấy tên và profile của bo ở
[bảng phía trên](#các-bo-trong-bản-fork-này).

1. Lấy [SDK token](https://gadgets.muse.ai/settings/sdk-tokens) và đọc
   [Điều khoản Gadget SDK](https://gadgets.muse.ai/sdk-terms). Gadget nào cũng
   cần token mới ghép đôi được.
2. Cài ESP-IDF **v6.0.1** (xem [`esp32/README.md`](esp32/README.md)).
3. Biên dịch từ thư mục `esp32/`. Trên macOS hoặc Linux, dùng tên của bo:

   ```sh
   tools/muse/board.sh build ostb
   ```

   Trên Windows, trong ESP-IDF PowerShell, dùng profile của bo:

   ```powershell
   $P = "ostb-3st"
   idf.py -B build-muse-$P -DIDF_TARGET=esp32s3 `
     "-DSDKCONFIG=build-muse-$P/sdkconfig" `
     "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-$P" build
   ```

   Đặt token bằng `idf.py -B build-muse-$P menuconfig`
   (ESP32 Device SDK > Muse Gadgets SDK token), rồi biên dịch lại.
4. Nạp qua USB-C, thay `PORT` bằng cổng của bo:

   ```powershell
   idf.py -B build-muse-$P -p PORT flash
   ```

   Nếu không kết nối được, đưa bo vào chế độ nạp rồi thử lại: giữ **BOOT**,
   nhấn **RESET**, thả **BOOT**. Với bo không có nút RESET, giữ BOOT trong lúc
   cắm cáp.

   Để nạp qua web, tạo một file duy nhất và ghi ở địa chỉ `0x0`:

   ```powershell
   idf.py -B build-muse-$P merge-bin -o muse-gadget-$P-merged.bin
   ```

   Bản build có SDK token của bạn sẽ mang theo token đó, nên hãy cân nhắc trước
   khi đăng file công khai.
5. Trong ứng dụng Muse, bật **Settings > Devices > Developer mode**, thêm thiết
   bị tên `MuseGadget-XXXXXX`, rồi nhấn nút nói của bo khi được hỏi (BOOT trên
   bo 1.85C, bo LCDWIKI và EchoEar, phím + của bo OSTB-3ST).

## Thêm một thiết bị khác

Một bo có màn hình, loa và micro cần một file driver và vài dòng đăng ký. Mỗi
bo trong bản fork này gồm những phần sau:

1. `esp32/components/muse/boards/board_<id>.c` điền vào `muse_board_t`
   (`muse_board.h`): màn hình, cảm ứng, âm thanh, nút, pin và nguồn. Hãy bắt
   đầu từ bo giống bo của bạn nhất.
2. `esp32/devices/sdkconfig.muse-<profile>` chứa cấu hình build: chip, flash,
   PSRAM.
3. `esp32/components/muse/Kconfig`, `CMakeLists.txt` và `idf_component.yml`
   đăng ký bo, kèm component driver nếu cần.
4. `esp32/tools/muse/board.sh`, `ports.py` và `avatar.py` thêm tên ngắn của bo.
5. Tài liệu cũng cần cập nhật: một dòng trong bảng và một mục trong từng
   README, các dòng trong [`esp32/devices/README.md`](esp32/devices/README.md),
   một ảnh trong `doc/image/`, và nguồn tham khảo cùng giấy phép của chúng
   trong [`CREDITS.md`](CREDITS.md).

Giao diện tự co giãn theo màn tròn và màn chữ nhật. Các ảnh ở trên lấy từ trình
mô phỏng trong `esp32/simulator`, sau khi đổi kích thước màn hình trong
`src/sim_board.c` của nó thành kích thước của bo: cách này cho xem trước một
kích thước mới khi chưa có bo trong tay.
[`esp32/devices/AGENTS.md`](esp32/devices/AGENTS.md) (tiếng Anh) có hướng dẫn
đầy đủ, từ thu thập thông tin của bo đến kiểm tra kết quả.

## Phần còn lại của SDK

| | |
|---|---|
| [**ESP32 Device SDK**](esp32) | Kết nối bo ESP32 của bạn với Muse. Gắn thêm màn hình để hiện ảnh, thêm âm thanh vào ra, hoặc nối các cảm biến khác. |
| [**Linux Device SDK**](linux) | Biến chiếc Raspberry Pi hay máy Linux để không thành một Muse gadget. Tự thêm lệnh của bạn để Muse lo việc quản trị hệ thống hoặc hệ Home Assistant. |

Gadget ESP32 và Linux ghép đôi với ứng dụng Muse trên iOS và Android, trong
Settings > Devices. Bật Developer mode ở đó trước, rồi tìm thiết bị có tên bắt
đầu bằng "MuseGadget". Mỗi thư mục có một `README.md` để bắt đầu và một
`AGENTS.md` dành cho các trợ lý lập trình (đều bằng tiếng Anh).

## Cộng đồng

Cộng đồng của dự án gốc trao đổi trên
[Discord](https://discord.gg/3bhjCkZdd6). Với các bo do bản fork này thêm vào,
hãy mở issue trên repo này.

## Giấy phép và ghi công

- Muse Gadget SDK thuộc bản quyền của Meta Platforms, Inc. và các công ty liên
  kết, phát hành theo Giấy phép Apache, Phiên bản 2.0, trong
  [`LICENSE`](LICENSE).
- Các thay đổi trong bản fork này thuộc bản quyền 2026 của ledienbien-ai, theo
  cùng giấy phép đó.
- Bản port 1.85C dựa trên tài liệu và
  [mã mẫu](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C) của
  Waveshare (Apache-2.0) và trên phần board tương ứng của
  [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (MIT).
- Bản port OSTB-3ST lấy chân, bảng khởi tạo màn hình và bảng pin từ mã nguồn
  firmware xiaozhi-esp32 của bo, vốn không ghi giấy phép riêng; xiaozhi-esp32
  theo giấy phép MIT.
- Bản port LCDWIKI 2.8inch dựa trên [trang của LCDWIKI](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display) về bo này, trên
  các file xiaozhi-esp32 của bo, và trên các phép đo công bố trong
  [esphome-es3c28p-light-panel](https://github.com/jvduuren/esphome-es3c28p-light-panel); không chép mã nào từ các nguồn đó.
- Hai file của bản gốc giữ giấy phép riêng: `minimp3.h` (CC0-1.0) và
  `pixel_font.c` (BSD-2-Clause). Các component tải về lúc build theo giấy phép
  của chính chúng.
- Logo khởi động DB_ROBOT là của ledienbien-ai và được dùng tự do, theo cùng
  giấy phép với các thay đổi khác của bản fork này.
- Giấy phép Apache không bao gồm [avatar Jollybot](esp32/avatar), cũng không
  bao gồm tên và nhãn hiệu Meta, Muse và Waveshare.

[`CREDITS.md`](CREDITS.md) (tiếng Anh) có danh sách đầy đủ: từng nguồn, giấy
phép của nguồn đó, các file bản fork này thêm hoặc sửa, và những việc cần giữ
đúng ở các phiên bản sau.
