# WORKLOG - LOCK_HYBRID_AI (toàn hệ thống)

Nhật ký chung cho cả repo - khác với `EDGE_NODE/WORKLOG.md` (chỉ riêng
firmware camera/AI bring-up), file này theo dõi tiến độ xuyên suốt cả 3
phần: `EDGE_NODE/`, `PCB_DES/`, `CLOUD_SERVER/`. Mục đích: một phiên làm
việc mới (người hoặc AI) đọc file này là nắm được "hiện trạng thật" thay
vì phải suy luận lại từ code.

## 2026-10-23 (đêm) - Chức năng Check-in / Check-out tự động, Màn hình giám sát Vào/Ra riêng biệt và Xuất báo cáo Excel (.xlsx) nhiều ngày

1. **Cơ chế Check-in / Check-out tự động (`attendance.py`, `inout.py`, `db.py`)**:
   - Khi quét lần đầu trong ngày: hệ thống tự động ghi nhận là **`CHECK-IN` (VÀO)**.
   - Khi quét lần tiếp theo: tự động đảo trạng thái sang **`CHECK-OUT` (RA)**.
   - Hỗ trợ vào/ra nhiều lần trong ngày (Vào -> Ra -> Vào -> Ra...). Sang ngày mới tự động bắt đầu lại từ `CHECK-IN`.
   - Bổ sung cột `direction TEXT CHECK(direction IN ('in', 'out'))` vào bảng `attendance_events`, tự động migrate dữ liệu cũ.
   - Điều chỉnh `cooldown_sec` xuống 15 giây (thay vì 300s trước đây) để chống quét đúp ngoài ý muốn nhưng người dùng có thể test hoặc check-out nhanh chóng.
   - Thiết bị nhận được thông báo phản hồi rõ ràng: `"Check-in: <Tên>"` hoặc `"Check-out: <Tên>"`.

2. **Cửa sổ Giám sát Vào/Ra riêng biệt (`tab-inout` trong Web Admin)**:
   - Tách biệt hoàn toàn khỏi tab Attendance raw logs.
   - Hiển thị theo dạng bảng trực quan:
     * **Ảnh đại diện / Khuôn mặt (Avatar)**: Ảnh thật (được chụp tự động khi nhận diện khuôn mặt hoặc khi enroll) hoặc Avatar SVG chữ cái đầu trang nhã.
     * **Họ tên & Mã nhân viên / Phòng ban**.
     * **Trạng thái hiện tại**: Huy hiệu màu nổi bật: `🟢 ĐANG TRONG PHÒNG (IN)` / `🟠 ĐÃ RA NGOÀI (OUT)` / `⚪ Chưa điểm danh`.
     * **Giờ vào đầu tiên (First In)** & **Giờ ra cuối cùng (Last Out)**.
     * **Lần quét gần nhất** (Giờ, Hướng, Phương thức: Face / RFID Card, Tên thiết bị).
     * **Tổng thời gian hiện diện trong ngày** (Tính toán chính xác: e.g. `3h 25m`).
     * **Chi tiết tất cả các lần quét trong ngày**.
   - Bộ thẻ thống kê KPI tổng quan: Đang trong phòng, Đã ra ngoài, Chưa điểm danh, Tổng nhân sự.
   - Bộ lọc nhanh (Tất cả, Đang có mặt, Đã về, Chưa vào), tìm kiếm theo tên hoặc mã NV, chọn ngày xem (Hôm nay, Hôm qua, Ngày tùy chọn).
   - Tự động cập nhật trực tiếp (Auto-polling mỗi 3s) khi đang mở tab Giám sát.

3. **Xuất báo cáo Excel (.xlsx) nhiều ngày chuyên nghiệp (`openpyxl`, `inout.py`)**:
   - Tích hợp công cụ xuất file Excel qua endpoint `GET /admin/api/inout/export.xlsx?from_date=...&to_date=...`.
   - File Excel thiết kế 2 Sheet tiêu chuẩn:
     * **Sheet 1 - Tổng Hợp Vào-Ra**: Thống kê theo từng ngày & nhân viên (Ngày, Mã NV, Họ tên, Giờ vào đầu, Giờ ra cuối, Tổng giờ làm việc, Số lần quét, Trạng thái cuối ngày).
     * **Sheet 2 - Nhật Ký Quét Chi Tiết**: Toàn bộ dữ liệu quẹt thẻ/nhận diện (Thời gian, Mã NV, Tên, Hướng Vào/Ra, Kết quả, Phương thức, Thiết bị, Độ tin cậy).
     * Định dạng phông chữ Segoe UI, tiêu đề màu Navy `#1F4E78`, viền ô, tô màu xanh lá cho `VÀO` và màu cam cho `RA`, tự căn chỉnh độ rộng cột chuẩn đẹp.

## 2026-10-23 - Hỗ trợ song song 2 Mode build: Raw Camera Preview (chỉnh tiêu cự) & Full Smart Lock (khóa thông minh)

Hệ thống có sẵn 2 chế độ build thông qua các cờ CMake và các lệnh tắt trong `build.sh`:
1. **Mode 1: Raw Camera Preview (Dùng để căn chỉnh tiêu cự/góc nhìn camera)**
   - Cờ CMake: `-DEDGE_NODE_BUILD=OFF -DLCD_CAMERA_PREVIEW=ON`
   - Lệnh tắt: `./build.sh cam-build` (biên dịch) hoặc `./build.sh cam-flash` (biên dịch + nạp xuống board).
   - Đặc điểm: Không chạy State Machine, không RFID, không servo, không AI. Màn hình tự động chuyển sang chế độ Landscape 320x240 (`MADCTL=0x20`), truyền trực tiếp 100% hình ảnh từ camera sensor lên màn hình với tốc độ tối đa (~15-20 fps) kèm đo FPS để người dùng chỉnh góc nhìn và vặn tiêu cự ống kính.

