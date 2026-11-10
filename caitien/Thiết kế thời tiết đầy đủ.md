# Màn hình chờ thời tiết — ESP32 Muse Gadget (LCDWIKI 2.8" ES3C28P)

**Trạng thái:** Thiết kế + code sẵn, chờ Sếp duyệt rồi mới nạp lên board.
**Firmware gốc:** fork `ledienbien-ai/muse-gadget-sdk`, commit `d2132fd` (đã kiểm chứng
ngày 2026-11-11). **Không flash, không sửa repo fork** trong bước này.
**File trong thư mục này:** `DESIGN.md` (bản này) · `muse_weather.h` · `muse_weather.c` ·
`muse_ui_weather.patch` (patch áp vào fork) · `README.md` (cách áp patch, build, flash, test) ·
`mockup-320x240.png` (ảnh phác thảo màn hình, đúng tỉ lệ 320×240).

---

## 1. Yêu cầu của Sếp

> Nếu một lúc không chạm vào màn hình thì gadget tự chuyển sang hiển thị thời tiết;
> chạm vào thì quay lại màn hình chat Muse.

## 2. Thiết kế chốt

| Hạng mục | Chốt |
|---|---|
| Thời gian chờ trước khi hiện thời tiết | **30 giây** mặc định, chỉnh được trong Cài đặt (Tắt / 15s / 30s / 60s / 120s) |
| Quay lại màn hình chat | **Chạm bất cứ đâu** trên màn hình, hoặc giữ nút BOOT để nói, hoặc có tin nhắn mới từ Muse — đều quay lại ngay |
| Vị trí thời tiết | **Đồng Xoài, Bình Phước** — 11.5188, 106.9073 (vị trí Sếp đang ở) |
| Nguồn dữ liệu | **Open-Meteo** (`api.open-meteo.com`) — miễn phí, **không cần API key**, không cần tài khoản |
| Cập nhật | Mỗi **15 phút**, chỉ khi có Wi-Fi; lần đầu lấy ngay khi khởi động xong |
| Nội dung hiển thị | Giờ + ngày, nhiệt độ lớn, mô tả thời tiết, cảm giác như, độ ẩm, gió, giờ cập nhật |
| Ngôn ngữ | **Tiếng Việt** (dùng đúng bảng chuỗi + font tiếng Việt có sẵn của fork) |
| Màn hình | **320×240 ngang** (xem lưu ý quan trọng ở mục 3) |

### 2.1. Hành vi chi tiết (state machine)

```
Màn hình chat (Muse) ── không chạm 30s, đang IDLE, có Wi-Fi ──▶ Màn hình thời tiết
Màn hình thời tiết ── chạm / BOOT / Muse bắt đầu nói / có menu / có ảnh ──▶ Màn hình chat
Màn hình thời tiết ── hết giờ tự tắt màn hình (auto-sleep) ──▶ Màn hình tối (như cũ)
```

Quy tắc hiển thị thời tiết (tất cả phải đúng cùng lúc):

1. Chế độ là `MUSE_MODE_IDLE` (không nghe, không nghĩ, không nói).
2. Màn hình đang sáng (`!muse_state_asleep()`).
3. Số giây không chạm `muse_state_idle_secs() >= weather_idle_s` (mặc định 30).
4. Không có menu, không có ảnh `display.draw_url`, không có bảng ghép đôi đang mở.
5. Tính năng đang bật (`weather_idle_s > 0`).

Quy tắc ẩn ngay lập tức khi: bất kỳ lần chạm nào (`on_any_press` đã có sẵn),
nút BOOT được bấm, chế độ đổi khỏi IDLE, menu mở, ảnh hiện lên, màn hình tự tắt.

### 2.2. Ràng buộc quan trọng với auto-sleep (đã có sẵn)

Fork đã có tự tắt màn hình sau khi nghỉ: `muse_settings_sleep_s()`, mặc định **120 giây**
(`muse_input.c: check_sleep()` — IDLE và `idle_secs > sleep_s` thì tắt màn hình).

Vì vậy **thời gian chờ thời tiết phải nhỏ hơn thời gian tự tắt màn hình**,
nếu không màn hình tắt trước khi kịp hiện thời tiết. Mặc định 30s < 120s nên an toàn.
Nếu Sếp chỉnh tự tắt màn hình xuống 30s mà vẫn để thời tiết 30s thì thời tiết
sẽ không bao giờ hiện — code tự xử lý: thời gian chờ hiệu dụng =
`min(weather_idle_s, sleep_s - 5)` khi `sleep_s > 0`; `sleep_s = 0` (không bao giờ
tắt) thì không giới hạn. Ghi rõ trong Cài đặt để Sếp khỏi thắc mắc.

