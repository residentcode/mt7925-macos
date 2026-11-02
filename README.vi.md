# mt7925-macos

**Driver Wi-Fi và Bluetooth cho MediaTek MT7925 (card Wi-Fi 7, PCI `14c3:7925` + USB `0489:e11e`) trên macOS, Hackintosh x86.**
Hai nửa của card là hai thiết bị riêng nên repo có hai nhóm kext. **Wi-Fi:** trên macOS 26 Tahoe và 15 Sequoia, card hiện thành
giao diện Wi-Fi thật (menu Wi-Fi, danh sách mạng trong System Settings, nhập mật khẩu như máy Mac; không nhét mật khẩu vào
kext). **Bluetooth:** controller chạy và thiết bị BLE kết nối được, mới kiểm tra trên macOS 15.

[English](README.md) · **[Hướng dẫn cài](docs/GUIDE.vi.md)** · [Roadmap](docs/ROADMAP.md) · [Releases](https://github.com/thanhnndev/mt7925-macos/releases) · [Bluetooth](docs/BLUETOOTH.md) · [AirDrop / AWDL](docs/ROADMAP-AWDL.md)

> [!WARNING]
> **Dự án thử nghiệm, code do AI viết, mới test trên một máy.** Gần như toàn bộ code và tài liệu do AI (Claude) viết, một người
> định hướng và test trên một chiếc **ASUS ROG Zephyrus G14 2025 (GA403WM)**. Chưa ai audit. Kext chạy trong kernel nên lỗi có thể
> gây panic hoặc treo máy. Hãy backup trước và luôn có sẵn đường mạng khác (USB LAN, chia sẻ mạng qua USB). Không bảo hành,
> xem [LICENSE](LICENSE).

## Tình trạng (2026-11-02)

| | macOS 26 Tahoe (`25G241`, 26.7.1) | macOS 15 Sequoia (`24H32`, 15.8.1) |
|---|---|---|
| **Chế độ native** (menu Wi-Fi, kết nối từ System Settings) | ✅ **chạy được**: tải xuống 176.6 Mb/s, tải lên 65.0 Mb/s (`networkQuality`), HE 160 MHz 2×2, kênh DFS | ✅ chạy được: 28-36 MB/s một luồng TCP, `networkQuality` 293 / 45 Mb/s |
| **Chế độ legacy** (hiện là Ethernet, mật khẩu nhét vào kext) | chưa test trên Tahoe | ✅ ổn định, dùng lâu nhất |
| Lấy ở đâu | nhánh `main`, release **v2.x** | nhánh `sequoia`, release **v1.1** |
| **Bluetooth** (`MtkBtFw` + `BtLeDlFix`) | ❓ chưa kiểm tra trên `25G241`. `BtLeDlFix` vá `bluetoothd` và các byte được tìm trên 24H32 nên nhiều khả năng phải làm lại | ✅ controller chạy; chuột BLE và tai nghe classic kết nối được. Chưa ghi nhận ghép bàn phím |
| AirDrop / AWDL / Continuity | 🔧 đang làm ở nhánh `dev/awdl` (mặc định tắt) | ❌ |

Kext native gắn với đúng bản build macOS mà nó được viết cho: bản Tahoe mới kiểm tra trên `25G241`, bản Sequoia trên `24H32`.
Nếu bạn dùng build khác, hãy kiểm tra trước ([GUIDE §7](docs/GUIDE.vi.md#7-bản-build-macos-khác)).

## Đã làm được

- **Firmware và khởi động chip:** firmware MediaTek được nhúng sẵn trong kext, MAC đọc từ card.
- **Quét mạng:** 2.4 GHz kênh 1-13 và toàn bộ 5 GHz (36-165), gồm cả các kênh DFS 52-64 và 100-144. Bảng AP chứa được 128
  mạng, nên ở chỗ đông mạng thì mạng 2.4 GHz không bị đẩy khỏi danh sách.
- **Bảo mật:** WPA2-PSK (CCMP) và mạng WPA/WPA2 hỗn hợp (pairwise CCMP, group TKIP). Bắt tay 4-way chạy trong driver; ở chế độ
  native thì macOS đưa khoá xuống.
- **Tốc độ:** thang mức 0-6 gồm HT/VHT/HE, 160 MHz, 2 luồng, Block Ack. Nếu một mạng không qua được bắt tay ở mức cao, driver
  tự hạ mức.
- **Tích hợp macOS (native):** `IO80211Controller` và giao diện Skywalk. Có menu Wi-Fi, danh sách trong System Settings, RSSI và
  tốc độ, sự kiện chất lượng link (để macOS không ngắt mạng sau 60 giây), CoreCapture, ioctl NSS.
- **Tự phục hồi:** khi mất link, driver reset chip, nạp lại firmware rồi kết nối lại.
- **ItlWiFi.app:** app trên thanh menu để quét, kết nối và đo tốc độ, dùng cho chế độ legacy.
- **Bluetooth:** `MtkBtFw` nạp firmware Bluetooth MT7925 qua USB (theo quy trình `btmtk` của Linux) để `bluetoothd` thấy một controller thật;
  `BtLeDlFix` (plugin Lilu) giúp thiết bị BLE kết nối xong. Chi tiết và việc còn mở: [docs/BLUETOOTH.md](docs/BLUETOOTH.md).
- **Test tự động trong CI:** WPA2, phân tích HT/VHT/HE, Block Ack, kiểm tra stub sinh ra khớp với header.

## Chưa có

WPA3 / SAE, mạng mở (không mật khẩu), enterprise (802.1X), 6 GHz, AirDrop / AWDL (đang làm), sleep/wake (chưa kiểm chứng).
Bluetooth trên Tahoe chưa kiểm tra, và một lần khởi động lại nóng `bt-state` báo `error` dù controller vẫn chạy (chưa tái hiện).
Tải lên chậm hơn tải xuống. A-MSDU nhận được cố ý tắt. Chi tiết và kế hoạch xem **[docs/ROADMAP.md](docs/ROADMAP.md)**.

## Cài nhanh

1. Tải release đúng với macOS của bạn (bảng trên) ở [Releases](https://github.com/thanhnndev/mt7925-macos/releases), giải nén
   `itlmt7925.kext` và `itlmt7925Native.kext`.
2. Chép cả hai vào `EFI/OC/Kexts`, thêm vào `Kernel → Add` theo thứ tự **`itlmt7925.kext` trước, `itlmt7925Native.kext` sau**.
   Gỡ các kext Wi-Fi khác đang dùng cho card này.
3. Thêm boot-args `itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1 itlmt_fast=6`.
4. Khởi động lại, rồi kết nối mạng từ menu Wi-Fi.
5. Bluetooth (tuỳ chọn): thêm `MtkBtFw.kext`, `BlueToolFixup.kext` và `BtLeDlFix.kext`, xem [GUIDE §13](docs/GUIDE.vi.md#13-bluetooth).

Từng bước, cách kiểm tra, sửa lỗi và quay lại: **[docs/GUIDE.vi.md](docs/GUIDE.vi.md)**.

## Nhánh và release

| Nhánh | Nội dung |
|---|---|
| `main` | driver hiện tại: native trên Tahoe và các bản sửa mới nhất của lõi. Release `v2.*` build từ đây |
| `dev/awdl` | đang làm AirDrop / AWDL (phase 3: phát đã chạy, nhận đã tới được peer manager của Apple). Chỉ chạy khi có `itlmt_awdl=1`. Có thể panic |
| `sequoia` | nhánh macOS 15 đã đóng băng, nguồn của `v1.0` / `v1.1` |

| Release | macOS | Gồm |
|---|---|---|
| `v1.0` | 15 Sequoia | `itlmt7925.kext` (chỉ legacy), `ItlWiFi.app`. Chép từ `v1` của `ga403wm-hackintosh` |
| `v1.1` | 15 Sequoia | `itlmt7925.kext` + `itlmt7925Native.kext` (native, A-MSDU tắt), `ItlWiFi.app`. Chép từ `kext-v1.1` |
| `v2.*` | 26 Tahoe | CI tự build từ `main` khi push tag `v2*` |

## Phần cứng

Mọi máy x86 có card MT7925 (`14c3:7925`) đều có thể nhận driver, nhưng mới có GA403WM được test. Rất mong nhận báo cáo từ
laptop khác: dùng mẫu bug report và đính kèm `scripts/native-wifi-test.sh logs`.

Card gồm hai thiết bị: Wi-Fi qua PCIe và Bluetooth qua USB, nên Wi-Fi và Bluetooth cùng nằm trong repo này, ở các kext riêng.
Bluetooth mới kiểm tra trên GA403WM (USB `0489:e11e`); máy khác có thể dùng USB id khác, xem [docs/BLUETOOTH.md](docs/BLUETOOTH.md).

## Lịch sử

Driver được phát triển trong [thanhnndev/ga403wm-hackintosh](https://github.com/thanhnndev/ga403wm-hackintosh) tới 2026-10-26,
rồi tách sang đây. Phần Wi-Fi được giữ nguyên lịch sử commit; phần Bluetooth được thêm vào ngày 2026-11-02 (lịch sử cũ của nó vẫn ở repo kia).

## Giấy phép

Code: [Clear BSD](LICENSE). Lớp phần cứng MT7925 được viết lại dựa theo driver Linux `mt76` (© MediaTek, BSD-3-Clause-Clear).
Firmware trong `fw/` theo giấy phép riêng của MediaTek. Dự án clean-room: không dùng code từ itlwm, AirportItlwm, OWL hay
OpenDrop (GPL). Xem [NOTICE.md](NOTICE.md).