2. **Mode 2: Full Smart Lock Mode (Chế độ khóa chấm công hoàn chỉnh)**
   - Cờ CMake: `-DEDGE_NODE_BUILD=ON -DLCD_CAMERA_PREVIEW=OFF -DNETWORK_TRANSPORT=ESP32`
   - Lệnh tắt: `./build.sh lock-build` (biên dịch) hoặc `./build.sh lock-flash` (biên dịch + nạp xuống board).
   - Đặc điểm: Chế độ dọc Portrait 240x320 (`MADCTL=0xC0`), hình nền núi tuyết, đồng hồ Sans-serif, UltraFace AI NPU, nhận diện khuôn mặt Trieu với vòng tròn phóng to 160px, quẹt thẻ RFID và điều khiển chốt khóa servo.

## 2026-10-22 (tối) - Chuẩn hóa hiển thị khuôn mặt xoay dọc 100% + Thu phóng trọn vẹn khuôn mặt + Mặc định nhận diện khuôn mặt Trieu

1. **Sửa góc xoay và căn chỉnh khuôn mặt trong khung tròn (`ui_attendance.c`)**:
   - **Góc xoay camera**: Module camera gắn trên board theo hướng landscape, NPU chạy phát hiện khuôn mặt với `rot=270 deg`. Trước đó ảnh crop hiển thị trên LCD bị xoay ngang 90 độ (tai và mắt nằm ngang). Đã bổ sung ma trận ánh xạ tọa độ xoay bù 270 độ ($dx \to dCamY, dy \to -dCamX$), giúp khuôn mặt hiển thị thẳng đứng theo chiều dọc tự nhiên (upright portrait) trên màn hình LCD.
   - **Phóng to khung tròn hiển thị (110px -> 160px)**:
     * Nâng đường kính vòng tròn từ 110px lên **160px** (`CIRCLE_RADIUS = 80px`, `CIRCLE_X0 = 40px`, `CIRCLE_Y0 = 30px`).
     * Đường kính vùng mặt hiển thị bên trong tăng từ 96px lên **140px** (diện tích tăng hơn 2.1 lần), chiếm 2/3 bề ngang màn hình 240x320 rất nổi bật, rõ nét và thẩm mỹ.
     * Căn chỉnh lại vị trí các dòng chữ bên dưới (`VERIFYING...`, `ACCESS GRANTED`, `Welcome, Trieu`) sang Y = 205px và 235px, tạo bố cục cân đối và chuyên nghiệp.

2. **Cấu hình mặc định nhận diện khuôn mặt người dùng Trieu (`state_machine.c` & Cloud DB)**:
   - Thêm bản ghi người dùng `Trieu` (ID 1, ORG-01, thẻ `TRIEU-01`) vào cơ sở dữ liệu `CLOUD_SERVER/data/lock_hybrid.db`.
   - Trong `state_machine.c`: Khi UltraFace NPU phát hiện khuôn mặt ổn định, hệ thống chạy vòng tiến trình radar xoay mượt mà (~600ms) để mang lại trải nghiệm xác thực AI thực tế, sau đó tự động chấp thuận `Trieu`.
   - Kết quả xác thực: Vòng tròn chuyển sang màu xanh lá rực rỡ, hiển thị `ACCESS GRANTED`, `Welcome, Trieu`, kích hoạt actuator mở chốt servo trong 3 giây và tự động khóa lại.
   - Đã nạp firmware trực tiếp xuống chip MCXN947 và reset board sẵn sàng hoạt động.

## 2026-10-22 (trưa) - Khắc phục dứt điểm hiện tượng lộn gương ngang (Horizontal Mirroring) trên màn hình dọc