### 2.3. Dữ liệu thời tiết — Open-Meteo (đã kiểm chứng trực tiếp)

URL gọi (GET, không key):

```
https://api.open-meteo.com/v1/forecast?latitude=11.5188&longitude=106.9073
  &current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day
  &timezone=Asia%2FHo_Chi_Minh
```

Kiểm chứng lúc 01:00 +07 ngày 2026-11-11, trả về thật:

```json
{"current": {"time": "2026-11-11T01:00", "temperature_2m": 23.9,
  "relative_humidity_2m": 89, "weather_code": 0, "wind_speed_10m": 12.8, "is_day": 0}}
```

→ Đồng Xoài lúc đó 23,9 °C, ẩm 89%, trời quang, gió 12,8 km/h, ban đêm.
`weather_code` là mã WMO, dịch sang tiếng Việt theo bảng ở `muse_weather.c`
(0 Trời quang, 1 Ít mây, 2 Có mây, 3 Nhiều mây, 45/48 Sương mù, 51/53/55 Mưa phùn,
56/57 Mưa phùn lạnh, 61/63/65 Mưa, 66/67 Mưa lạnh, 71/73/75/77 Tuyết, 80/81/82
Mưa rào, 85/86 Tuyết rào, 95 Dông, 96/99 Dông có mưa đá).

Xử lý lỗi: mất mạng / API lỗi → giữ **dữ liệu cũ** và hiện "Dữ liệu cũ" nhỏ ở
góc dưới; chưa từng lấy được lần nào → hiện "Đang tải thời tiết…" rồi
"Không có mạng, chạm để quay lại". Không bao giờ làm treo UI: việc gọi mạng chạy
ở task riêng, UI chỉ đọc bản cache.

### 2.4. Giờ và ngày — cần thêm SNTP (firmware gốc chưa có đồng hồ)

Firmware gốc **không có giờ hệ thống** (chỉ dùng `esp_timer` đếm giây từ lúc boot).
Để hiện giờ/ngày thật phải thêm đồng bộ giờ SNTP: `esp_netif_sntp_init()` với
`pool.ntp.org`, múi giờ `ICT-7` (Asia/Ho_Chi_Minh). Code đã gồm sẵn trong
`muse_weather.c` (`muse_weather_time_init()`). Nếu chưa đồng bộ được giờ thì
đồng hồ hiện `--:--` chứ không hiện giờ sai.

---

## 3. Lưu ý quan trọng: màn hình là 320×240 NGANG

Sếp nói "màn 240x320" nhưng board dùng **ngang 320×240** (file
`boards/board_lcdwiki_s3_28.c`: `LCD_W 320`, `LCD_H 240`, chú thích
*"240x320 ILI9341V LCD on SPI, used in landscape (320x240)"*). Toàn bộ bố cục
dưới đây thiết kế theo **320 rộng × 240 cao, màn hình ngang**. Vì cao chỉ 240 px
nên fork dùng layout gọn (`s_short = true`): không có vòng tròn, chữ caption ở
dưới — màn hình thời tiết cũng theo tinh thần gọn đó.

---

## 4. UI/UX — thiết kế cho đẹp

### 4.1. Nguyên tắc

- **Hài hòa với giao diện Muse có sẵn:** nền đen, chữ trắng/tím nhạt, điểm nhấn
  tím `#a77dff` — đúng bảng màu của `muse_ui.c`, không thêm màu lạ.
- **Đọc được từ xa, trong 1 cái liếc mắt:** nhiệt độ là thứ to nhất màn hình;
  mọi chữ tiếng Việt dùng font có dấu chuẩn của fork (không dùng font ASCII
  cho chữ có dấu — fork đã cảnh báo `unscii_8` không có chữ Việt).
- **Không nhồi nhét:** chỉ 7 thông tin (giờ, ngày, nhiệt độ, mô tả, cảm giác như,
  độ ẩm, gió) + 1 dòng gợi ý chạm để quay lại. Không hiện mã code, không hiện
  tên API, không hiện địa chỉ IP.
