# Màn hình chờ thời tiết — cách làm

Tính năng: không chạm màn hình 30 giây → gadget tự hiện thời tiết Đồng Xoài;
chạm vào → quay lại màn hình chat Muse. Thiết kế đầy đủ trong `DESIGN.md`,
ảnh phác thảo trong `mockup-320x240.png`.

**Chưa nạp lên board.** Các bước dưới đây là việc Sếp (hoặc Cursor) làm sau
khi duyệt thiết kế.

## File trong thư mục này

| File | Dùng để làm gì |
|---|---|
| `DESIGN.md` | Thiết kế chốt: hành vi, nguồn dữ liệu, UI/UX, bố cục, màu, cỡ chữ |
| `mockup-320x240.png` | Ảnh phác thảo màn hình, đúng tỉ lệ 320×240 |
| `muse_weather.h` / `muse_weather.c` | Module mới: lấy thời tiết Open-Meteo + giờ SNTP |
| `muse_ui_weather.patch` | Patch áp vào fork, đã thử áp sạch lên commit `d2132fd` |
| `README.md` | File này |

## 1. Áp patch vào fork (trên PC, cần git + ESP-IDF v6.0.3)

```sh
git clone https://github.com/ledienbien-ai/muse-gadget-sdk
cd muse-gadget-sdk
git checkout d2132fd            # đúng commit đã kiểm chứng
cp <thư-mục-này>/muse_weather.h <thư-mục-này>/muse_weather.c esp32/components/muse/
git apply <thư-mục-này>/muse_ui_weather.patch
git status --short              # phải thấy 7 file sửa: muse_ui.c, muse_settings(.h/.c),
                                # muse_settings_ui.c, muse_lang.c, muse_app.c, CMakeLists.txt
```

## 2. Build + flash (board LCDWIKI 2.8" ES3C28P)

```sh
cd muse-gadget-sdk/esp32
. ~/esp/esp-idf-v6/export.sh    # ESP-IDF v6.0.3, xem esp32/AGENTS.md trong repo
tools/muse/board.sh build lcd28
tools/muse/board.sh flash lcd28
```

Không muốn cài ESP-IDF thì flash online ở https://dbrobot.vn/firmware.html
**không dùng được** cho bản này — web flasher chỉ có firmware gốc của fork,
không có tính năng thời tiết. Muốn dùng web flasher thì phải build file `.bin`
từ bước trên rồi nạp bằng công cụ flash tay (xem `tools/muse/board.sh flash`).

### Nhiệt độ to 48 px (tùy chọn, khuyến nghị)

Mặc định nhiệt độ hiện ở 28 px (font đã bật sẵn). Muốn to 48 px như mockup:

```sh
idf.py menuconfig   # Component config → LVGL → Font → bật Montserrat 48
```

Code tự chọn 48 px khi font được bật (`#if LV_FONT_MONTSERRAT_48` trong patch),
không cần sửa code thêm.

## 3. Kiểm thử trên board (sau khi flash + pair lại như cũ)

1. Bật board, vào Wi-Fi, đợi 10–20 giây cho lấy dữ liệu + đồng bộ giờ.
2. **Không chạm 30 giây** → màn hình chuyển sang thời tiết: giờ/ngày đúng,
   nhiệt độ, mô tả, độ ẩm, gió. So với app thời tiết trên điện thoại.
3. **Chạm 1 cái** → quay lại màn hình mặt Muse ngay.
4. **Giữ BOOT nói** → Muse trả lời bình thường, không kẹt ở màn thời tiết.
5. Vuốt sang Cài đặt → mục Tắt màn hình → thấy thêm mục chọn thời gian hiện
   thời tiết (Tắt / 15 giây / 30 giây / 1 phút / 2 phút). Đổi sang 15 giây,
   đợi 15 giây xem hiện nhanh hơn.
6. Tắt Wi-Fi trên board (hoặc rút mạng) → màn hình thời tiết vẫn hiện dữ liệu
   cũ + chữ "Dữ liệu cũ", không treo máy.
7. Để yên 120 giây (mặc định tự tắt màn hình) → màn hình tối như cũ; chạm
   vào → sáng lại ở màn hình chat, không phải màn thời tiết.

## 4. Quay lại firmware cũ (nếu không thích)

```sh
cd muse-gadget-sdk
git checkout -- esp32/components/muse/
rm esp32/components/muse/muse_weather.h esp32/components/muse/muse_weather.c
tools/muse/board.sh build lcd28 && tools/muse/board.sh flash lcd28
```

Hoặc flash lại firmware gốc ở https://dbrobot.vn/firmware.html rồi pair lại.

## 5. Nhờ Cursor làm

Copy đoạn này cho Cursor (Cursor đã có sẵn hướng dẫn build live-voice ở
`CURSOR-PROMPT-LIVE-VOICE.md`; đây là việc khác, làm riêng):

> Trong repo fork ledienbien-ai/muse-gadget-sdk commit d2132fd, áp patch
> `muse_ui_weather.patch` và chép `muse_weather.h/.c` vào
> `esp32/components/muse/`, theo `DESIGN.md` và `README.md` trong cùng thư
> mục. Build bằng ESP-IDF v6.0.3: `tools/muse/board.sh build lcd28`.
> Không flash khi chưa có lệnh của Sếp; báo lại kết quả build (thành công hay
> lỗi gì, log lỗi đầy đủ).

## 6. Lưu ý

- Patch chỉ sửa code UI/cài đặt, **không động vào pipeline giọng nói, Wi-Fi,
  Bluetooth, pair, pin**. Rủi ro chính là lỗi biên dịch (sai tên hàm LVGL) —
  build sẽ báo ngay, chưa flash nên board vẫn chạy firmware cũ an toàn.
- `muse_weather.c` gọi Open-Meteo qua HTTPS (`esp_crt_bundle_attach`, mẫu
  đã dùng trong `muse_account_api.c`); không gửi gì khác ra ngoài ngoài
  tọa độ Đồng Xoài trong URL.
- Nếu sau này Sếp muốn đổi vị trí (ví dụ về TP.HCM), sửa 2 số trong
  `muse_weather.h` (`MUSE_WEATHER_LAT/LON`) rồi build lại.