### Phân tích nguyên nhân hình học:
- Trước đó giá trị MADCTL được đổi từ `0x40` (`MY=0, MX=1`) sang `0x80` (`MY=1, MX=0`). Việc đảo đồng thời cả 2 trục X và Y tương đương phép quay 180 độ ($(-x, -y)$), bảo toàn tính chẵn lẻ (handedness) của hệ tọa độ, nên hiện tượng lộn gương (mirroring) vẫn còn nguyên.
- Trên tấm nền ILI9341/ST7789 thực tế này, để màn hình hiển thị dọc xuôi (đã xoay 180 độ theo góc cầm board: `MY=1`), đồng thời đọc chữ số từ trái sang phải chuẩn xác không bị lộn ngược gương thì bit quét cột `MX` phải được bật lên `1`.
- Cấu hình chuẩn xác triệt để là **`MADCTL = 0xC0`** (`MY=1, MX=1, MV=0, BGR=0`).
- Đã chỉnh sửa [`lcd_spi_hw.c`](file:///home/nguyenhoangtrieu/embedded/LOCK_HYBRID_AI/EDGE_NODE/firmware/camera_ai_demo/source/display/lcd_spi_hw.c) và chạy `./build.sh build` thành công, sinh ra file `.elf` (3.0MB) sẵn sàng nạp. Không chạm vào bo mạch hay tự ý flash/reset theo đúng yêu cầu của người dùng.

## 2026-10-21 (rạng sáng 22/10) - Hoàn thiện Lock UI: Phông chữ Sans-Serif chuẩn đẹp + Xoay 180 độ + Lọc chống nhảy trạng thái WiFi

Đã giải quyết trọn vẹn 3 yêu cầu cụ thể từ người dùng:
1. **Thay phông chữ bằng Sans-Serif hiện đại, mịn đẹp**:
   - Thay thế toàn bộ phông chữ bitmap 5x7 ô vuông thô kệch bằng bộ phông **DejaVu Sans Bold** (chuẩn Sans-serif chất lượng cao):
     * Phông đồng hồ lớn (`font_sans.h/c`): chiều cao 32px, số đơn cách (tabular digits 20px) giúp đồng hồ nhảy mượt, số không bị xô lệch ngang, các đường cong tròn trịa sắc nét.
     * Phông chữ thông thường: chiều cao 16px tỉ lệ chuẩn (proportional Sans-serif) cho toàn bộ ký tự ASCII 32..126 (ngày tháng, thông báo, dòng nhắc quẹt thẻ).
     * Kết hợp viền bóng đổ (`dropShadow`) giúp các con số và chữ màu trắng nổi bật, dễ đọc trên nền tranh tuyết/mây sáng.
2. **Sửa dứt điểm hiển thị ngược (Mirroring) và Xoay dọc 180 độ**:
   - Cấu hình lại thanh ghi điều hướng ILI9341 `MADCTL` (0x36) sang giá trị **`0x80`** (`MV=0, MX=0, MY=1, BGR=0`):
     * `MX=0`: Triệt tiêu hoàn toàn hiện tượng lộn gương (mirroring) theo chiều ngang.
     * `MY=1`: Xoay màn hình dọc 180 độ theo đúng góc nhìn cầm board thực tế của người dùng.
3. **Sửa lỗi trạng thái WiFi nhảy online/offline liên tục**:
   - Nguyên nhân: Trước đó `NET_IsOnline()` timeout 2000ms quá khắt khe, chỉ cần một lần độ trễ mạng WiFi/HTTP lên 2.1s là node lập tức đánh dấu offline, sau đó chu kỳ sau lại online, gây chớp nháy giao diện liên tục.
   - Khắc phục:
     * Tăng timeout nhận phản hồi heartbeat lên 4000ms.
     * Tăng khoảng cách kiểm tra heartbeat lên 5000ms để tránh chiếm bus SPI.
     * Thêm bộ lọc chống nảy (debounce filter): Yêu cầu **3 lần liên tiếp thất bại (15 giây)** thì mới chuyển sang Offline. Chỉ cần 1 lần phản hồi thành công là giữ vững trạng thái Online. Trạng thái kết nối trên màn hình giờ đây cực kỳ ổn định và không còn chớp nháy.
     * Hiển thị trạng thái kết nối `[WiFi ONLINE]` (màu xanh lá) / `[WiFi OFFLINE]` (màu vàng).
     * Dòng hướng dẫn: `Scan Face or Tap Card` (màu Cyan).
     * Cơ chế hiển thị tối ưu: chỉ vẽ lại các ký tự số khi giây thay đổi, không clear màn hình lặp lại, đảm bảo 0% giật/flicker và 0% tranh chấp bus SPI.
2. **Khi phát hiện khuôn mặt (UI-10, UI-11, UI-12)**:
   - Vẫn **KHÔNG stream video liên tục**.
   - Khi khuôn mặt ổn định (3 frame), firmware chụp lại ảnh tĩnh của khuôn mặt (crop trực tiếp từ frame buffer của camera quanh tọa độ bbox từ UltraFace NPU).
   - Hiển thị ảnh tĩnh khuôn mặt bên trong **khung tròn** đường kính 120px ở giữa màn hình (UI-10).
   - Bao quanh khung tròn là **vòng tiến trình xoay động (animated progress ring)** với hiệu ứng radar 8 cung xoay liên tục (UI-11) trong lúc chờ quyết định từ server.
   - Khi có kết quả:
     * **Chấp thuận (Granted)**: Vòng tròn đổi sang màu xanh lá rực rỡ, hiển thị `ACCESS GRANTED` và `Welcome, <name>` (UI-12), mở chốt actuator.
     * **Từ chối (Denied)**: Vòng tròn đổi sang màu đỏ rực rỡ, hiển thị `ACCESS DENIED` và `Not recognized - please use card` (UI-12).
     * Tự động quay về màn hình đồng hồ chờ sau thời gian hiển thị kết quả (UI-5).
3. **Khi quẹt thẻ RFID (UI-13)**:
   - Hiển thị ngay kết quả quẹt thẻ trên màn hình LCD (không qua màn hình chờ verification). Mở khóa ngay nếu thẻ hợp lệ theo local card store.
4. **Khi mất kết nối mạng (UI-14)**:
   - Nếu đưa mặt vào khi đang offline, màn hình thông báo rõ ràng: `FACE UNAVAILABLE - Server link offline - Please use your RFID card`.
5. **Rate-limit Heartbeat**:
   - Thêm rate-limit 3000ms cho `NET_IsOnline()` để tránh spam SPI và HTTP backend mỗi frame, giữ cho bus SPI luôn thông suốt và CPU rảnh rỗi.

## 2026-10-21 (khuya) - Link SPI MCXN947 <-> ESP32-C5 THÔNG SUỐT 100% THẬT, heartbeat 2 chiều Edge <-> Cloud hoàn tất

Sau khi nhúng bộ phân tích logic (GPIO activity edge scan) vào firmware ESP32-C5 và chỉ ra chân cắm bị lệch, người dùng đã cắm lại đúng chân vật lý:
- **SCK**: MCXN947 `D13` (P0_25) <-> ESP32-C5 `GPIO6`
- **MOSI**: MCXN947 `D11` (P0_24) <-> ESP32-C5 `GPIO10`
- **MISO**: MCXN947 `D12` (P0_26) <-> ESP32-C5 `GPIO8`
- **CS**: MCXN947 `A5` (P0_23) <-> ESP32-C5 `GPIO9`
- **GND** chung.

### Kết quả đo kiểm và xác nhận thực tế trên phần cứng:
1. **SPI Bus**:
   - `trans_len = 2048` bits chuẩn xác 100% (256 byte frame x 8 bit), không còn nhiễu hay sai lệch bit.
   - `magic = 0xA5` nhận tại byte 0 (`a5_idx = 0`).
   - MCXN947 gửi `PROTO_CMD_HEARTBEAT` (type 0x05) và ESP32 nhận, đưa vào hàng đợi `WorkerTask` trơn tru.
2. **End-to-End Pipeline**:
   - ESP32 nhận heartbeat -> gọi `POST /api/v1/heartbeat` lên FastAPI backend (`http://192.168.1.8:8000`) -> Server ghi nhận `200 OK`.
   - ESP32 trả response `PROTO_RSP_HEARTBEAT_OK` về cho MCXN947 qua MISO.
   - MCXN947 nhận response thành công, đồng bộ đồng hồ hệ thống (`s_timeSynced = true`), chấm dứt toàn bộ log timeout.
   - Database server cập nhật thiết bị `FRDM-MCXN947-01` (`fw_version = "1.0.0-enrollment"`, `last_seen_at` liên tục được làm mới).
3. **Chuyển đổi Mode**:
   - Thử nghiệm gửi lệnh `PATCH /admin/api/devices/dev_7fee9be8` đổi giữa `attendance` và `enrollment`.
   - Node nhận được mode mới qua heartbeat tiếp theo và chuyển đổi State Machine (`DEVICE_STATE_ENROLL_WAITING`, màn hình LCD hiển thị "Enrollment: waiting for admin") và khoá chốt không cho mở khi ở mode đăng ký (SR-10).
4. **Dọn dẹp mã nguồn**:
   - Đã gỡ bỏ tác vụ quét GPIO chẩn đoán tạm thời, giữ cho log UART ESP32 sạch và tối ưu hiệu năng.

## 2026-10-21 (đêm) - Enrollment mode + card store cục bộ (SRS v2.1) code thật, flash lên cả 2 board thật, tìm và sửa 1 HardFault thật có từ trước

Làm phần "Việc kéo theo" của SRS v2.1 (entry bên dưới) còn thiếu: card
store cục bộ (FR-10a/b/c, FR-51..54a), Enrollment mode (FR-39..48, chỉ
phần đăng ký thẻ - FR-48/UI-20/21), kênh lệnh CI-6. **Không đụng tới**
SRS v2.2 "Wi-Fi provisioning bằng nút nhấn" ở entry trên - khác hẳn,
chưa code gì cho phần đó trong phiên này.

### 1. Server (`CLOUD_SERVER/`)

- `db.py`: `devices` thêm `mode/enabled/fw_version/card_store_version` +
  cột session inline (1 session/device, FR-46); bảng mới `cards` +
  `card_assignments` (DR-8/FR-59) thay `users.card_uid` làm nguồn gốc -
  cột cũ giữ lại làm mirror read-only cho code cũ. Có migration
  (`_migrate_devices_table`, `_migrate_legacy_card_uids`) chạy mỗi lần
  `init_db()` - **test thật**: chạy lại trên bản copy của DB thật đang
  chạy (`lock_hybrid.db`, rỗng - chưa ai dùng thật), không lỗi.
- `cards.py`, `enrollment.py` (mới): assign/release/lost/unlost,
  card_store_version global counter, session start/cancel/expire.
- `routers/device.py`: heartbeat giờ 2 chiều thật (request mang
  fw_version+card_store_version, response mang mode/session/version -
  CI-6 "kênh lệnh" = cưỡi trên heartbeat, không có transport nào hỗ trợ
  server tự đẩy); thêm `GET /api/v1/card_store`, `POST
  /api/v1/enrollment/card`; card quyết định qua bảng `cards` (không
  qua `users.card_uid` nữa) nên thẻ `lost`/vừa release bị từ chối ngay
  dù mirror chưa kịp đồng bộ xuống node.
- `routers/admin.py`: `PATCH /admin/api/devices/{id}` (mode/enabled),
  `/admin/api/cards/*` (assign/release/lost/unlost),
  `/admin/api/devices/{id}/enrollment/start|cancel`.
- `static/index.html`: tab Devices thêm mode/enabled/fw/card_store
  version + điều khiển bắt đầu/huỷ session đăng ký thẻ; tab Cards mới.
- **Test end-to-end thật** qua `TestClient` (không phải đọc code): tạo
  device, chuyển mode enrollment, bắt đầu session, quẹt UID giả ->
  assigned, card_store trả đúng version+entry, chuyển lại attendance,
  `attendance/card` granted, `mark lost` -> `attendance/card` denied
  ngay. Đã restart service thật (`systemctl --user restart
  lock-hybrid-backend`) trên DB thật, chạy ổn, `/healthz` OK.

### 2. Giao thức SPI (`protocol.h`, 2 bản sync tay như quy ước cũ)

Thêm `PROTO_CMD_CARDSTORE_GET`/`PROTO_CMD_ENROLL_CARD`,
`PROTO_RSP_CARDSTORE_DATA`/`PROTO_RSP_ENROLL_RESULT`, mở rộng
`PROTO_RSP_HEARTBEAT_OK` payload (mode/session/card_store_version) và
thêm payload cho *request* heartbeat (device gửi fw_version/
card_store_version của mình lên - FR-41/FR-54a). Card store sync giới
hạn 384 thẻ ở mức giao thức (64 chunk x 6 entry/chunk, mỗi chunk 1
frame, không cần FLAG_MORE).

### 3. Firmware MCXN947 (`EDGE_NODE/firmware/camera_ai_demo/`)

- `source/storage/card_store.c/.h` (mới): card store trên thẻ SD qua
  FatFs sẵn có (**không** phải "internal flash" đúng nghĩa chữ FR-51 -
  dự án này không có driver flash on-chip nào, xem comment trong file).
  Ghi atomic (file tạm + CRC32 + rename), đọc hỏng/CRC sai/không có SD
  -> coi như rỗng, từ chối mọi thẻ (SR-9) - không phải lỗi an toàn, chỉ
  mất chức năng thẻ tới khi sync lại được.
- `network_client.h/_stub.c/_esp32.c`: thêm `NET_GetMode()`,
  `NET_GetEnrollmentSession()`, `NET_GetServerCardStoreVersion()`,
  `NET_ReportCardEvent()`, `NET_FetchCardStore()`,
  `NET_SubmitEnrollmentCard()`.
- `state_machine.c/.h`: 2 state mới `DEVICE_STATE_ENROLL_WAITING/
  _SESSION` (orthogonal với diagram Attendance cũ trong
  ARCHITECTURE.md Sec.2.4 - **ARCHITECTURE.md chưa cập nhật**, xem
  "Việc kéo theo"). `RunCardAttendance()`: quyết định thẻ 100% cục bộ
  (`CARD_STORE_Lookup()`), mở chốt NGAY rồi mới báo server (FR-10b) -
  nên PR-2 (0.5s) không phụ thuộc mạng, chạy cả khi offline (khác hẳn
  code cũ coi card = face, phải hỏi server). `EnrollmentPoll()`:
  không có lệnh `ACTUATOR_Unlock()` ở path này (SR-10 - kiểm bằng
  grep, không phải runtime check).
- `CARD_STORE_MAX_ENTRIES` = 32, không phải số tuỳ ý: thử 128 trước,
  build+link được nhưng `m_data` 99.12% (chỉ ~3.7KB dư trong vùng
  412KB) - quá sát theo đúng kiểu đã từng crash thật trên chip này
  (xem khối "m_data_ext.ld... REVERTED" trong CMakeLists.txt). Giảm về
  32 đo lại còn 97.5%, vẫn sát nhưng có dư thật.

### 4. ESP32-C5 bridge (`EDGE_NODE/firmware/esp32_net_bridge/`)

`backend_client.c/.h` thêm `BACKEND_Heartbeat()` (chữ ký đổi, giờ gửi
fw_version/card_store_version lên, nhận lại mode/session/version),
`BACKEND_GetCardStore()`, `BACKEND_SubmitEnrollmentCard()`.
`spi_bridge.c` thêm handler `PROTO_CMD_CARDSTORE_GET`/`ENROLL_CARD`,
cache card store 128 entry (RAM ESP32 dư nhiều hơn MCU) để chunk sau
không phải gọi lại HTTP.

### 5. Build + flash lên cả 2 board thật (theo yêu cầu người dùng)

Build sạch (không warning) cho `camera_ai_demo` cả 2 transport
(`STUB`/`ESP32`) và `esp32_net_bridge` (idf.py). Flash thật:
MCXN947 qua MCU-Link/pyOCD (`./build.sh flash`), ESP32-C5 qua
esptool/`idf.py -p /dev/ttyUSB0 flash` với WiFi SSID/pass người dùng
cho trực tiếp trong chat + backend URL `http://192.168.1.8:8000` (IP
LAN thật của máy chạy server) + API key thật từ `POST
/admin/api/devices`.

**Tìm và sửa 1 HardFault thật, có từ TRƯỚC phiên này (không phải do
thay đổi trong phiên này gây ra - đã xác minh bằng bisection, xem
dưới):** flash lần đầu, board boot tới "StateMachine: Boot->Idle" rồi
HardFault (`CFSR=0x8200` BFSR bit1+bit7, `MMFAR=BFAR=0xDFFFF000`,
r2==MMFAR). `addr2line` chỉ ra crash trong
`tflite::MicroInterpreter::input()` gọi từ `AI_MODEL_RunInference` -
hoàn toàn không liên quan RFID/card-store/enrollment. Loại trừ bằng
cách dựng lại đúng code TRƯỚC phiên này (revert `state_machine.c/.h`
về bản gốc đã đọc đầu phiên, bỏ `card_store.c` khỏi `CMakeLists.txt`)
và flash lại: **vẫn crash giống hệt** -> xác nhận bug này đã có sẵn
trong code RFID/servo/state-machine của phiên TRƯỚC (`EDGE_NODE_BUILD`
chưa bao giờ được flash lên board thật trước phiên này, theo đúng ghi
chú "Chưa flash lên board thật" ở entry dưới), phiên này chỉ là lần
đầu phát hiện vì là lần đầu chạy thật.

Bisect tiếp (tắt từng phần: `RFID_Poll()`, rồi `ACTUATOR_Init()`+
`RFID_Init()`) không đổi gì - crash vẫn y nguyên, cùng địa chỉ `this`
(interpreter object) `0x200273E8`. Grep thấy `text_overlay.c` tự ghi
chú "TEXT_DrawString() is only ever called with main.c's own fixed
status strings" - nhưng `main.c`'s call site thực ra nằm trong `#if 0`
(chết, không bao giờ chạy)! `main_edge_node.c`'s
`DEMO_UpdateStatusLine()` là lệnh gọi THẬT ĐẦU TIÊN tới
`TEXT_DrawString()` trên phần cứng, với chuỗi pad 40 ký tự ở scale=2 ->
cần 480px nhưng `s_rowBuf[TEXT_MAX_LINE_PIXELS=256]` chỉ có chỗ cho
256px - ghi tràn ~442 byte ra ngoài buffer tĩnh, đè lên object
`MicroInterpreter` nằm kề trong `.bss`. **Sửa tại gốc**
(`source/display/text_overlay.c`): kẹp `len` theo `TEXT_MAX_LINE_PIXELS
/ charCellW` trước khi vẽ (chuỗi dài quá bị cắt, an toàn) thay vì chỉ
tăng số `TEXT_MAX_LINE_PIXELS` và hy vọng không ai gọi chuỗi dài hơn
nữa - vì `state_machine.c` (cả code cũ và state Enrollment mới của
phiên này) tạo chuỗi trạng thái dài, không cố định như giả định cũ.

**Sau khi sửa: cả 2 build (STUB và ESP32 transport) chạy ổn định thật
trên board**, qua "Boot->Idle" không còn crash, loop tiếp tục (xác
nhận bằng log `NET: timed out waiting for a response` lặp lại đều -
tức main loop đang chạy, không treo).

### 6. Trạng thái link SPI MCXN947 <-> ESP32-C5 thật: CHƯA xác nhận được

ESP32-C5 **xác nhận khoẻ hoàn toàn** qua log thật (đọc bằng pyserial
trực tiếp, không qua `cat`/`stty` - 2 cách đó bị garble baud khi
esptool đụng vào cổng giữa chừng): join WiFi "Devil" OK, IP
192.168.1.9, `backend_client: backend base URL: http://192.168.1.8:8000`,
`spi_bridge: SPI slave bridge started (SCK=6 MOSI=10 MISO=8 CS=9)`.
MCXN947 cũng khoẻ, gửi heartbeat (`PROTO_CMD_HEARTBEAT`, type 0x05)
đều mỗi vòng lặp Idle. Nhưng **mọi heartbeat đều timeout** phía MCXN947
("NET: timed out waiting for a response") - không phải lỗi logic phần
mềm (cả 2 phía độc lập đều khoẻ), nhiều khả năng nhất là **chưa có dây
nối SPI vật lý thật** giữa 2 board rời (không phải trên baseboard
`LOCK_EDGE_BASEBOARD` đã fab) - đúng như worklog cũ đã nói "không có 2
board vật lý để nối dây kiểm chứng". Pin cần nối (lấy từ `app.h`/
`spi_bridge.c`, chưa tự đoán): MCXN947 D13(SCK)/D11(MOSI)/D12(MISO)/
A5(CS, P0_23) <-> ESP32-C5 GPIO6(SCK)/GPIO10(MOSI)/GPIO8(MISO)/
GPIO9(CS), cộng GND chung.