- **Icon vẽ bằng hình khối LVGL** (tròn, vạch, cung), không dùng emoji — font
  trên thiết bị không có glyph emoji, vẽ khối luôn nét và đồng bộ màu.

### 4.2. Bảng màu (lấy đúng từ `muse_ui.c`)

| Dùng cho | Màu | Mã |
|---|---|---|
| Nền màn hình | Đen | `#000000` |
| Chữ chính / nhiệt độ | Trắng sáng | `#f2efff` (`COLOR_LIT`) |
| Mô tả thời tiết | Tím nhấn | `#a77dff` (`COLOR_ACCENT`) |
| Chữ phụ (độ ẩm, gió, gợi ý) | Tím xám | `#8b84a8` (`COLOR_DIM`) |
| Chữ caption | Tím nhạt | `#d8d2ff` (`COLOR_CAPTION`) |
| Đường chia | Tím tối | `#3a3358` (`COLOR_DOT_OFF`) |
| Mặt trời | Vàng | `#ffd34d` |
| Mây | Trắng tím | `#cfc9e8` |
| Mưa / sấm | Xanh dương | `#6db3ff` |

### 4.3. Cỡ chữ (font đã có sẵn trong fork)

| Nội dung | Cỡ | Font |
|---|---|---|
| Nhiệt độ `24°` | **48 px** nếu bật `LV_FONT_MONTSERRAT_48` trong menuconfig, không thì 28 px (code tự chọn bằng `#if LV_FONT_MONTSERRAT_48`) | `lv_font_montserrat_48/28` — chỉ số và `°`, không cần dấu tiếng Việt |
| Giờ `01:00` | 20 px | `muse_font(20)` (có dấu Việt) |
| Mô tả `Trời quang` | 20 px | `muse_font(20)` |
| Ngày, độ ẩm, gió, giờ cập nhật | 14–16 px | `muse_font(14)` / `muse_font(16)` |
| Địa điểm, gợi ý chạm | 14 px | `muse_font(14)` |

Font tiếng Việt của fork chỉ có 14/16/20 px (`muse_font_montserrat_vi_*`); xin
28 px cho chữ có dấu sẽ rớt về font không dấu — vì vậy **mọi chữ tiếng Việt
chỉ dùng 14/16/20 px**, số nhiệt độ mới dùng font lớn.

### 4.4. Bố cục chi tiết (tọa độ px, gốc trên-trái, màn 320×240)

Lề trái/phải 16 px, các khối cách nhau 8 px, căn lề trái thống nhất — không
căn giữa lung tung (trừ dòng gợi ý cuối và cụm icon).

```
y=12   ┌────────────────────────────────────────────────────────┐
       │ ĐỒNG XOÀI, BÌNH PHƯỚC                    01:00        │
y=34   │                                         Thứ Tư, 11/11  │  ← ngày, 14px, dim
       │                                                        │
y=56   │  24°C                                   (icon 76×76)  │  ← nhiệt độ 48px, trắng
       │  ┌─────────────────────────┐           ┌──────────┐    │
       │  │  Trời quang             │           │   ☀      │    │
y=118  │  │  (20px, tím nhấn)       │           │  mặt trời│    │
y=144  │  │  Cảm giác như 26°       │           └──────────┘    │
       │  └─────────────────────────┘                          │
y=168  │ ─────────────── đường chia 1px ─────────────────────── │
       │   ĐỘ ẨM          GIÓ            CẬP NHẬT             │
y=182  │   89%           13 km/h         01:00                │  ← nhãn 14px dim, số 16px trắng
       │                                                        │
y=224  │         Chạm để quay lại trò chuyện                    │  ← 14px, dim, căn giữa
       └────────────────────────────────────────────────────────┘
```

Bảng tọa độ chính xác (dùng trong code):

