#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Downloads the MediaTek MT7927 (MT6639) Wi-Fi and Bluetooth firmware from linux-firmware into fw/, so scripts/build.sh embeds it.
# The files are redistributable under the MediaTek licence (fw/LICENCE.mediatek), but are not stored in this repository.
set -euo pipefail
cd "$(dirname "$0")/.."
FILES="WIFI_MT6639_PATCH_MCU_2_1_hdr.bin WIFI_RAM_CODE_MT6639_2_1.bin BT_RAM_CODE_MT6639_2_1_hdr.bin"
BASES="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek/mt7927
https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/mediatek/mt7927"
for f in $FILES; do
  ok=0
  for b in $BASES; do
    if curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 -o "fw/$f.tmp" "$b/$f"; then mv "fw/$f.tmp" "fw/$f"; ok=1; break; fi
  done
  rm -f "fw/$f.tmp"
  if [ $ok != 1 ]; then
    case "$f" in
      BT_*) echo "warning: $f is not in linux-firmware (yet): Bluetooth for the MT7927 will not be built, Wi-Fi is unaffected"; continue ;;
      *)    echo "could not download $f"; exit 1 ;;
    esac
  fi
  printf '%-36s %8d bytes  %s\n' "$f" "$(wc -c < "fw/$f")" "$(shasum -a 256 "fw/$f" | cut -c1-16)"
done
# sanity: the patch header starts with an ASCII build date, the RAM code has a trailer
head -c 16 fw/WIFI_MT6639_PATCH_MCU_2_1_hdr.bin | LC_ALL=C grep -q '[0-9]' || { echo "patch file does not look like MediaTek firmware"; exit 1; }
echo "ok: MT7927 firmware in fw/"