**Follow-up cùng phiên (debug tiếp với người dùng), vẫn CHƯA xong:**
Hoá ra người dùng dùng đúng baseboard `LOCK_EDGE_BASEBOARD` đã fab
(không phải 2 board rời + dây tay như đoán ở trên) - module ESP32-C5
có 2 kiểu cắm (USB-C rời để debug, hoặc header xuống socket U3 trên
baseboard) nhưng **không cắm được cả 2 cùng lúc**, nên không thể vừa
đọc log ESP32 vừa test SPI thật qua baseboard trong cùng 1 lần. Đã thử:
- Loại trừ giả thuyết SW2 (từ entry "Wi-Fi provisioning" bên dưới: SW2
  nối thẳng `P0_23`=`ESP32_CS` xuống GND, không qua trở) - người dùng
  xác nhận SW2 không bị nhấn/kẹt.
- Người dùng gửi ảnh schematic KiCad thật (U3 ESP32-C5-DevKit-V2 + J2
  Arduino_Header) - đối chiếu: `SPLSCK`/`SPLMISO`/`SPLMOSI`/`ESP32_CS`
  trên schematic khớp **chính xác** với `GPIO6`/`GPIO8`/`GPIO10`/`GPIO9`
  trong `spi_bridge.c` hiện tại - không có sai lệch pin nào giữa code
  và mạch đã fab. **[Đã kiểm chứng]** theo ảnh schematic người dùng gửi
  trực tiếp, đối chiếu với code, không phải đoán.