| Khối | Vị trí | Cỡ | Ghi chú |
|---|---|---|---|
| Địa điểm `ĐỒNG XOÀI, BÌNH PHƯỚC` | x=16, y=12 | 14 px, dim, giãn chữ 2 | Viết hoa, gọn |
| Giờ `01:00` | căn phải x=304, y=10 | 20 px, trắng sáng | Đồng hồ thật (SNTP) |
| Ngày `Thứ Tư, 11/11` | căn phải x=304, y=34 | 14 px, dim | Thứ + ngày/tháng |
| Nhiệt độ `24°` + `°C` nhỏ | x=16, y=56 | 48 px (28 nếu chưa bật font) | To nhất màn hình |
| Mô tả `Trời quang` | x=16, y=118 | 20 px, tím nhấn | Dịch từ mã WMO |
| Cảm giác như `Cảm giác như 26°` | x=16, y=144 | 14 px, dim | `apparent_temperature` |
| Icon thời tiết | vùng 76×76, tâm (262, 100) | vẽ bằng hình khối | Nắng/mây/mưa/sấm/sương mù |
| Đường chia | x=16..304, y=168 | dày 1 px, `#3a3358` | |
| 3 ô thống kê | tâm x=64 / 160 / 256, y=182–214 | nhãn 14 px dim, số 16 px trắng | Độ ẩm / Gió / Cập nhật |
| Gợi ý chạm | căn giữa, y=224 | 14 px, dim | `Chạm để quay lại trò chuyện` |
| Trạng thái dữ liệu cũ | góc dưới phải, cạnh giờ cập nhật | 14 px, vàng nhạt | Chỉ hiện khi dữ liệu > 60 phút |

### 4.5. Icon thời tiết (vẽ bằng hình khối LVGL)

| Trời | Vẽ như thế nào |
|---|---|
| Trời quang (ngày) | Tròn vàng `#ffd34d` Ø 34 px + 8 tia ngắn xung quanh |
| Trời quang (đêm) | Trăng khuyết: tròn `#cfc9e8` Ø 30 px + tròn đen đè lệch 10 px |
| Ít mây | Mặt trời nhỏ góc trên + 1 đám mây trắng tím phía dưới |
| Nhiều mây / âm u | 3 tròn `#cfc9e8` xếp chồng thành đám mây, không có mặt trời |
| Sương mù | Đám mây + 3 vạch ngang `#8b84a8` bên dưới |
| Mưa / mưa phùn | Đám mây + 3 vạch chéo xanh `#6db3ff` |
| Dông | Đám mây xám + tia sét vàng hình ziczac |
| Tuyết | Đám mây + 3 dấu `×` trắng |

Code vẽ icon nằm trong `muse_weather.c` (`weather_draw_icon()`), dùng
`lv_obj` tròn/vạch cơ bản — không cần thêm ảnh hay font icon.

### 4.6. Mockup

**Ảnh phác thảo đúng tỉ lệ 320×240:** `mockup-320x240.png` (mở lên xem là
hình dung được ngay). Dữ liệu trong ảnh là dữ liệu thật đo được lúc 01:00
ngày 2026-11-11: 24 °C, trời quang, ẩm 89%, gió 13 km/h.

**Phác thảo bằng text** (khung 64 ký tự ≈ tỉ lệ màn hình):

```
┌────────────────────────────────────────────────────────────────┐
│ ĐỒNG XOÀI, BÌNH PHƯỚC                              01:00     │
│                                                   Thứ Tư, 11/11 │
│                                                                │
│   24°C                                          ╔══════╗       │
│                                                ║  \│/  ║       │
│   Trời quang                                   ║ ──☀── ║       │
│                                                ║  /│\  ║       │
│   Cảm giác như 26°                              ╚══════╝       │
│                                                                │
│ ────────────────────────────────────────────────────────────── │
│    ĐỘ ẨM           GIÓ            CẬP NHẬT                   │
│    89%            13 km/h          01:00                      │
│                                                                │
│              Chạm để quay lại trò chuyện                       │
└────────────────────────────────────────────────────────────────┘
```

---

## 5. Cách gắn vào code fork (tóm tắt — chi tiết trong patch)

Vị trí chèn đã xác định bằng cách đọc code fork commit `d2132fd`:

1. **Màn hình mới = overlay, không phải tile thứ 3.** UI hiện tại là `lv_tileview`
   2 tile: mặt Muse (0,0) + Cài đặt (1,0) (`muse_ui.c: build_screen()`).
   Thêm tile thứ 3 sẽ đụng vuốt sang Cài đặt. Vì vậy màn thời tiết là một
   `lv_obj` toàn màn hình tạo trong `build_overlays()` (cùng chỗ với `s_image`,
   `s_cover`), mặc định ẩn (`LV_OBJ_FLAG_HIDDEN`), hiện/ẩn bằng
   `lv_obj_clear_flag / lv_obj_add_flag`.
