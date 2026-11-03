# Hướng dẫn cài Wi-Fi MT7925 trên macOS

[English](GUIDE.md) · [README](../README.vi.md) · [Roadmap](ROADMAP.md)

Hướng dẫn này đưa bạn từ một Hackintosh OpenCore chưa có Wi-Fi tới lúc MT7925 hiện trong menu Wi-Fi, kèm cách kiểm tra, sửa
lỗi và quay lại. Hãy đọc hết một lượt trước khi làm.

**Mục lục:** [1. Chuẩn bị](#1-chuẩn-bị) · [2. Chọn release](#2-chọn-release) · [3. Cài kext](#3-cài-kext) · [4. Native](#4-chế-độ-native-khuyên-dùng) ·
[5. Legacy](#5-chế-độ-legacy-kiểu-ethernet) · [6. Kiểm tra](#6-kiểm-tra) · [7. Build macOS khác](#7-bản-build-macos-khác) ·
[8. Sửa lỗi](#8-sửa-lỗi) · [9. Quay lại](#9-quay-lại--gỡ) · [10. Báo lỗi](#10-báo-lỗi) · [11. Tự build](#11-tự-build) ·
[12. Boot-args](#12-bảng-boot-args) · [13. Bluetooth](#13-bluetooth)

---

## 1. Chuẩn bị

**Bạn cần có:**

- Máy x86_64 boot macOS bằng **OpenCore**, có card MT7925. Kiểm tra từ Windows/Linux (`lspci -nn` ra `14c3:7925`) hoặc từ macOS
  (`ioreg -l | grep -i 7925`).
- **macOS 26 Tahoe** hoặc **macOS 15 Sequoia** (§2 ghi rõ release nào đã kiểm tra trên build nào).
- **Một đường mạng khác** cho lần boot đầu: USB LAN, chia sẻ mạng Android qua USB (HoRNDIS) hoặc iPhone qua USB.
- **Bản backup EFI**, và một cách sửa ESP khi macOS không lên: Windows, Linux, OpenCore shell hoặc USB có EFI thứ hai.

**Gỡ** mọi driver khác đang dùng cho card này khỏi `EFI/OC/Kexts` và `Kernel → Add`: bản `itlmt7925` cũ, hay kext MediaTek thử
nghiệm khác.

OpenCore nạp các kext này như mọi kext inject khác. Không cần chỉnh SIP, không cài gì vào `/Library/Extensions`.

## 2. Chọn release

| macOS của bạn | Release | Nhánh | Kext native build cho |
|---|---|---|---|
| 26 Tahoe | **`v2.*` mới nhất** | `main` | `25G241` (26.7.1) |
| 15 Sequoia | **`v1.1`** | `sequoia` | `24H32` (15.8.1) |

Xem build của bạn bằng `sw_vers -buildVersion`. Nếu khác bảng trên, đọc [§7](#7-bản-build-macos-khác) trước khi nạp kext native.
Kext lõi (`itlmt7925.kext`) không dùng ABI riêng của từng build Apple.

| File | Là gì |
|---|---|
| `itlmt7925.kext.zip` | driver **lõi**, luôn cần. Phần mật khẩu Wi-Fi để trống |
| `itlmt7925Native.kext.zip` | **lớp native**, biến card thành giao diện Wi-Fi thật. Không làm gì nếu thiếu `itlmt_native=1` |
| `ItlWiFi.app.zip` | app thanh menu cho chế độ legacy. Native không cần |
| `SHA256SUMS.txt` | checksum: `shasum -a 256 -c SHA256SUMS.txt` |

## 3. Cài kext

1. Mount ESP: `sudo diskutil mount disk0s1` (hoặc `scripts/esp-automount.sh`). Backup:
   `cp -R /Volumes/EFI/EFI ~/EFI-backup-$(date +%Y%m%d)`.
2. Giải nén hai kext vào `EFI/OC/Kexts/`.
3. Thêm vào `config.plist → Kernel → Add`, **kext lõi trước**:

   | BundlePath | ExecutablePath | PlistPath | MinKernel | MaxKernel |
   |---|---|---|---|---|
   | `itlmt7925.kext` | `Contents/MacOS/itlmt7925` | `Contents/Info.plist` | | |
   | `itlmt7925Native.kext` | `Contents/MacOS/itlmt7925Native` | `Contents/Info.plist` | bản Tahoe: `25.0.0` | bản Sequoia: `24.99.99` |

   `Arch` = `x86_64` (hoặc `Any`), `Enabled` = `true`. Min/MaxKernel giữ cho kext native không bị nạp trên macOS mà nó không được
   build cho, ví dụ sau khi nâng cấp macOS.
4. Chạy `ocvalidate` (hoặc `python3 tools/occonfig.py -c <config.plist> check`).

## 4. Chế độ native (khuyên dùng)

macOS thấy giao diện **Wi-Fi**: có menu Wi-Fi, System Settings → Wi-Fi, quét và kết nối. Mật khẩu nằm trong keychain của macOS,
không nằm trong kext.

**Boot-args** (`NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`, thêm vào sau các tham số bạn đang có):

```
itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1 itlmt_fast=6
```

Hoặc dùng công cụ trong repo (tự tạo backup `config.plist.pre-native`):

```bash
C=/Volumes/EFI/EFI/OC/config.plist
python3 tools/occonfig.py -c $C args add --tag pre-native -- itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1 itlmt_fast=6
python3 tools/occonfig.py -c $C check
```

Nếu trước đó bạn dùng legacy, bỏ `itlmt_m1/m2/m3`. **Không** thêm `itlmt_amsdu=1`. Nếu boot-args đang nằm trong NVRAM
(`nvram boot-args` có in ra), NVRAM sẽ thắng `config.plist`: hãy reset NVRAM ở màn hình OpenCore.

**Khởi động lại**, mở menu Wi-Fi, chọn mạng và nhập mật khẩu.

Mạng kết nối được: WPA2-Personal (gồm mạng WPA2/WPA3 hỗn hợp, qua nhánh WPA2) và WPA/WPA2 hỗn hợp có group TKIP. Mạng
**không** kết nối được: chỉ-WPA3, mạng mở, enterprise, 6 GHz.

## 5. Chế độ legacy (kiểu Ethernet)

Dùng khi native không chạy trên máy bạn (Sequoia), hoặc để có mạng trong lúc cài macOS. Card hiện là **Ethernet**, không có menu
Wi-Fi. Driver tự kết nối sau khi boot, dùng tối đa 4 mạng lưu **dạng chữ thường (không mã hoá)** trong `Info.plist` của kext.

**Boot-args:** `itlmt_m2=1 itlmt_m3=1 itlmt_fast=6`.

**Nhét mạng vào kext.** Trên macOS:

```bash
P=EFI/OC/Kexts/itlmt7925.kext/Contents/Info.plist
/usr/libexec/PlistBuddy -c "Set :IOKitPersonalities:itlmt7925:itl_ssid 'TenWifi'" \
                        -c "Set :IOKitPersonalities:itlmt7925:itl_psk  'mat khau'" "$P"
# thêm mạng: itl_ssid2/itl_psk2 ... itl_ssid4/itl_psk4
```

Trên Windows (PowerShell quyền admin, script đọc các profile Wi-Fi đã lưu trên máy):

```powershell
.\scripts\inject-creds.ps1 -Kext 'E:\EFI\OC\Kexts\itlmt7925.kext' -Ssid 'TenWifi','Hotspot-dien-thoai'
```

> [!CAUTION]
> Kext đã nhét mật khẩu chứa mật khẩu ở dạng chữ thường. Không chia sẻ, không commit, không đính kèm vào issue.

Sau khi boot, **ItlWiFi.app** cho bạn danh sách mạng trên thanh menu để quét, kết nối thêm mạng và đo tốc độ.

## 6. Kiểm tra

```bash
sysctl -n kern.bootargs | tr ' ' '\n' | grep itlmt     # boot-args đang thực sự có hiệu lực
kextstat | grep itlmt                                   # luôn có com.itlmt7925.kext; native thì có thêm com.itlmt7925.native
ioreg -rc itlmt7925 -d1 | grep wifi-                    # wifi-state = "connected", wifi-rate, kênh...
networksetup -listallhardwareports | grep -A2 Wi-Fi     # native: có cổng Wi-Fi
networkQuality -v                                       # đo tốc độ
```

## 7. Bản build macOS khác

Lớp native dùng các giao diện riêng của Apple (`IO80211Controller`, `IO80211InfraInterface`, Skywalk). Bố cục của chúng có thể đổi
giữa các bản build, và lệch bố cục sẽ làm **danh sách mạng bị lỗi chữ hoặc kernel panic**. Hiện mới kiểm tra trên Tahoe
**25G241** và Sequoia **24H32**.

Trên build khác, hãy kiểm tra liên kết với kernel collection của bạn trước (chạy trên Mac, trong bản clone repo):

```bash
KC=/System/Library/KernelCollections/BootKernelExtensions.kc python3 tools/check_kext_links.py \
    build/itlmt7925Native.kext build/itlmt7925.kext
```

Exit 0 chỉ chứng minh các symbol liên kết được, chưa chứng minh bố cục struct khớp. Lần boot đầu nhớ để sẵn đường mạng dự
phòng. Cách port sang build mới: [IO80211-ABI.md](IO80211-ABI.md) và mục "Tahoe (25G241) first load" trong
[TEST-NATIVE-WIFI.md](TEST-NATIVE-WIFI.md).

## 8. Sửa lỗi

| Triệu chứng | Cách xử lý |
|---|---|
| Không có cổng Wi-Fi | `kextstat \| grep itlmt`. Nếu thiếu `com.itlmt7925.native`: kiểm tra thứ tự Kernel → Add (lõi trước), `MaxKernel` của kext native (nếu là `24.99.99` thì kext không nạp trên Tahoe), boot-args có hiệu lực chưa (NVRAM ghi đè config) |
| Kext native lỗi `0xdc00800e` | `Info.plist` của kext lõi thiếu `OSBundleCompatibleVersion`. Dùng kext lõi cùng release |
| Tên mạng trong menu bị lỗi chữ hoặc trống | ABI lệch: kext native không khớp build macOS (§7). Quay về legacy (§9) |
| Panic có `itlmt7925Native` / `IO80211` | boot với kext native bị tắt (dùng EFI khác hoặc sửa config), rồi báo lỗi kèm 30 dòng đầu của file panic |
| Kết nối được, khoảng 60 giây sau rớt | đã sửa trên Tahoe từ round 27. Hãy cập nhật `v2.*` |
| Một mạng cứ báo sai mật khẩu | driver tự hạ mức khi bắt tay 4-way lỗi. Nếu vẫn lỗi, thử `itlmt_fast=3` và báo lại. Mạng chỉ-WPA3 thì không kết nối được |
| Thiếu mạng 2.4 GHz ở chỗ đông mạng | đã sửa trên `main` (bảng AP 128). Hãy cập nhật |
| Rất chậm (khoảng 50 KB/s) | có `itlmt_amsdu=1`: bỏ nó đi |
| Tải lên chậm hơn tải xuống nhiều | đã biết: đường TX chưa được tối ưu |
| Nhiều dịch vụ Wi-Fi `en*` cũ | System Settings → Network: xoá dịch vụ Wi-Fi cũ và thêm lại |
| Mất Wi-Fi sau sleep/wake | chưa kiểm chứng sleep/wake. Tắt/bật Wi-Fi, không được thì khởi động lại |

## 9. Quay lại / gỡ

**Từ native về legacy:**

```bash
C=/Volumes/EFI/EFI/OC/config.plist
python3 tools/occonfig.py -c $C args del --tag pre-legacy -- itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1
python3 tools/occonfig.py -c $C args add --tag pre-legacy2 -- itlmt_m2=1 itlmt_m3=1 itlmt_fast=6
python3 tools/occonfig.py -c $C check
```

Sau đó nhét mạng vào kext như §5. Kext native để nguyên cũng được: thiếu `itlmt_native=1` thì nó không làm gì.

**Gỡ hẳn:** xoá hai mục trong `Kernel → Add`, xoá hai kext khỏi `EFI/OC/Kexts`, xoá mọi boot-arg `itlmt_*`.

**macOS không boot được:** từ Windows/Linux/USB, chép `config.plist.<tag>` (bản backup `occonfig.py` tạo) hoặc bản backup EFI đè lên
`config.plist`. Nếu bạn đã dùng `scripts/native-wifi-test.sh install` thì chạy `scripts/native-wifi-test.sh restore`.

## 10. Báo lỗi

```bash
scripts/native-wifi-test.sh logs     # ghi ra ~/itlmt-backup/logs-<thời gian>/
```

Mở issue theo mẫu bug report, ghi rõ: model máy, `sw_vers`, release hoặc commit, boot-args, chế độ đang dùng, bạn đã làm gì,
chuyện gì xảy ra. Đính kèm thư mục log (nén zip). Nếu panic thì thêm 30 dòng đầu của `/Library/Logs/DiagnosticReports/*.panic`.
**Log có tên mạng (SSID/BSSID) và địa chỉ MAC**: hãy che chúng trước khi đăng.

## 11. Tự build

Trên macOS có Xcode command line tools:

```bash
git clone https://github.com/thanhnndev/mt7925-macos && cd mt7925-macos
git checkout main                                   # Tahoe; macOS 15 thì dùng "sequoia"
bash scripts/build.sh itlmt7925 itlmt7925Native     # ra build/*.kext
bash scripts/build.sh MtkBtFw BtLeDlFix             # kext Bluetooth (BtLeDlFix còn clone acidanthera/Lilu)
bash scripts/build-itlwifi.sh                       # ra build/ItlWiFi.app
```

Để thử nghiệm nhanh trên Mac, `scripts/native-wifi-test.sh install build/itlmt7925Native.kext --stage b` sẽ backup ESP (cả boot-args
trong NVRAM), cài kext và đặt boot-args. `restore` để hoàn tác.

## 12. Bảng boot-args

| boot-arg | Chế độ | Ý nghĩa |
|---|---|---|
| `itlmt_native=1` | native | bật lớp `IO80211Controller` |
| `itlmt_native_if=1` | native | tạo giao diện Wi-Fi |
| `itlmt_native_dp=1` | native | nối giao diện với đường dữ liệu. Lõi quét và kết nối cho macOS, lớp Ethernet legacy thì tắt |
| `itlmt_fast=N` | cả hai | mức tối đa của đường dữ liệu, 0-6. **6** = HE, 160 MHz, 2×2, Block Ack. Tự hạ mức khi kết nối lỗi |
| `itlmt_m2=1 itlmt_m3=1` | legacy | tự quét và kết nối bằng các mạng đã nhét vào kext |
| `itlmt_nopwr=1` | tuỳ chọn | không gửi bảng công suất theo vùng khi kết nối |
| `itlmt_no160=1` | tuỳ chọn | giới hạn băng thông ở 80 MHz |
| `itlmt_stat=1` | tuỳ chọn | ghi `stat-log` / `rx-log` / `ts-log` vào registry để chẩn đoán |
| `itlmt_native_nvtrace=1` | native, tuỳ chọn | lưu trace lúc khởi động vào NVRAM (chỉ để chẩn đoán) |
| `itlmt_amsdu=1` | chỉ debug | **không dùng**: tốc độ tụt mạnh |
| `itlmt_awdl=1` | thử nghiệm | bật giao diện AWDL (`main`: chỉ giao diện; `dev/awdl`: phát + nhận). Không dùng hằng ngày; `dev/awdl` có thể panic |

## 13. Bluetooth

MT7925 gồm hai thiết bị: Wi-Fi qua PCIe (mọi phần ở trên) và Bluetooth qua USB (`0489:e11e` trên GA403WM; kiểm tra máy bạn bằng
`ioreg -p IOUSB -l | grep -i -B2 -A8 Wireless_Device`). Nửa Bluetooth độc lập với kext Wi-Fi: cài cái nào cũng được. Ghi chép điều
tra, tình trạng và việc còn mở: [BLUETOOTH.md](BLUETOOTH.md).

**Tình trạng:** mới kiểm tra trên macOS 15 (`24H32`). Controller lên được, tai nghe classic kết nối được, chuột BLE kết nối được nhờ
`BtLeDlFix`. **Chưa kiểm tra trên macOS 26 Tahoe.** `BtLeDlFix` vá `bluetoothd` theo mẫu byte nên trên macOS khác có thể không làm gì
(chuột BLE lại không kết nối được, các thứ khác không hỏng).

**Cài:**

1. Lấy `MtkBtFw.kext` và `BtLeDlFix.kext` (release `bt-v1.0`, hoặc tự build ở §11), và `BlueToolFixup.kext` từ
   [acidanthera/BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM/releases) (đã dùng bản 2.7.2). `BlueToolFixup` và `BtLeDlFix`
   cần [Lilu](https://github.com/acidanthera/Lilu).
2. Chép vào `EFI/OC/Kexts`, thêm vào `Kernel → Add` sau Lilu: `MtkBtFw.kext` (`Contents/MacOS/MtkBtFw`), `BlueToolFixup.kext`,
   `BtLeDlFix.kext`.
3. Trong `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82` đặt `bluetoothExternalDongleFailed` = `00` và
   `bluetoothInternalControllerInfo` = 14 byte `00`, đồng thời liệt kê cả hai tên trong `NVRAM → Delete` cùng GUID để OpenCore ghi
   lại mỗi lần khởi động.
4. Khởi động lại rồi kiểm tra:

```bash
ioreg -rc MtkBtFw -l | grep -E '"bt-state"|"bt-log"'      # bt-state = ready; bt-log liệt kê từng bước
system_profiler SPBluetoothDataType | head -12             # có địa chỉ và chipset thật thay cho giá trị giả
```

**Gỡ:** đặt `Enabled` = false cho `MtkBtFw.kext`, `BlueToolFixup.kext` (và `BtLeDlFix.kext` nếu chỉ muốn bỏ bản vá BLE), hoặc khôi phục
bản backup EFI. Boot-arg `-btledlfxoff` chỉ tắt `BtLeDlFix`.