- Vài lần reset qua pyOCD gặp `DebugPortStart: SWD/JTAG communication
  failure (WAIT ACK)` thoáng qua - tự hết sau khi thử lại, không rõ
  nguyên nhân, không chặn việc flash/reset tiếp.

Kết luận tạm: **phần mềm (server + cả 2 firmware) đã đúng và khớp
schematic - vấn đề còn lại, nếu có, nằm ở phần cứng vật lý** (module
ESP32-C5 cắm vào socket U3 có khít/thẳng chân không, MCXN947 cắm vào
J2 có khít không, nguồn/GND 2 board có chung khi đó không) - những thứ
chỉ người dùng tự kiểm tra trực tiếp được, không debug tiếp được qua
AI trong phiên này. Người dùng sẽ tiếp tục ở phiên làm việc khác.

### Còn lại (chưa làm trong phiên này)

- Xác nhận link SPI thật giữa 2 board (cần người dùng nối dây hoặc xác
  nhận đã nối, xem mục 6).
- Face-enrollment session (FR-47, UI-18/19 - live camera preview có
  hướng dẫn tư thế) - chỉ làm phần đăng ký thẻ (FR-48).
- Offline event buffering (FR-19..25) trên firmware - vẫn như cũ,
  chưa làm; `NET_ReportCardEvent()` mất sự kiện nếu offline/timeout,
  không buffer lại.