2. **Bắt sự kiện chạm:** đã có sẵn `on_any_press()` gắn vào indev
   (`LV_EVENT_PRESSED` → `muse_state_poke()`, `muse_ui.c` cuối `build_overlays()`).
   Thêm 1 dòng trong đó: nếu màn thời tiết đang hiện thì ẩn nó đi. Không cần
   đăng ký touch riêng.
3. **Đếm thời gian không chạm:** đã có sẵn `muse_state_idle_secs()` và
   `muse_state_poke()` (`muse_state.c`) — mọi chạm/bấm đều poke. Không cần
   timer riêng.
4. **Vòng lặp quyết định hiện/ẩn:** trong `frame_tick()` (`muse_ui.c`, timer LVGL
   chạy mỗi frame), sau `update_sleep()`: gọi `muse_weather_tick(now)` — hiện
   khi đủ điều kiện (mục 2.1), ẩn khi hết điều kiện. Khi chế độ đổi sang
   `MUSE_MODE_LISTENING` thì `frame_tick` đã gọi `muse_ui_show_face()` — màn
   thời tiết cũng ẩn ở đó.
5. **Tự tắt màn hình:** không sửa `check_sleep()` trong `muse_input.c` — nó vẫn
   tắt màn hình theo `sleep_s` như cũ; màn thời tiết chỉ là một trạng thái hiển
   thị trước khi tắt.
6. **Cài đặt mới:** thêm `weather_idle_s` vào `muse_settings.c` theo đúng mẫu
   `sleep_s` (struct + default 30 + NVS key `"weather_idle_s"` + getter/setter
   + `MUSE_SETTING_WEATHER`), và thêm trang chọn trong `muse_settings_ui.c`
   theo mẫu trang Sleep (`SLEEP_CHOICES` → `WEATHER_CHOICES = {0, 15, 30, 60, 120}`).
   Thêm chuỗi tiếng Việt vào bảng `muse_lang.c` (mẫu `"Sleep" → "Tắt màn hình"`).
7. **Module mới `muse_weather.c/.h`:** task FreeRTOS riêng lấy dữ liệu
   (theo mẫu `esp_http_client` + `cJSON` đã dùng trong `muse_account_api.c`),
   cache trong RAM, hàm `muse_weather_start()`, `muse_weather_tick()`,
   `muse_weather_show()/hide()`, `muse_weather_draw_icon()`. Thêm 2 file vào
   danh sách `srcs` trong `components/muse/CMakeLists.txt` (danh sách liệt kê
   tường minh, không glob). `esp_http_client` và `nvs_flash` đã có trong
   `PRIV_REQUIRES` nên không cần thêm thư viện.
8. **Giờ hệ thống:** `muse_weather_time_init()` (SNTP + `TZ=ICT-7`) gọi trong
   `muse_app_run()` sau `muse_ui_start()`.

## 6. Các mặc định Sếp có thể đổi (ghi rõ để duyệt)

| Mục | Mặc định | Đổi ở đâu |
|---|---|---|
| Thời gian chờ | 30 giây | Cài đặt trên màn hình (vuốt sang Cài đặt → Thời tiết) |
| Vị trí | Đồng Xoài 11.5188, 106.9073 | `muse_weather.h`: `MUSE_WEATHER_LAT/LON` |
| Cập nhật thời tiết | 15 phút | `muse_weather.h`: `MUSE_WEATHER_REFRESH_S` |
| Nguồn dữ liệu | Open-Meteo | Chỉ đổi URL trong `muse_weather.c` nếu muốn nguồn khác |
| Ngôn ngữ | Theo ngôn ngữ màn hình (mặc định tiếng Việt) | Chuỗi trong `muse_lang.c` |
| Nhiệt độ lớn | 48 px (cần bật font) / 28 px | `idf.py menuconfig` → LVGL → Font → Montserrat 48 |

## 7. Giới hạn đã biết

- Thời tiết chỉ hiện khi có Wi-Fi và đang ở chế độ rảnh; đang nghe/nói thì
  không hiện (đúng ý Sếp: chạm để quay lại chat).
- Lần đầu sau khi flash cần vài giây để lấy dữ liệu + đồng bộ giờ; trước đó
  hiện "Đang tải thời tiết…".
- Open-Meteo là dịch vụ miễn phí, không cam kết uptime; khi lỗi thì hiện dữ
  liệu cũ, không treo máy.
- Tính năng này chưa bật "nói liên tục" (realtime voice) — đó là việc khác,
  đang có file hướng dẫn riêng cho Cursor.
