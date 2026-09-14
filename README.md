# Makita LXT Battery Monitor — ESP32-C3 Super Mini + Wi-Fi Web UI

Bản chuyển từ `src/main.cpp` của [synrais/Makita-LXT-Battery-Monitor-Unlocker](https://github.com/synrais/Makita-LXT-Battery-Monitor-Unlocker)
sang **ESP32-C3 Super Mini**, giữ nguyên toàn bộ phần giao tiếp 1-Wire và giải mã Makita,
bổ sung **Wi-Fi AP + web server + giao diện điện thoại (dark theme)**.

- Wi-Fi: `Makita Battery Monitor` — **không mật khẩu**
- Truy cập: <http://192.168.4.1> (điện thoại thường tự mở trang này nhờ captive portal)
- Firmware: `2.0.0-c3web`

---

## 1. Đấu nối phần cứng

| ESP32-C3 Super Mini | Nối tới | Ghi chú |
|---|---|---|
| GPIO2  | DATA của pin Makita | **bắt buộc** trở kéo 4.7 kΩ lên 3V3, nên có 120 Ω nối tiếp |
| GPIO1  | ENABLE của pin Makita | **bắt buộc** trở kéo 4.7 kΩ lên 3V3, nên có 120 Ω nối tiếp |
| GPIO10 | DIN của NeoPixel (WS2812) | cấp nguồn LED bằng 3V3 hoặc 5V |
| GPIO5  | ngõ ra chọn chế độ | luôn ở mức HIGH |
| GPIO7  | ngõ vào chọn chế độ | có kéo xuống bên trong; **nối tắt GPIO5–GPIO7 = chế độ Khóa Omega** |
| GND    | GND của pin | phải chung mass |

Lưu ý an toàn:

- Tuyệt đối **không** đưa 5 V vào chân GPIO của ESP32-C3.
- GPIO2 là chân strapping: lúc khởi động phải ở mức cao. Trở kéo 4.7 kΩ đảm bảo điều này,
  vì vậy đừng bỏ trở kéo, và nếu đường DATA bị kéo xuống lúc cấp nguồn thì module sẽ không boot.
- Một số pack cần tải mới “thức dậy”: có thể mắc thêm trở 1 kΩ ngang hai cực nguồn của pin.
- Không cấp nguồn cho ESP32 từ cực chính của pin 18 V; dùng cổng USB-C.

Sơ đồ rút gọn:

```
                3V3
                 |     |
                4k7   4k7
                 |     |
GPIO2 --120R-----+------------ DATA   (pin Makita)
GPIO1 --120R-----------+------ ENABLE (pin Makita)
GND -------------------------- GND
GPIO10 ----------------------- DIN (NeoPixel)
GPIO5  --- (nối tắt để vào chế độ Omega) --- GPIO7
```

---

## 2. Nạp firmware

### Cách A — PlatformIO (khuyên dùng)

```bash
pip install platformio          # nếu chưa có
cd makita-c3-web
pio run                         # biên dịch
pio run -t upload               # nạp (cắm USB-C, board tự vào chế độ nạp)
pio device monitor              # xem log serial 115200
```

Nếu board không tự vào bootloader: giữ **BOOT**, nhấn nhả **RESET**, thả **BOOT**, rồi nạp lại.

### Cách B — nạp file .bin có sẵn

Trong thư mục `dist/`:

- `makita-c3-web-merged.bin` — nạp **một file duy nhất tại offset 0x0**
  (dùng được với <https://espressif.github.io/esp-launchpad/> hoặc esptool).

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 write_flash 0x0 dist/makita-c3-web-merged.bin
```

- `firmware.bin` — chỉ phần ứng dụng, nạp ở offset `0x10000`.

---

## 3. Sử dụng

1. Cấp nguồn USB cho ESP32-C3, chờ ~2 giây.
2. Trên điện thoại, vào Wi-Fi và kết nối `Makita Battery Monitor` (không mật khẩu).
3. Điện thoại sẽ tự mở trang; nếu không, mở trình duyệt và gõ `192.168.4.1`.
4. Gắn pin Makita vào jig → thiết bị **tự động quét** và hiển thị đầy đủ thông số.
5. Rút pin ra, gắn pin khác → tự quét lại (hot-swap).

> Android/iOS có thể báo “mạng không có Internet”. Chọn **giữ kết nối / vẫn dùng mạng này**.

### Giao diện hiển thị

- Điện áp tổng, trạng thái khóa
- Model, Type (0/2/3/5/6), cấu hình cell, dung lượng, ngày sản xuất, ROM ID, xuất xứ
- Điện áp từng cell (thanh bar, đánh dấu cell cao nhất ↑ / thấp nhất ↓), độ chênh lệch cell
- Sức khỏe (health 0–4), số chu kỳ sạc, mức pin SOC, nhiệt độ cell & mosfet, số lần xả kiệt / quá tải
- Mã lỗi, lỗi cell, nybble 34, byte 19, 5 checksum (CS0/CS1/CS2/AUX/AUX) và **lý do bị khóa**
- Khung dữ liệu thô 32 byte
- Nhật ký giao tiếp (giống hệt log serial)

### Các nút

| Nút | Tác dụng | Ghi vào BMS? |
|---|---|---|
| **Quét lại** | đọc lại toàn bộ thông số | không |
| **Mở khóa** | reset lỗi DA04 + sửa nybble 34/CS0/CS2 | **có** |
| **Khóa Omega** | ghi nybble 34 = 4 để sạc từ chối pin | **có** |
| Tự động mở khóa | tự mở khóa ngay khi phát hiện pin khóa | **có**, khi bật |

**Mặc định thao tác quét chỉ ĐỌC, không ghi gì vào BMS.** Hai nút ghi đều có hộp thoại xác nhận.
Mở khóa chỉ hỗ trợ Type 0/2/3 (Type 5 chỉ đọc, Type 6 không hỗ trợ).

### NeoPixel

Giữ nguyên như bản gốc: nhấp nháy trắng khi đang quét, xanh lá = OK/đã mở khóa,
đỏ = lỗi / không phản hồi, nháy khi cell lệch quá ngưỡng, chế độ Omega dùng màu đỏ.

---

## 4. Khác biệt so với bản gốc

- Kiến trúc ESP32 FreeRTOS: tác vụ `batt` chạy giao tiếp 1-Wire, tác vụ `led` điều khiển NeoPixel,
  `loop()` phục vụ HTTP/DNS → giao diện không bị đứng khi đang đọc pin.
- Watchdog RP2040 → `esp_task_wdt`.
- Log serial được nhân đôi vào bộ đệm vòng và đẩy lên web (`/api/state`).
- Quét mặc định **read-only**; hành vi tự động mở khóa của bản gốc trở thành tùy chọn.
- Khung 32 byte chỉ được chấp nhận khi **hai lần đọc liên tiếp giống hệt nhau**
  (chống nhiễu timing do ngắt Wi-Fi), tối đa 4 lần thử.

## 5. API

| Endpoint | Mô tả |
|---|---|
| `GET /api/state?since=<seq>` | JSON toàn bộ trạng thái + log mới |
| `GET|POST /api/cmd?a=scan` | quét lại |
| `GET|POST /api/cmd?a=unlock` | mở khóa |
| `GET|POST /api/cmd?a=omega` | khóa Omega |
| `GET|POST /api/cmd?a=autounlock&v=0|1` | bật/tắt tự mở khóa |
| `GET|POST /api/cmd?a=clearlog` / `a=clearscan` | xóa log / xóa kết quả |

## 6. Xem thử giao diện không cần phần cứng

```bash
python3 tools/mock_server.py      # http://127.0.0.1:8080
```

Script này lấy đúng trang HTML nhúng trong `src/main.cpp` và phục vụ dữ liệu giả.

## 7. Xử lý sự cố

### Wi-Fi bật vài giây rồi mất / không kết nối được

Đây là dấu hiệu board **đang khởi động lại liên tục**. Cách xác định nguyên nhân:

1. Cắm USB, chạy `pio device monitor` (115200). Mỗi 5 giây firmware in một dòng
   `[hb] up=... heap=... clients=...`. Nếu số `up=` nhảy về 0 → board reboot.
2. Dòng `Reset reason` ngay sau khi khởi động cho biết lý do:
   - `BROWNOUT` → sụt áp: đổi cáp USB ngắn/dày hơn, đổi cổng/củ sạc,
     tháo NeoPixel ra thử. (Firmware đã giảm công suất phát Wi-Fi xuống 11 dBm để đỡ tốn dòng.)
   - `TASK_WDT` / `INT_WDT` → treo trong lúc giao tiếp 1-Wire.
   - `PANIC` → có backtrace ngay phía trên, gửi nguyên đoạn log.
3. Các bản chẩn đoán trong `dist/` (đều nạp ở offset `0x0`), nạp lần lượt để khoanh vùng:

| File | Nội dung | Ý nghĩa nếu ỔN ĐỊNH |
|---|---|---|
| `makita-c3-wifitest-merged.bin` | chỉ Wi-Fi + web | nguồn/Wi-Fi tốt, lỗi ở phần pin/LED |
| `makita-c3-ledtest-merged.bin` | Wi-Fi + NeoPixel | NeoPixel không phải thủ phạm |
| `makita-c3-batttest-merged.bin` | Wi-Fi + đọc pin, tắt NeoPixel | thủ phạm là NeoPixel/GPIO10 |
| `makita-c3-web-merged.bin` | bản đầy đủ | hoạt động bình thường |

Tương ứng khi build bằng PlatformIO: `-e esp32c3_wifitest`, `-e esp32c3_ledtest`,
`-e esp32c3_batttest`, `-e esp32c3_supermini`.

Đã sửa (v2.0.1): trước đây thư viện Adafruit NeoPixel cài/gỡ driver RMT ở **mỗi lần**
cập nhật LED, chạy liên tục cạnh Wi-Fi AP làm board reset. Firmware nay tự điều khiển
WS2812 bằng một kênh RMT giữ cố định và hạ nhịp nhấp nháy xuống 25 Hz.

Dòng `[hb]` còn in `stack led=... batt=...` (byte trống của mỗi tác vụ); nếu giá trị
tụt gần 0 trước khi board reset thì đó là tràn stack.

| Hiện tượng | Cách xử lý |
|---|---|
| Không thấy Wi-Fi | kiểm tra nguồn USB; xem log serial 115200 |
| Web mở được nhưng luôn “chưa có pin” | sai chân DATA/ENABLE, thiếu trở kéo 4.7 kΩ, hoặc chưa chung GND |
| Log báo `Waiting...` liên tục | BMS ngủ — thử mắc tải 1 kΩ ngang cực nguồn pin |
| Module không boot khi cắm pin | đường DATA (GPIO2) bị kéo xuống lúc khởi động |
| Đọc lúc được lúc không | rút ngắn dây, siết chặt tiếp xúc, kiểm tra 120 Ω / 4.7 kΩ |

⚠️ Thao tác ghi (mở khóa / Omega) thay đổi EEPROM của BMS. Sai sót có thể làm hỏng pin vĩnh viễn.
Tự chịu trách nhiệm khi sử dụng.