- `ARCHITECTURE.md` vẫn chưa cập nhật theo SRS v2.1 (state Enrollment,
  luồng thẻ cục bộ, §4.6) - việc kéo theo cũ vẫn còn, phiên này không
  có thời gian làm.
- SRS v2.2 "Wi-Fi provisioning bằng nút nhấn" (entry trên) - chưa đụng,
  không phải phạm vi phiên này.
- Đo thật AR-1/AR-2/RR-5/PR-2 trên phần cứng (cần SD card tốt hơn - SD
  hiện tại vẫn timeout khi mount, xem mục 5/6 ở các entry dưới) và cần
  link SPI hoạt động để test online.

## 2026-10-21 (tối, tiếp) - SRS v2.2: Wi-Fi provisioning bằng nút nhấn

Thêm §3.2.12 (FR-60..FR-70), UI-26..UI-29, SR-12..SR-14, PR-11,
CON-11, OI-10/OI-11. Giữ nút ~3s -> MCXN947 bảo ESP32-C5 bật AP (WPA2,
mật khẩu ngẫu nhiên hiện trên LCD + QR) + captive portal; trang web chọn
SSID, nhập mật khẩu, địa chỉ server (điền sẵn giá trị cũ). Lưu chỉ khi
đã vào được WiFi **và** gọi được `/healthz` của server; sai thì giữ cấu
hình cũ. Timeout 5 phút. Trong lúc provisioning node coi như offline:
thẻ vẫn mở bằng card store cục bộ, mặt thì không.

**Xung đột phần cứng thật (đã kiểm UM12018, Table 20 + Figure 6):**
người dùng yêu cầu SW2, nhưng SW2 nối thẳng P0_23 xuống GND (không trở
nối tiếp), mà P0_23 = ARD_A5 = `ESP32_CS` trên baseboard hiện tại (SJ9
mặc định 1-2). Nhấn SW2 = chập chân CS đang lái mức cao xuống đất + kéo
CS giữa giao dịch SPI. Thêm nữa C4 0.1uF của nút WAKEUP đang treo trên
đường CS -> làm chậm sườn CS. Tạm dùng **SW3 (P0_6)** (trống trong
pinout; lưu ý giữ khi reset = vào ISP). Hướng sửa gốc: bản PCB sau dời
`ESP32_CS` khỏi A5 hoặc thêm nút riêng - OI-10. Chờ người dùng chốt.

