# Lệnh đọc to `device.speak` — thiết kế và cách dùng cho bản tin sáng

Tính năng: Muse (phía VM) ra lệnh cho gadget **nói thành tiếng** một đoạn
văn — để đọc bản tin buổi sáng, nhắc việc, hay báo một tin quan trọng — mà
người dùng không cần chạm vào thiết bị. Gadget đã có sẵn TTS Google
(`muse_tts`) vẫn đọc các câu trả lời chat; lệnh này mở đường cho Muse chủ
động nói trước.

Mã nguồn nằm trong fork ở các file: `esp32/main/app.c` (xử lý lệnh),
`esp32/main/noise_control.cpp` (khai báo lệnh trong `link.register`),
`esp32/components/muse/muse_chat_session.cpp` + `muse_chat.h`
(`muse_hatch_announce`), `muse_tts_text.c` (kiểm tra văn bản). Kiểm thử
host: `esp32/tests/test_muse_tts_announce.py` và case 31 trong
`tests/test_muse_chat_session.py`.

## Vì sao tên là `device.speak`

Bảng lệnh hiện có phân nhóm theo *ai làm việc*: `device.*` là hành động của
chính thiết bị (`device.health`, `device.ota`, `device.discover`),
`display.*` là vẽ lên màn, `camera.*` là chụp hình. Nói thành tiếng là việc
của thiết bị, không phải nội dung hiển thị, nên `device.speak` nhất quán
với bảng lệnh và dễ đoán cho phía VM.

## Lệnh

Gửi như mọi `link.invoke` khác trên phiên Link:

```json
{
  "method": "link.invoke",
  "id": "speak-0700-2026-11-15",
  "command": "device.speak",
  "params": { "text": "Chào buổi sáng anh. Hôm nay Đồng Xoài nắng, 31 độ...", "lang": "vi" }
}
```

| Tham số | Bắt buộc | Ý nghĩa |
|---|---|---|
| `text` | có | Văn bản thuần cần nói. Được làm sạch như một câu trả lời chat trước khi đọc (bỏ Markdown, link, emoji). **Tối đa 1000 byte sau khi làm sạch** — văn bản tiếng Việt tính theo byte UTF-8, khoảng 500 ký tự có dấu. |
| `lang` | không | Thẻ ngôn ngữ đọc, ví dụ `"vi"`, `"en"` (tối đa 7 ký tự). Bỏ trống: văn bản có chữ tiếng Việt đọc giọng Việt, còn lại đọc giọng Anh; các đoạn Trung/Nhật/Hàn vẫn tự nhận diện như khi đọc câu trả lời. |

## Kết quả

Lệnh trả lời bất đồng bộ (kiểu `_async` như `camera.capture`): kết quả về
khi gadget **đã nhận lời**, không phải khi đã nói xong (một bản tin dài nói
cả phút).

Thành công:

```json
{ "ok": true, "payload": { "queued": true, "spoken": true, "lang": "vi" } }
```

- `queued: true` — văn bản đã được phiên thoại giữ, sẽ hiện và đọc khi
  thiết bị rảnh.
- `spoken` — tại thời điểm trả lời, **công tắc loa trong Settings** đang
  bật (`true`: sẽ đọc thành tiếng) hay tắt (`false`: chỉ hiện chữ trên màn
  hình theo nhịp đọc, không phát tiếng). Đây là cách phía VM biết "đã nói"
  hay "chỉ đã hiện".

Lỗi (`ok: false`, `error.code`):

| Mã | Khi nào |
|---|---|
| `missing_param` | Thiếu `text` hoặc `text` rỗng. |
| `invalid_params` | `lang` dài hơn một thẻ ngôn ngữ ngắn. |
| `busy` | Đang có cuộc gọi realtime, đang ghi âm ghi chú thoại, hoặc đang phát một câu trả lời. Lệnh **không nói chồng** — phía VM nên thử lại sau ít phút. |
| `nothing_to_say` | Sau khi làm sạch (bỏ emoji/Markdown) văn bản không còn gì để nói. |
| `text_too_long` | Văn bản sau làm sạch vượt 1000 byte. Chia nhỏ hoặc tóm tắt lại. |
| `unavailable` | Bản build không có TTS Google (`CONFIG_MUSE_TTS_GOOGLE` tắt), không có phiên thoại riêng (`CONFIG_MUSE_HATCH` tắt), phiên thoại chưa chạy, hoặc đã có một thông báo khác đang chờ. |
| `out_of_memory` | Không cấp phát được bộ nhớ cho tác vụ xử lý. |
| `unsupported` | Bản build Link-only (không `CONFIG_MUSE_ENABLED`): lệnh không tồn tại. |

Hành vi đáng biết:

- Tối đa **một** thông báo chờ tại một thời điểm; thông báo thứ hai trong
  lúc một cái đang chờ nhận `unavailable`.
- Thiết bị đang ngủ trên pin (resting) thì thông báo được giữ và phát khi
  thiết bị thức dậy — cùng quy tắc với câu trả lời đến muộn. Để bàn cắm USB
  thì phát gần như ngay lập tức.
- Đang có một lượt chat chờ phát (câu trả lời đến muộn) thì câu trả lời đó
  phát trước, thông báo phát sau.

## Bản tin sáng 07:00 — phần lịch nằm ở phía Muse

Firmware **không** giữ lịch; nó chỉ biết nói khi được lệnh. Việc "7 giờ
sáng mỗi ngày" thuộc về phía Muse của người dùng (VM/agent), nơi đã có
cơ chế tác vụ hẹn giờ:

1. 07:00, tác vụ hẹn giờ của Muse tổng hợp bản tin: thời tiết Đồng Xoài
   (module thời tiết/Open-Meteo đã có), lịch hôm nay, việc cần nhắc — thành
   một đoạn văn thuần **dưới 1000 byte sau làm sạch**, tiếng Việt.
2. Muse gửi `link.invoke` `device.speak` với `lang: "vi"` tới gadget đã
   ghép nối (đúng khung JSON ở trên).
3. Đọc kết quả: `ok` + `spoken: true` là đã nhận và sẽ đọc; `busy` thì hẹn
   thử lại sau 3–5 phút (người dùng có thể đang gọi điện); `unavailable`
   kéo dài thì báo lại trong chat thay vì im lặng.

Ví dụ một bản tin đạt:

> "Chào buổi sáng anh. Thứ bảy, 15 tháng 11. Đồng Xoài hôm nay nắng, cao
> nhất 31 độ, chiều có thể mưa rào. Anh có họp lúc 9 giờ với khách, và hạn
> nộp báo cáo là 17 giờ. Chúc anh một ngày tốt lành."

(~340 byte sau làm sạch — thoải mái trong giới hạn.)
