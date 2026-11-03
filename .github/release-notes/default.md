> Experimental, AI-written, tested on one laptop (ASUS GA403WM). Kernel code: it can panic. Back up first and keep another way online.

**Install:** [docs/GUIDE.md](https://github.com/thanhnndev/mt7925-macos/blob/main/docs/GUIDE.md) ·
[Tiếng Việt](https://github.com/thanhnndev/mt7925-macos/blob/main/docs/GUIDE.vi.md) ·
**What works / what's next:** [docs/ROADMAP.md](https://github.com/thanhnndev/mt7925-macos/blob/main/docs/ROADMAP.md)

| File | What | Install |
|---|---|---|
| `itlmt7925.kext.zip` | core driver, always needed. Credentials empty | `EFI/OC/Kexts`, first in `Kernel → Add` |
| `itlmt7925Native.kext.zip` | native Wi-Fi front end | after the core |
| `ItlWiFi.app.zip` | menu bar app for legacy (Ethernet-style) mode | optional |
| `SHA256SUMS.txt`, `LICENSE`, `LICENCE.mediatek` | checksums and licenses | |

Native mode boot-args: `itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1 itlmt_fast=6`.