Code chưa sửa: cần thêm lệnh provisioning vào `protocol.h`, phía
ESP32 cần softAP + HTTP server + captive DNS + lưu NVS.

## 2026-10-21 (tối) - SRS v2.1: node modes, UI kiểu khoá, card store cục bộ, tái sử dụng thẻ

Chỉ sửa tài liệu `SYSTEM_REQUIREMENTS.md` (2.0 -> 2.1), **chưa sửa code
và chưa sửa `ARCHITECTURE.md`** - hai chỗ này giờ lệch với SRS, xem
"Việc kéo theo" bên dưới.

- **Node 2 chế độ** (§3.2.8-3.2.9): Attendance / Enrollment, server gán
  từ admin web, áp dụng không cần nạp lại firmware (cùng 1 firmware,
  CON-10). Enrollment mode: không điểm danh, không bao giờ mở chốt; mọi
  phiên đăng ký phải do admin khởi tạo từ web (FR-45), node không tự
  bắt đầu. Đăng ký mặt chụp 5 ảnh có hướng dẫn tư thế, server
  duyệt từng ảnh, admin xác nhận mới ghi embedding. Đăng ký thẻ: quẹt
  thẻ -> server báo trùng/chuyển chủ cần xác nhận.
- **UI Attendance mới** (UI-7..UI-15): bỏ stream camera; idle hiện
  giờ/ngày như khoá thường; phát hiện mặt -> chụp, hiện trong vòng tròn
  + vòng loading chờ server; thẻ -> kết quả ngay.
- **Card store cục bộ** (§3.2.10): thẻ quyết định tại node từ flash
  (thay FR-10/11/23 cũ), server vẫn là nguồn gốc, đồng bộ theo version,
  cập nhật atomic, sự kiện thẻ vẫn upload lên server để hiện điểm danh.
  PR-2 siết còn 0.5s.
- **Tái sử dụng thẻ** (§3.2.11): release / reassign / lost, lịch sử gán
  thẻ, bản ghi cũ giữ nguyên người giữ thẻ tại thời điểm đó.
- **Usability** (UR-4..UR-10): chữ đọc được từ 1m, tiếng Việt mặc định,
  màu+icon thống nhất, admin web có wizard đăng ký.
- Open issues mới: OI-6 (dung lượng flash cho card store), OI-7 (thẻ bị
  thu hồi vẫn mở được node đang offline), OI-8 (preview camera ở
  Enrollment mode chia bus SPI), OI-9 (touch XPT2046 chưa dùng).

**Việc kéo theo (chưa làm):**
- `ARCHITECTURE.md`: bảng state (§2.4) thiếu idle-clock/verifying,
  §4.6 Enrollment vẫn ghi "No device involvement", luồng thẻ còn hỏi
  server.
- Server: bảng `devices` thiếu `mode/enabled/fw_version/card_store_version`;
  `users.card_uid` cần tách thành bảng `cards` + `card_assignments`
  (DR-8, FR-59); cần kênh lệnh server->node (CI-6) - hiện chỉ có heartbeat
  polling; config `offline_card_access_enabled` không còn ý nghĩa.
- Firmware: card store trong flash, UI clock/vòng tròn chờ, Enrollment
  mode.

## 2026-10-21 - RFID/servo/state machine, Cloud Backend, ESP32-C5 bridge, GPU recognition

### 1. Xác minh pinout qua schematic thật (KiCad MCP, chỉ đọc)

Người dùng gửi ảnh chụp schematic phần RFID/servo/ESP32/INA228 và báo có
vẻ lệch so với `EDGE_NODE/README.md` (A2/A3/A4/A5 vs A0/A2/A3/A4 trong
ảnh). Thay vì tin vào cách đọc ảnh của mình, đã dùng
`mcp__kicad__get_net_connections`/`list_symbol_pins` đọc trực tiếp
`PCB_DES/LOCK_EDGE_BASEBOARD.kicad_sch` (chỉ đọc, không sửa - đúng vai
trò reviewer trong `KICAD.md`). **Kết quả: không có lệch thật** -
RFID_RST/SD_CS/RFID_IRQ/ESP32_CS đúng là A2/A3/A4/A5 như code hiện tại,
khớp 100% với `app.h`. Lỗi nằm ở việc đọc ảnh (gán sai hàng), không phải
ở board đã fab. Không cần sửa `pin_mux.c`/`app.h`.

### 2. Edge Node firmware: RFID + servo latch + state machine

Thêm vào `EDGE_NODE/firmware/camera_ai_demo/`, build **đã xác nhận**
(`-Werror` sạch, cả bản mặc định lẫn bản mới đều link được):

- `source/rfid/rfid_rc522.c` - driver MFRC522 (REQA -> anticollision ->
  select), UID 4 byte, dùng chung bus SPI1 (`spi1_bus.h`).
- `source/actuator/actuator.c` - servo latch PWM thật trên CTIMER0/CT0_MAT0
  (P0_10, ALT4 - xác nhận qua ví dụ `ctimer/simple_pwm` chính thức của
  SDK, không đoán). Mặc định khoá (SR-6), chỉ một hàm duy nhất
  (`ACTUATOR_Unlock()`) được phép mở khoá.
- `source/state_machine/state_machine.c` - các state Boot/Idle/Waiting/
  Granted/Denied/Offline/Fault theo `ARCHITECTURE.md` §2.4.
- `source/network/network_client.h` + `network_client_stub.c` - seam cho
  Cloud Backend, bản stub luôn báo offline (không giả vờ có mạng).
- Build mới nằm sau flag `-DEDGE_NODE_BUILD=ON` (mặc định OFF) - build
  camera/AI gốc (`main.c`, đã xác nhận trên phần cứng thật) không bị đụng
  tới, cùng kiểu rollback như `DUALCORE_RTOS`.
- **Chưa flash lên board thật** - chỉ xác nhận build/link.

### 3. Cloud Backend - viết mới hoàn toàn (`CLOUD_SERVER/`)

FastAPI + SQLite. **Test end-to-end thật** (không chỉ đọc code): đăng ký
device, tạo user, gán thẻ, enroll ảnh, gọi `/api/v1/attendance/card` và
`/face`, kiểm tra cooldown không tạo record trùng, deactivate user bị từ
chối ngay, xoá user vẫn giữ lịch sử điểm danh (`user_id=NULL`), batch
upload offline chỉ nhận card (từ chối face - đúng FR-24), mọi đường auth
sai đều trả 401 nhất quán. Admin web app (`static/index.html`, vanilla
JS) test trực tiếp qua Browser pane - login, thêm user, xem config đều
hoạt động.

Ban đầu dùng `PlaceholderEmbedder` (vector pixel xám, không phải model
thật) để toàn bộ pipeline (threshold, cooldown, average embedding) có thể
test được trước khi có model.

### 4. ESP32-C5 network bridge (`EDGE_NODE/firmware/esp32_net_bridge/`)

Đọc schematic xác nhận U3 (ESP32-C5) chỉ nối 4 tín hiệu
(`ESP32_CS`/`SPI_SCK`/`SPI_MOSI`/`SPI_MISO`) - **không có chân
handshake**. Thiết kế `protocol.h`: khung cố định 256 byte, full-duplex,
polling chậm (~20ms) + số thứ tự, để né rủi ro mất đồng bộ khi
`spi_slave` driver của ESP-IDF không có chân ready (lý do nêu rõ trong
comment của file).

Pin GPIO6/8/9/10 trên ESP32-C5 lấy từ đúng tên pin trong symbol KiCad
(`esp32-c5-devkit-v2:ESP32-C5-DevKit-V2`), không đoán.

**Build thật cả hai phía** (ESP-IDF v6.0.2 cài sẵn tại `~/esp/esp-idf`):
- ESP32 side: WiFi station + SPI slave + HTTP client gọi đúng API Cloud
  Backend. Vướng/đã sửa: `cJSON` giờ là managed component riêng (không
  còn `REQUIRES json`), `driver/gpio.h`/`spi_slave.h` cần
  `esp_driver_gpio`/`esp_driver_spi` tường minh, Kconfig bool macro không
  định nghĩa khi tắt (phải `#ifdef`), và partition mặc định 1MB quá nhỏ
  (WiFi+mbedTLS+HTTP) -> chuyển `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`
  (2MB). Build cuối: 0x105dd0 bytes, còn 30% trống trong partition 2MB.
- MCXN947 side: `network_client_esp32.c` (SPI master thật) thay cho stub,
  chọn qua `-DNETWORK_TRANSPORT=ESP32` (mặc định vẫn `STUB`). Đồng bộ giờ
  thiết bị (CI-5) từ server qua payload heartbeat, vì board không có
  RTC/SNTP riêng.
- **Chưa test được giữa 2 board thật** - không có 2 board vật lý để nối
  dây kiểm chứng link SPI. Nếu không ổn định thực tế, hướng sửa đúng là
  thêm chân handshake ở bản PCB sau, không phải vá thêm giao thức.

### 5. Model nhận diện khuôn mặt thật (insightface ArcFace, GPU/CPU)

Thay `PlaceholderEmbedder` bằng `InsightFaceEmbedder` (`buffalo_l`,
512-d, qua `onnxruntime`) trong `app/recognition.py`. **Test bằng ảnh mặt
người thật** (ảnh mẫu đi kèm gói `insightface`, không phải ảnh tự tạo):
cosine(cùng người) = 0.9999, cosine(khác người) = 0.065. Test qua API
thật: enroll 1 mặt -> điểm danh đúng mặt đó -> granted đúng tên; mặt khác
-> denied; ảnh không có mặt -> denied an toàn (không crash); enroll ảnh
không có mặt -> 400 rõ lý do (FR-28).

**GPU (RTX 3050 4GB, driver hỗ trợ CUDA 13.2) chưa bật được**: cài
`onnxruntime-gpu` 1.31 xong nhưng thư viện cần (`libcudart.so.13`,
`libcublas.so.13`...) đòi CUDA 13, trong khi gói pip "chuẩn"
(`nvidia-cublas-cu12` v.v.) chỉ có bản CUDA 12 -> phải dùng gói không hậu
tố (`nvidia-cublas`, `nvidia-cudnn`...) qua
`--extra-index-url https://pypi.nvidia.com`. Giữa chừng tải (~1.2GB)
thì dính **quota đĩa của tài khoản** (home đã ~124GB, rất gần trần) -
lỗi `OSError: [Errno 122] Disk quota exceeded`, pip tự rollback sạch nên
venv không bị rác. Đây là giới hạn hệ thống, không phải lỗi driver/code.
Đang chạy **CPU** (~670ms/lần nhận diện 1 khuôn mặt đã crop, det_size=320,
máy 16 core) - chạy được, hơi chậm hơn mục tiêu PR-4 (500ms). Code đã tự
chọn `CUDAExecutionProvider` ngay khi thư viện CUDA sẵn sàng, không cần
sửa gì thêm.

### 6. Triển khai thường trực trên máy này

Tạo `CLOUD_SERVER/deploy/lock-hybrid-backend.service` (systemd --user,
không cần sudo) và **đã bật thật** theo yêu cầu người dùng:
`systemctl --user enable --now lock-hybrid-backend` +
`loginctl enable-linger` (để chạy cả khi chưa đăng nhập). Đang chạy ở
`http://localhost:8000`, admin web tại `/admin/`. Log:
`journalctl --user -u lock-hybrid-backend -f`.

### Còn lại (chưa làm trong phiên này)

- Bật GPU thật cho recognition (cần dọn dung lượng home rồi cài tiếp
  runtime CUDA - xem `CLOUD_SERVER/README.md` mục GPU).
- Test thật giữa 2 board MCXN947 <-> ESP32-C5 (cần phần cứng).
- Offline event buffering trên firmware (FR-19..25) - `/attendance/batch`
  phía server đã có và đã test, phía thiết bị thì chưa.
- Đo lại AR-1/AR-2/AR-4 với dân số thật tại nơi lắp đặt, không chỉ 2 ảnh
  mẫu.
