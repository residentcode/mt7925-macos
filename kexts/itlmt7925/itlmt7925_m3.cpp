// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// Portions derived from the Linux mt76 driver, Copyright (C) 2020, 2023 MediaTek Inc. (BSD-3-Clause-Clear)
// itlmt7925 - M3: join the configured WPA2-PSK network and expose an Ethernet interface.
// Enabled with boot-arg itlmt_m3=1 (implies itlmt_m2=1; the target AP is picked from the M2 scan).
// Credentials come from the kext Info.plist personality keys itl_ssid / itl_psk (filled in locally by scripts/inject-creds.ps1,
// never committed). Progress is logged with m3log() into "m3-log" (mirrored to NVRAM itln0..itln3 by ItlDiag) and a one-line
// summary in "m3-res" (itln4).
// Port of the connac3 connect path in Linux mt76 (BSD-3-Clause-Clear, Copyright (C) MediaTek Inc.): mt7925/main.c, mcu.c, mac.c.
#include "itlmt7925.hpp"
#include "mt7925_regs.h"
#include "itl_wpa.h"

#include <string.h>
#include <stdarg.h>
#include <libkern/libkern.h>
#include <sys/random.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

#define ITL_TIMEOUT -110
#define ITL_EIO     -5
#define ITL_EINVAL  -22
#define ITL_ESTALE  -70

void itlmt7925::m3log(const char *fmt, ...)
{
    char tmp[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    IOLog("itlmt7925: M3 %s\n", tmp);
    size_t n = strlen(tmp);
    if (m3len + n + 2 >= sizeof(m3buf)) {
        // ring: drop the oldest third so the newest lines always survive (the log used to freeze at 8 KB, right at link-up)
        size_t drop = m3len / 3;
        while (drop < m3len && m3buf[drop] != ';') drop++;
        if (drop < m3len) drop++;
        memmove(m3buf, m3buf + drop, m3len - drop + 1);
        m3len -= drop;
    }
    if (m3len + n + 2 < sizeof(m3buf)) {
        memcpy(m3buf + m3len, tmp, n);
        m3len += n;
        m3buf[m3len++] = ';';
        m3buf[m3len] = 0;
    }
    setProperty("m3-log", m3buf);
    nvramLog(tmp);                            // last step survives a hang or reboot (variable itlmt7925-log)
    nvramChunks(m3buf);                       // and the whole log (itlk0..itlk4)
}

void itlmt7925::m3Publish(const char *stage)
{
    snprintf(m3res, sizeof(m3res), "%s tgt=%d ch=%u bssid=%02x%02x%02x ie=%u", stage, tgtFound ? 1 : 0, tgtCh, tgtBssid[3],
             tgtBssid[4], tgtBssid[5], tgtIeLen);
    setProperty("m3-res", m3res);
    size_t sl = strlen(stage);
    if ((sl >= 4 && !strcmp(stage + sl - 4, "fail")) || !strcmp(stage, "nocred")) evLog("connect failed: %s (lvl %u)", stage, lvl);
}

// Called by rxParseFrame for every beacon / probe response. `frame` points at the 802.11 header.
void itlmt7925::m3Capture(const uint8_t *frame, uint32_t len, const char *ssid, uint8_t ch)
{
    if (!m3On || !m3ssid[0] || len < 36 || strcmp(ssid, m3ssid) != 0) return;
    // prefer 5 GHz; otherwise keep the first one seen
    if (tgtFound && !(ch > 14 && tgtCh <= 14)) return;
    tgtFound = true;
    memcpy(tgtBssid, frame + 16, 6);
    tgtCh = ch;
    tgtBcnInt = (uint16_t)(frame[32] | (frame[33] << 8));
    tgtCap = (uint16_t)(frame[34] | (frame[35] << 8));
    tgtIeLen = len - 36 > sizeof(tgtIe) ? (uint32_t)sizeof(tgtIe) : len - 36;
    memcpy(tgtIe, frame + 36, tgtIeLen);
}

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------
static inline uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

#define M3_BSS_IDX     0
#define M3_BMC_WCID    19                   // MT792x_WTBL_RESERVED - bss idx
#define M3_AP_WCID     1

#define UNI_STA_REC_UPDATE   0x03
#define UNI_ROC              0x27
#define UNI_SET_DOMAIN_INFO  0x15
#define UNI_BSS_INFO_UPDATE  0x02
#define UNI_EDCA_UPDATE      0x04

// MCU message under construction: a fixed header (4 bytes BSS_INFO, 8 bytes STA_REC; tlv_num is at offset 2 in both)
// followed by TLVs.
struct M3Msg {
    uint8_t b[640];
    uint16_t n;
    void init(uint16_t hdr) { bzero(b, sizeof(b)); n = hdr; }
    uint8_t *tlv(uint16_t tag, uint16_t len)
    {
        uint8_t *p = b + n;
        put16(p, tag); put16(p + 2, len);
        n += len;
        uint16_t c = (uint16_t)(b[2] | (b[3] << 8)) + 1;
        b[2] = (uint8_t)c; b[3] = (uint8_t)(c >> 8);
        return p;
    }
};

// Sends a UNI command and logs the firmware's status word (event payload: cid, pad[3], status le32 at event offset 48).
int itlmt7925::m3Cmd(uint8_t cid, const void *payload, size_t plen, const char *name, bool noWait)
{
    static uint8_t ev[512];
    uint32_t evl = 0;
    int rc = uniSend(cid, payload, plen, false, ev, sizeof(ev), &evl, 3000, noWait);
    { McuHist &h = mcuHist[mcuHistN++ % 24]; h.name = name; h.rc = rc; h.ms = nowMs(); }
    // The AWDL ROC fires five commands per 440 ms period; logging every one of them evicts the RX diagnostics from the log
    // ring within seconds (docs/ROADMAP-AWDL.md §10.22). Only failures are reported while the AWDL window is running.
    if (!awdlQuiet || rc)
        m3log("%s rc=%d st=%d", name, rc, evl >= 52 ? (int)le32(ev + 48) : -1);
    // A firmware that stopped answering MCU commands is dead, not busy (2026-11-04: "postmortem silent 4s" after link-up,
    // then the Family disconnected before the 15 s beacon watch fired, and every later command - bss_info, scans - timed
    // out while nothing reset the chip: Wi-Fi stayed down until a reboot). Two timeouts in a row ask the supervisor for the
    // same reset + firmware reload the beacon watch uses.
    if (rc == ITL_TIMEOUT) {
        if (++m3CmdTimeouts == 2 && !recoverWanted) {
            recoverWanted = true;
            m3log("mcu: %s and the command before it timed out: chip reset requested", name);
            mcuHistDump();
            evLog("firmware not answering MCU commands: reset requested");
        }
    } else if (rc == 0) {
        m3CmdTimeouts = 0;
    }
    return rc;
}

// The command history, oldest first, as one or two log lines: which sequence the firmware stopped answering in.
void itlmt7925::mcuHistDump()
{
    char b[190];
    size_t n = 0;
    uint32_t first = mcuHistN > 24 ? mcuHistN - 24 : 0;
    uint32_t base = mcuHist[first % 24].ms;
    for (uint32_t i = first; i < mcuHistN; i++) {
        const McuHist &h = mcuHist[i % 24];
        int w = snprintf(b + n, sizeof(b) - n, "%s%s:%d@%u", n ? " " : "", h.name ? h.name : "?", h.rc, h.ms - base);
        if (w < 0 || (size_t)w >= sizeof(b) - n) { b[n] = 0; m3log("mcu hist %s", b); n = 0; i--; continue; }
        n += (size_t)w;
    }
    if (n) m3log("mcu hist %s", b);
    m3log("awdl roc latency <10=%u <40=%u <80=%u late=%u never=%u", awdlRocLat[0], awdlRocLat[1], awdlRocLat[2],
          awdlRocLat[3], awdlRocLat[4]);
}

// Everything that arrives on the MCU event ring: frames go to the beacon parser, events to m3Event.
int itlmt7925::m3PumpOnce()
{
    uint32_t before = rxPkts + evCount;
    rxDataPoll();
    const uint8_t *e; uint32_t l; int bi;
    while (ringPeek(rxEvt, &e, &l, &bi)) {
        if (l >= 16) {
            uint32_t d0 = le32(e), type = d0 >> 27, flag = (d0 >> 16) & 0xf;
            if (type == 2 || type == 8 || (type == 7 && flag == 1)) { rxPkts++; rxParseFrame(e, l); }
            else { evCount++; m3Event(e, l); }
        }
        ringRelease(rxEvt, bi);
    }
    ethFlush();
    return (rxPkts + evCount) != before;
}

void itlmt7925::m3Pump(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        m3PumpOnce();
        if (m3sleepy) IOSleep(1); else IODelay(1000);
    }
}

// Unsolicited firmware events we care about.
void itlmt7925::m3Event(const uint8_t *ev, uint32_t len)
{
    if (txPool && len >= 8 && (le32(ev) >> 27) == 6) { m3TxFree(ev, len); return; }
    if (len >= 40 && (le32(ev) >> 27) == 0) { m3Txs(ev, len); return; }              // TX status
    if (m3On && len > 48 && ev[36] == 0x04 && m3fwLogged < 24 && (m3wantFc || m3fwLogged < 6)) {   // firmware log text
        m3fwLogged++;
        char t[96];
        uint32_t k = 0;
        for (uint32_t i = 44; i < len && k < sizeof(t) - 1; i++) t[k++] = (ev[i] >= 0x20 && ev[i] < 0x7f) ? (char)ev[i] : '.';
        t[k] = 0;
        m3log("fw: %s", t);
    }
    if (!m3On || len < 55) return;
    if (ev[36] == UNI_ROC) {   // TLV at 48 {tag, len, bss_idx, tokenid, status, ...} (mt7925_roc_grant_tlv)
        uint16_t tag = (uint16_t)(ev[48] | (ev[49] << 8));
        uint8_t tok = ev[53];
        // Only UNI_EVENT_ROC_GRANT (tag 0) for the request in flight is a grant (Linux mt7925_mcu_uni_roc_event). Until §10.52
        // any ROC event counted, and its token replaced ours: an event of the previous, aborted request (status 173, "after
        // 0 ms") was taken as this one's answer, the abort that followed carried the old token, the firmware's real ROC was
        // never released, it granted nothing after that and stopped answering (2026-11-05 13:0x).
        // The join path (no link yet) keeps accepting a token mismatch, logged, so a firmware that numbered its tokens on its
        // own could still connect; the AWDL windows (link up) never do.
        bool joinPath = !m3linkUp && rocWant && tag == 0;
        if (joinPath && tok != rocTok) m3log("roc event: join grant with tok=%u, asked %u (accepted)", tok, rocTok);
        if (!joinPath && (tag != 0 || !rocWant || tok != rocTok)) {
            rocEvIgnored++;
            if (rocEvIgnored <= 6 || (rocEvIgnored % 100) == 0)
                m3log("roc event ignored: tag=%u tok=%u (want %u, pending=%d) status=%u n=%u", tag, tok, rocTok, rocWant ? 1 : 0,
                      ev[54], rocEvIgnored);
            return;
        }
        rocWant = false;
        rocStatus = ev[54];
        rocGrant = true;
        if (len >= 63) rocBand = ev[62];                    // mt7925_roc_grant_tlv.dbdcband (MT7927 port)
        if (!awdlQuiet) m3log("roc event bss=%u tok=%u status=%u dbdcband=%u", ev[52], tok, ev[54], len >= 63 ? ev[62] : 0xff);
    }
}

// RSN IE (48) for the association request and for msg2: the AP's group cipher, the pairwise cipher this link will use
// (CCMP when the AP offers it, TKIP otherwise) and PSK. Hardcoding CCMP here made mixed WPA/WPA2 APs (TKIP group) either
// refuse the association (status 40: no common cipher) or refuse msg2.
static void m3RsnIe(uint8_t *p, uint8_t group, uint8_t pw)
{
    static const uint8_t t[22] = {0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                  0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00};
    memcpy(p, t, sizeof(t));
    p[7] = group ? group : 4;
    p[13] = pw ? pw : 4;
}

// Rates / RSN of the target AP from the captured beacon IEs.
void itlmt7925::m3ParseAp()
{
    static const uint8_t r5g[8] = {12, 18, 24, 36, 48, 72, 96, 108};    // 500 kb/s units
    static const uint8_t rcck[4] = {2, 4, 11, 22};
    const uint8_t *ie = tgtIe, *end = tgtIe + tgtIeLen;
    apSupp = apBasic = apCck = apBasicCck = 0;
    apRsnLen = 0;
    apGroupCipher = apPairCipher = 0;
    while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
        uint8_t id = ie[0], l = ie[1];
        if (id == 1 || id == 50) {
            for (int i = 0; i < l; i++)
                for (int k = 0; k < 8; k++)
                    if ((ie[2 + i] & 0x7f) == r5g[k]) { apSupp |= 1u << k; if (ie[2 + i] & 0x80) apBasic |= 1u << k; }
            for (int i = 0; i < l; i++)
                for (int k = 0; k < 4; k++)
                    if ((ie[2 + i] & 0x7f) == rcck[k]) { apCck |= 1u << k; if (ie[2 + i] & 0x80) apBasicCck |= 1u << k; }
        } else if (id == 48 && l <= sizeof(apRsn) - 2) {
            apRsnLen = (uint8_t)(l + 2);
            memcpy(apRsn, ie, apRsnLen);
        }
        ie += 2 + l;
    }
    m3log("ap rates supp=%02x basic=%02x cck=%x/%x rsn=%u", apSupp, apBasic, apCck, apBasicCck, apRsnLen);
    m3FastPlan();
    if (apRsnLen >= 14) {
        // version(2) group(4) pairwise count(2) list(4 each) akm count(2) list(4 each) capabilities(2)
        const uint8_t *r = apRsn + 2;
        int rl = apRsnLen - 2;                               // bytes available in r[]
        uint16_t pc = (uint16_t)(r[6] | (r[7] << 8));
        int akmOff = 10 + 4 * pc;
        uint16_t ac = (rl >= akmOff + 2) ? (uint16_t)(r[akmOff - 2] | (r[akmOff - 1] << 8)) : 0;
        uint8_t akm = (rl >= akmOff + 4) ? r[akmOff + 3] : 0;
        int capOff = akmOff + 4 * ac;
        uint16_t cap = (rl >= capOff + 2) ? (uint16_t)(r[capOff] | (r[capOff + 1] << 8)) : 0;
        apGroupCipher = r[5];
        apPairCipher = 0;
        for (uint16_t i = 0; i < pc && 4 * i + 13 < rl; i++) {
            uint8_t t = r[11 + 4 * i];
            if (t == 4) { apPairCipher = 4; break; }         // CCMP whenever the AP offers it, TKIP only without it
            if (t == 2) apPairCipher = 2;
            else if (!apPairCipher) apPairCipher = t;
        }
        m3log("rsn group=%02x pw n=%u first=%02x pick=%02x akm=%02x cap=%04x", r[5], pc, pc ? r[11] : 0,
              apPairCipher, akm, cap);
    }
}

// Features at `level` for the current AP: bit 0 A-MSDU, bit 1 HE, bit 2 160 MHz channel. Used to skip levels that change nothing.
uint8_t itlmt7925::fastSig(uint8_t level)
{
    bool g2 = tgtCh <= 14;
    uint8_t bw, center;
    itl_caps_channel(&apCaps, tgtCh, level >= 5 && !g2 && !no160, &bw, &center);
    return (level >= 4 && !noAmsdu ? 1 : 0) | ((level >= 6 && apCaps.he) ? 2 : 0) | (bw == 3 ? 4 : 0);
}

// HT/VHT/HE plan (boot-arg itlmt_fast=<level>): what the AP offers, the channel width to run at and the rate to report to macOS.
// Stays off (legacy 6 Mb/s path, exactly as before) unless the AP advertises HT. 2.4 GHz is HT / HE only (no VHT).
void itlmt7925::m3FastPlan()
{
    fastActive = false;
    txPool = false;
    fastHe = fastAmsdu = fastAllow160 = false;
    if (!m3fast || lvl < 1) return;
    itl_caps_parse(tgtIe, tgtIeLen, &apCaps);
    bool g2 = tgtCh <= 14;
    if (g2) { apCaps.vht = false; apCaps.vhtOp = false; }
    if (!apCaps.ht) {
        m3log("fast: off (ch=%u ht=%d)", tgtCh, apCaps.ht ? 1 : 0);
        baLog("fast off ch=%u ht=%d", tgtCh, apCaps.ht ? 1 : 0);
        return;
    }
    itl_caps_channel(&apCaps, tgtCh, lvl >= 5 && !g2 && !no160, &fastBw, &fastCenter);
    fastAllow160 = fastBw == 3;
    fastAmsdu = lvl >= 4 && !noAmsdu;
    fastHe = lvl >= 6 && apCaps.he;
    fastNss = itl_caps_nss(&apCaps);
    fastRate = itl_caps_max_rate(&apCaps, fastBw, fastHe);
    fastActive = true;
    txPool = lvl >= 3;
    txClrAll();
    m3log("fast: on lvl=%u ht=%d vht=%d he=%d/%d bw=%u center=%u nss=%u rate~%u htcap=%04x vhtcap=%08x", lvl, apCaps.ht ? 1 : 0,
          apCaps.vht ? 1 : 0, apCaps.he ? 1 : 0, fastHe ? 1 : 0, fastBw, fastCenter, fastNss, fastRate, apCaps.htCap, apCaps.vhtCap);
    baLog("fast on lvl=%u ch=%u bw=%u c=%u nss=%u ht=%04x vht=%08x ampdu=%02x vhtop=%u/%u/%u", lvl, tgtCh, fastBw, fastCenter, fastNss,
          apCaps.htCap, apCaps.vhtCap, apCaps.ampduParm, apCaps.vhtOpWidth, apCaps.vhtSeg0, apCaps.vhtSeg1);
    baLog("fast feat amsdu=%d he=%d(ap %d) 160=%d heph=%02x%02x%02x%02x hemac=%02x%02x%02x%02x map=%04x/%04x", fastAmsdu ? 1 : 0,
          fastHe ? 1 : 0, apCaps.he ? 1 : 0, fastAllow160 ? 1 : 0, apCaps.hePhy[0], apCaps.hePhy[1], apCaps.hePhy[2], apCaps.hePhy[3],
          apCaps.heMac[0], apCaps.heMac[1], apCaps.heMac[2], apCaps.heMac[3], apCaps.heRx80, apCaps.heRx160);
}

// ---------------------------------------------------------------------------------------------
// hardware / firmware prep (mt7925_mac_init, mt7925_mcu_set_channel_domain)
// ---------------------------------------------------------------------------------------------
#define MT_MDP_DCR0              0x820cc800
#define MT_MDP_DCR0_DAMSDU_EN    (1u << 15)
#define MT_MDP_DCR1              0x820cc804
#define MT_WTBL_UPDATE_REG       0x820d4380
#define MT_WTBL_ITCR             0x820d43b0
#define MT_WTBL_ITDR0            0x820d43b8
#define MT_WTBL_ITDR1            0x820d43bc

int itlmt7925::m3HwPrep()
{
    // MDP: max rx length 1536, hardware A-MSDU de-aggregation
    uint32_t v = rdA(MT_MDP_DCR1);
    wrA(MT_MDP_DCR1, (v & ~0xfff8u) | (1536u & 0xfff8u));
    wrA(MT_MDP_DCR0, rdA(MT_MDP_DCR0) | MT_MDP_DCR0_DAMSDU_EN);
    m3log("mdp dcr0=%08x dcr1=%08x", rdA(MT_MDP_DCR0), rdA(MT_MDP_DCR1));

    if (!m3hw) {
        // steps the Linux driver does once after the firmware runs: firmware log to host, eeprom = efuse, RTS threshold
        m3hw = true;
        static const uint8_t fwlog[12] = {0, 0, 0, 0, 0, 0, 8, 0, 1, 0, 0, 0};            // UNI_WSYS_CONFIG_FW_LOG_CTRL, ctrl 1
        static const uint8_t eeprom[12] = {0, 0, 0, 0, 2, 0, 8, 0, 0, 0, 0, 0};          // UNI_EFUSE_BUFFER_MODE: efuse, whole
        static const uint8_t rts[16] = {0, 0, 0, 0, 8, 0, 12, 0, 0x2b, 0x09, 0, 0, 2, 0, 0, 0};   // band 0, len 0x92b, pkt 2
        m3Cmd(0x0b, fwlog, sizeof(fwlog), "fw_log");
        m3Cmd(0x2d, eeprom, sizeof(eeprom), "eeprom(efuse)");
        m3Cmd(0x08, rts, sizeof(rts), "rts_thresh");
    }

    // fixed-rate table: idx 14 + i, i = mt76_rates[]: CCK 1,2,5.5,11 then OFDM 6..54 Mb/s
    static const uint16_t rates[12] = { (0 << 6) | 0, (0 << 6) | 1, (0 << 6) | 2, (0 << 6) | 3,
                                        (1 << 6) | 11, (1 << 6) | 15, (1 << 6) | 10, (1 << 6) | 14,
                                        (1 << 6) | 9, (1 << 6) | 13, (1 << 6) | 8, (1 << 6) | 12 };
    for (int i = 0; i < 12; i++) {
        wrA(MT_WTBL_ITDR0, rates[i]);
        wrA(MT_WTBL_ITDR1, 1u << 6);                         // MT_WTBL_SPE_IDX_SEL
        wrA(MT_WTBL_ITCR, (1u << 16) | (1u << 31) | (uint32_t)(14 + i));
    }
    // clear the admission counters of the two wcids we use
    static const uint16_t wcids[2] = { M3_BMC_WCID, M3_AP_WCID };
    for (int i = 0; i < 2; i++) {
        uint32_t u = rdA(MT_WTBL_UPDATE_REG);
        wrA(MT_WTBL_UPDATE_REG, (u & ~0xfffu) | wcids[i] | (1u << 14));
        pollA(MT_WTBL_UPDATE_REG, 1u << 31, 0, 50);
    }

    // channel domain: world regulatory ("00"), every channel we might join (the scan list in itlmt7925_m2.cpp mirrors this)
    static const uint16_t ch2[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    static const uint16_t ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
                                    144, 149, 153, 157, 161, 165 };
    static uint8_t dom[16 + 8 * 48];
    unsigned n2 = sizeof(ch2) / sizeof(ch2[0]), n5 = sizeof(ch5) / sizeof(ch5[0]);
    bzero(dom, sizeof(dom));
    dom[0] = '0'; dom[1] = '0';
    dom[4] = 0; dom[5] = 3; dom[6] = 3;                      // bw_2g 20/40, bw_5g 20..160, bw_6g
    put16(dom + 8, 2);                                       // tag
    put16(dom + 10, (uint16_t)(8 + (n2 + n5) * 8));          // len
    dom[12] = (uint8_t)n2; dom[13] = (uint8_t)n5; dom[14] = 0;
    uint8_t *w = dom + 16;
    for (unsigned i = 0; i < n2; i++, w += 8) put16(w, ch2[i]);
    for (unsigned i = 0; i < n5; i++, w += 8) put16(w, ch5[i]);
    int rc = m3Cmd(UNI_SET_DOMAIN_INFO, dom, (size_t)(w - dom), "set_domain");
    if (rc) return rc;
    if (!clcCc[0] && !noPwr) m3PowerSetup();
    return rc;
}

// Power tables: CLC (world, indoor) + the SKU limits. Best effort: the firmware ran without them before (they were added without
// a hardware run and the efuse query failed on every join of the first one), so a failure must not abort the connection.
int itlmt7925::m3PowerSetup()
{
    sarInit();
    uint8_t encap = 0;
    int rc = efuseRead(0xa71, &encap);
    if (rc) {
        m3log("efuse hw type failed rc=%d, joining without power tables", rc);
        clcCc[0] = 0;
        return 0;
    }
    hwEncap = encap & 3;
    rc = m3Clc("00", 1);                                   // world, indoor; same domain as set_domain
    if (!rc) rc = m3TxPower();
    if (rc) {
        clcCc[0] = 0;                                       // retry the complete setup on the next join
        m3log("power tables failed rc=%d, joining without them", rc);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// BSS_INFO_UPDATE for the station interface (mt7925_mcu_add_bss_info_sta, non-MLD)
// ---------------------------------------------------------------------------------------------
// UNI_BSS_INFO_BASIC + SEC + RATE + QBSS + MLD + IFS_TIME (+ RLM, HE_BASIC, BSS_COLOR once associated) for our one BSS.
// Default shape (grpWcid19 == 0, the path that has worked on hardware): sta_idx = the AP wcid in every phase. With
// itl_grp19=1 the three shapes follow mt76 instead: creation and removal are mt76_connac_mcu_uni_add_dev (sta_idx = the
// BSS wcid 19, conn_state 1, inactive on removal) and only the associated update is mt7925_mcu_add_bss_info with the
// peer link (sta_idx = the AP wcid 1, conn_state 0).
int itlmt7925::m3Bss(M3BssOp op)
{
    bool assoc = op == M3_BSS_ASSOC;
    M3Msg m;
    m.init(4);
    m.b[0] = M3_BSS_IDX;

    uint8_t *b = m.tlv(0, 32);                               // UNI_BSS_INFO_BASIC
    b[4] = (grpWcid19 && op == M3_BSS_REMOVE) ? 0 : 1;       // active (station)
    b[5] = 0;                                                // omac_idx
    b[6] = 0;                                                // hw_bss_idx
    b[7] = vifBand;                                          // band_idx (MT7925: 0xff, MT7927: band of the AP's channel)
    put32(b + 8, 0x10001);                                   // CONNECTION_INFRA_STA
    b[12] = assoc ? 0 : 1;                                   // conn_state
    b[13] = 0;                                               // wmm_idx
    memcpy(b + 14, tgtBssid, 6);
    put16(b + 20, M3_BMC_WCID);
    put16(b + 22, tgtBcnInt ? tgtBcnInt : 100);
    b[24] = 1;                                               // dtim_period
    bool g2 = tgtCh <= 14;
    b[25] = 0x01;                                            // phymode: 5 GHz legacy (PHY_MODE_A)
    if (fastActive && !g2) b[25] |= 0x10 | (apCaps.vht ? 0x20 : 0) | (fastHe ? 0x80 : 0);   // | PHY_MODE_AN | PHY_MODE_AC | PHY_MODE_AX_5G
    if (fastActive && g2) b[25] = 0x02 | 0x04 | 0x08 | (fastHe ? 0x40 : 0);                  // 2.4 GHz: B | G | GN | AX_24G
    put16(b + 26, (grpWcid19 && !assoc) ? M3_BMC_WCID : M3_AP_WCID);   // sta_idx: mt76's BSS wcid before/after the association
    put16(b + 28, (fastActive && g2) ? 1 : 3);               // nonht_basic_phy = PHY_TYPE_ERP_INDEX (2.4 GHz) / OFDM_INDEX
    b[30] = 0;                                               // phymode_ext
    b[31] = 0;                                               // link_idx

    uint8_t *s = m.tlv(16, 8);                               // UNI_BSS_INFO_SEC
    if (m3cipher == 4) { s[4] = 7; s[5] = 6; s[6] = 4; }     // WPA2-PSK, CCMP
    else if (m3cipher == 2) { s[4] = 7; s[5] = 4; s[6] = 2; } // WPA2-PSK, TKIP (mt7925_mcu_bss_sec_tlv: status 4)
    else { s[4] = 0; s[5] = 1; s[6] = 0; }                   // open

    uint8_t *r = m.tlv(11, 16);                              // UNI_BSS_INFO_RATE
    put16(r + 6, (1 << 6) | (1 << 8) | (1 << 10));           // basic_rate = OFDM_BASIC_RATE
    r[12] = 0;                                               // short_preamble (5 GHz: no)
    r[13] = 18;                                              // bc_fixed_rate: table idx 14 + 4 (OFDM 6M)
    r[14] = 18;                                              // mc_fixed_rate

    uint8_t *q = m.tlv(15, 8);                               // UNI_BSS_INFO_QBSS
    q[4] = 1;

    uint8_t *ml = m.tlv(26, 20);                             // UNI_BSS_INFO_MLD
    ml[4] = 0xff;                                            // group_mld_id
    ml[5] = M3_BSS_IDX + 32;                                 // own_mld_id
    memcpy(ml + 6, mac, 6);
    ml[12] = 0xff;                                           // remap_idx
    ml[13] = 0xff;                                           // link_id

    uint8_t *f = m.tlv(23, 20);                              // UNI_BSS_INFO_IFS_TIME
    f[4] = 1;                                                // slot_valid
    put16(f + 8, 9);                                         // slot_time

    if (assoc) {
        uint8_t *rl = m.tlv(2, 16);                          // UNI_BSS_INFO_RLM
        rl[4] = tgtCh;                                       // control_channel
        rl[5] = tgtCh;                                       // center_chan (20 MHz)
        rl[6] = 0;
        rl[7] = 0;                                           // CMD_CBW_20MHZ
        rl[8] = 1; rl[9] = 1;                                // tx / rx streams
        rl[10] = 0;                                          // ht_op_info
        if (fastActive) {                                    // mt7925_mcu_bss_rlm_tlv: AP's width, both streams, "HT 40 allowed", sco from control vs center
            rl[5] = fastCenter;
            rl[7] = fastBw;
            rl[8] = rl[9] = 2;
            rl[10] = fastBw ? 4 : 0;
            rl[11] = tgtCh < fastCenter ? 1 : tgtCh > fastCenter ? 3 : 0;   // sco: secondary above / below
        } else {
            rl[11] = 0;                                      // sco
        }
        rl[12] = tgtCh > 14 ? 2 : 1;                         // band
    }
    if (assoc && fastHe) {
        uint8_t *he = m.tlv(5, 16);                          // UNI_BSS_INFO_HE_BASIC: default PE duration, RTS threshold, our MCS maps
        put16(he + 4, 1023);                                 // he_rts_thres
        he[6] = 4;                                           // he_pe_duration
        put16(he + 8, ITL_HE_MCS_MAP);                       // max_nss_mcs[BW80]
        put16(he + 10, fastBw == 3 ? ITL_HE_MCS_MAP : 0xffff);   // [BW160]
        put16(he + 12, 0xffff);                              // [BW8080]
        uint8_t *col = m.tlv(4, 8);                          // UNI_BSS_INFO_BSS_COLOR
        col[4] = apCaps.heColorOn ? 1 : 0;
        col[5] = apCaps.heColor;
    }
    int rc = m3Cmd(UNI_BSS_INFO_UPDATE, m.b, m.n,
                   op == M3_BSS_ADD ? "bss_info(add)" : op == M3_BSS_ASSOC ? "bss_info(assoc)" : "bss_info(del)");
    m3Edca();                                                // Linux: "ensure that enable txcmd_mode after bss_info"
    return rc;
}

// ---------------------------------------------------------------------------------------------
// EDCA_UPDATE (mt7925_mcu_set_tx): contention parameters of the four access categories. Linux sends it after every BSS_INFO
// (BSS_CHANGED_QOS / add_interface); without it the firmware's AC queues kept whatever they start with, and the measured
// uplink was ~1.1 firmware retries per frame at -39 dBm, 1.7 MB/s.
// ---------------------------------------------------------------------------------------------
int itlmt7925::m3Edca()
{
    // mac80211 AC order (VO, VI, BE, BK), as Linux passes it in edca.queue; station defaults (IEEE 802.11-2020 Table 9-155),
    // cw as the exponent (Linux: fls(cw)), txop in 32 us units
    uint8_t aifs[4] = {2, 2, 3, 7}, ecwMin[4] = {2, 3, 4, 4}, ecwMax[4] = {3, 4, 10, 10};
    uint16_t txop[4] = {47, 94, 0, 0};
    static const uint8_t aciToAc[4] = {2, 3, 1, 0};          // ACI 0 BE, 1 BK, 2 VI, 3 VO
    bool wmm = false;
    for (const uint8_t *e = tgtIe, *end = tgtIe + tgtIeLen; e + 2 <= end && e + 2 + e[1] <= end; e += 2 + e[1]) {
        // WMM Parameter element: OUI 00:50:f2, type 2, subtype 1, version, QoS info, reserved, 4 AC records
        if (e[0] != 0xdd || e[1] < 24 || e[2] != 0x00 || e[3] != 0x50 || e[4] != 0xf2 || e[5] != 2 || e[6] != 1) continue;
        const uint8_t *r = e + 10;
        for (int i = 0; i < 4; i++, r += 4) {
            uint8_t ac = aciToAc[(r[0] >> 5) & 3];
            aifs[ac] = (r[0] & 0xf) < 2 ? 2 : (r[0] & 0xf);
            ecwMin[ac] = (r[1] & 0xf) ? (r[1] & 0xf) : 5;    // Linux: cw 0 -> 5 / 10
            ecwMax[ac] = (r[1] >> 4) ? (r[1] >> 4) : 10;
            txop[ac] = (uint16_t)(r[2] | (r[3] << 8));
        }
        wmm = true;
        break;
    }
    uint8_t req[4 + 4 * 12];
    bzero(req, sizeof(req));
    req[0] = M3_BSS_IDX;
    for (int ac = 0; ac < 4; ac++) {
        uint8_t *t = req + 4 + ac * 12;
        put16(t + 0, 0);                                     // MCU_EDCA_AC_PARAM
        put16(t + 2, 12);
        t[4] = (uint8_t)ac;                                  // queue
        t[5] = 0xf;                                          // WMM_PARAM_SET: aifs | cw_min | cw_max | txop
        t[6] = ecwMin[ac];
        t[7] = ecwMax[ac];
        put16(t + 8, txop[ac]);
        t[10] = aifs[ac];
    }
    m3log("edca wmm=%d be aifs=%u cw=%u/%u txop=%u", wmm, aifs[2], ecwMin[2], ecwMax[2], txop[2]);
    return m3Cmd(UNI_EDCA_UPDATE, req, sizeof(req), "edca");
}

// ---------------------------------------------------------------------------------------------
// STA_REC_UPDATE for the AP (mt7925_mcu_sta_cmd, non-MLD station, legacy OFDM peer)
// state: 0 NONE, 1 AUTH, 2 ASSOC
// ---------------------------------------------------------------------------------------------
int itlmt7925::m3Sta(uint8_t state, bool newly, uint16_t aid)
{
    M3Msg m;
    m.init(8);
    m.b[0] = M3_BSS_IDX;
    m.b[1] = M3_AP_WCID & 0xff;                              // wlan_idx_lo
    m.b[4] = 1;                                              // is_tlv_append
    m.b[5] = 0;                                              // muar_idx = omac_idx
    m.b[6] = M3_AP_WCID >> 8;

    uint8_t *b = m.tlv(0, 20);                               // STA_REC_BASIC
    put32(b + 4, 0x10002);                                   // CONNECTION_INFRA_AP (the peer is an AP)
    b[8] = 2;                                                // CONN_STATE_PORT_SECURE
    b[9] = 1;                                                // qos (WMM)
    put16(b + 10, aid);
    memcpy(b + 12, tgtBssid, 6);
    put16(b + 18, newly ? 3 : 1);                            // EXTRA_INFO_VER | NEW

    bool ht = fastActive && state == 2;                      // capabilities go in with the association, not the early "none" record
    if (ht) {                                                // mt7925_mcu_sta_ht_tlv / vht_tlv: the peer's (the AP's) capabilities
        uint8_t *h = m.tlv(9, 8);                            // STA_REC_HT
        put16(h + 4, apCaps.htCap);
        if (apCaps.vht) {
            uint8_t *v = m.tlv(10, 16);                      // STA_REC_VHT
            put32(v + 4, apCaps.vhtCap);
            put16(v + 8, apCaps.vhtRxMcs);
            put16(v + 10, apCaps.vhtTxMcs);
        }
    }

    bool g2 = fastActive && tgtCh <= 14;                     // 2.4 GHz in fast mode: CCK / ERP rates as well
    if (ht && fastHe) {
        uint8_t *he = m.tlv(0x19, 28);                       // STA_REC_HE_V2 (mt76_connac_mcu_sta_he_tlv_v2): the AP's HE capabilities
        memcpy(he + 4, apCaps.heMac, 6);
        memcpy(he + 10, apCaps.hePhy, 11);
        he[21] = ITL_HE_PKT_EXT;                             // pkt_ext: nominal packet padding 16 us
        put16(he + 22, apCaps.heRx80);                       // max_nss_mcs[BW80]
        put16(he + 24, fastBw == 3 ? apCaps.heRx160 : 0);    // [BW160]
    }

    uint8_t *p = m.tlv(0x15, 12);                            // STA_REC_PHY
    put16(p + 4, g2 ? (uint16_t)(apBasicCck | (apBasic << 4)) : (uint16_t)apBasic);   // basic_rate (mac80211 band rate indices)
    p[6] = g2 ? 0x03 : (1u << 3);                            // PHY_TYPE_BIT_HR_DSSS | ERP (2.4 GHz) / PHY_TYPE_BIT_OFDM
    if (ht) {
        uint8_t af = apCaps.ampduParm & 3, mm = (apCaps.ampduParm >> 2) & 7;
        p[6] |= (1u << 4) | (apCaps.vht ? (1u << 5) : 0) | (fastHe ? (1u << 6) : 0);    // PHY_TYPE_BIT_HT | VHT | HE
        if (apCaps.vht) {
            uint8_t vaf = (uint8_t)((apCaps.vhtCap >> 23) & 7);
            if (vaf > af) af = vaf;
        }
        p[7] = (uint8_t)((af & 3) | (mm << 2));              // ampdu: factor | density << 2
        p[10] = af;                                          // max_ampdu_len (connac3)
    }

    uint8_t *ra = m.tlv(1, 16);                              // STA_REC_RA
    put16(ra + 4, (uint16_t)((uint16_t)apSupp << 6) | (g2 ? apCck : 0));   // legacy: RA_LEGACY_OFDM GENMASK(13, 6), RA_LEGACY_CCK GENMASK(3, 0)
    if (ht) memcpy(ra + 6, apCaps.mcsRx, sizeof(apCaps.mcsRx));    // rx_mcs_bitmask: HT MCS the AP receives

    uint8_t *st = m.tlv(7, 16);                              // STA_REC_STATE
    st[4] = state;
    if (ht && apCaps.vht) st[12] = (uint8_t)(fastBw | (fastNss << 4));   // vht_opmode: bandwidth | rx_nss << 4

    if (state != 0) {
        uint8_t *ml = m.tlv(0x20, 28);                       // STA_REC_MLD
        memcpy(ml + 4, tgtBssid, 6);
        put16(ml + 10, M3_AP_WCID);                          // primary_id
        put16(ml + 14, M3_AP_WCID);                          // wlan_id
        ml[16] = 1;                                          // link_num
        put16(ml + 20, M3_AP_WCID);                          // link[0].wlan_id
        ml[22] = M3_BSS_IDX;                                 // link[0].bss_idx
    }

    uint8_t *h = m.tlv(0x2b, 8);                             // STA_REC_HDR_TRANS
    h[4] = 0; h[5] = 1;                                      // from_ds, to_ds
    h[6] = 1;                                                // dis_rx_hdr_tran: we handle 802.11 frames ourselves
    char nm[32];
    snprintf(nm, sizeof(nm), "sta_rec(st=%u)", state);
    return m3Cmd(UNI_STA_REC_UPDATE, m.b, m.n, nm);
}

// STA_REC_UPDATE for the BSS wcid (19) - the message mt7925_vif_cfg_changed sends on BSS_CHANGED_ASSOC:
// mt7925_mcu_sta_update(dev, NULL, vif, &mvif->sta.deflink, true, MT76_STA_INFO_STATE_ASSOC) reaches
// mt7925_mcu_sta_cmd with link_sta == NULL, which appends nothing but STA_REC_HDR_TRANS. This is the update that
// brings wcid 19 up in the firmware's table; without it the entry never exists, so group-addressed TX (and the group
// key installed on this wcid) has no path and the hardware drops those frames.
int itlmt7925::m3StaBss()
{
    M3Msg m;
    m.init(8);
    m.b[0] = M3_BSS_IDX;
    m.b[1] = M3_BMC_WCID & 0xff;                             // wlan_idx_lo
    m.b[4] = 1;                                              // is_tlv_append
    m.b[5] = 0;                                              // muar_idx = omac_idx
    m.b[6] = M3_BMC_WCID >> 8;

    uint8_t *h = m.tlv(0x2b, 8);                             // STA_REC_HDR_TRANS (mt7925_mcu_sta_hdr_trans_tlv)
    h[4] = 0;                                                // from_ds
    h[5] = 1;                                                // to_ds (station vif)
    h[6] = 1;                                                // dis_rx_hdr_tran: we handle 802.11 frames ourselves
    return m3Cmd(UNI_STA_REC_UPDATE, m.b, m.n, "sta_rec_bss");
}

// ---------------------------------------------------------------------------------------------
// ROC (remain on channel) to be allowed to transmit auth / assoc on the AP's channel
// ---------------------------------------------------------------------------------------------
int itlmt7925::m3Roc(bool acquire, int channel, uint8_t reqtype, uint32_t durMs)
{
    uint8_t req[4 + 24];
    bzero(req, sizeof(req));
    uint8_t *t = req + 4;
    // The AWDL sync path holds the radio on its own channel while the infrastructure link stays on tgtCh; channel <= 0 keeps
    // the join path's behaviour (the AP's channel).
    uint8_t ch = channel > 0 ? (uint8_t)channel : tgtCh;
    if (acquire) {
        rocGrant = false;
        rocStatus = 0xff;
        rocTok++;
        rocWant = true;                                      // m3Event parses UNI_ROC even after link up while this is set
        put16(t + 0, 0);                                     // UNI_ROC_ACQUIRE
        put16(t + 2, 24);
        t[4] = M3_BSS_IDX;
        t[5] = rocTok;
        t[6] = ch;                                           // control_channel
        t[7] = 0;                                            // sco
        t[8] = ch > 14 ? 2 : 1;                              // band
        t[9] = 0;                                            // bw 20 MHz
        t[10] = ch;                                          // center_chan
        t[12] = 0;                                           // bw_from_ap
        t[13] = ch;                                          // center_chan_from_ap
        t[15] = reqtype;                                     // 0 JOIN, 1 ROC (Linux mt7925: MT7925_ROC_REQ_JOIN / _ROC)
        put32(t + 16, durMs);                                // maxinterval (ms): the granted time
        t[20] = 0xff;                                        // dbdcband: auto
        int rc = m3Cmd(UNI_ROC, req, sizeof(req), "roc_acquire", reqtype == 1);   // AWDL windows: no wait (Linux)
        if (rc) rocWant = false;
        return rc;
    }
    rocWant = false;
    put16(t + 0, 1);                                         // UNI_ROC_ABORT
    put16(t + 2, 12);
    t[4] = M3_BSS_IDX;
    t[5] = rocTok;
    t[6] = 0xff;                                             // dbdcband
    return m3Cmd(UNI_ROC, req, 4 + 12, "roc_abort", m3linkUp);   // after the join: no wait, as Linux mt7925_mcu_abort_roc
}

// ---------------------------------------------------------------------------------------------
// AWDL PSF sync frame (docs/ROADMAP-AWDL.md phase 3). The Family's AWDL state machine hands the driver one sync frame per
// period (setAWDL_SYNC_FRAME_TEMPLATE) and expects it on the air from the AWDL channel; this firmware has no AWDL channel
// sequencing, so the frame is transmitted inside a ROC window on the social channel and the link returns to the AP's channel
// afterwards. awdlSubmit() runs on the caller's thread (the Family's work queue) and only fills the mailbox.
// ---------------------------------------------------------------------------------------------
int itlmt7925::awdlSubmit(const uint8_t *frame, uint32_t len, uint8_t channel, uint32_t dwellMs, uint64_t awBaseUs,
                          uint16_t awBaseSeq, uint32_t awPeriodUs, uint16_t awFlags, const uint8_t *frame2, uint32_t len2,
                          uint64_t startUs)
{
    bool listenOnly = !frame && !len && !frame2;             // v4: a receive window only
    if (!listenOnly && (!frame || len < 24 || len > sizeof(awdlFrame))) return ITL_EINVAL;
    if (frame2 && (len2 < 24 || len2 > sizeof(awdlFrame2))) return ITL_EINVAL;
    if (cmdLock) IOLockLock(cmdLock);
    if (len) memcpy(awdlFrame, frame, len);
    if (len >= 16 && !(frame[10] & 1) && memcmp(awdlOwnAddr, frame + 10, 6) != 0) {   // addr2 = the AWDL interface
        memcpy(awdlOwnAddr, frame + 10, 6);
        awdlOwnSent = false;
    }
    awdlFrameLen = len;
    if (frame2) memcpy(awdlFrame2, frame2, len2);
    awdlFrame2Len = frame2 ? len2 : 0;
    awdlChannel = channel ? channel : 6;
    awdlDwellMs = dwellMs ? dwellMs : 60;
    awdlAwBaseUs = awBaseUs;
    awdlAwBaseSeq = awBaseSeq;
    awdlAwPeriodUs = awPeriodUs;
    awdlAwFlags = awFlags;
    awdlStartUs = startUs;
    __atomic_add_fetch(&awdlFrameSeq, 1, __ATOMIC_RELEASE);
    if (cmdLock) IOLockUnlock(cmdLock);
    __atomic_or_fetch(&cmdBits, CMD_AWDL_SYNC, __ATOMIC_RELEASE);
    return 0;
}

static uint64_t awdlUptimeUs()
{
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000;                                        // the clock the RX path stamps frames with (rxParseFrame)
}

// The synchronisation parameters TLV (type 4) of an AWDL action frame carries two values that only hold at the instant the
// frame leaves: the TX down counter (TU from now to the next availability window) and the AW sequence number. On Apple's
// hardware the firmware rewrites them per transmission; the template the Family hands over has whatever they were when it
// was built. Field offsets inside the TLV value, as IO80211AWDLPeer::parseAwdlSyncParamsTlv reads them in 25G241: +1 the down
// counter ("dc", u16), +29 the sequence number ("sn", u16), both little endian (docs/ROADMAP-AWDL.md §10.28).
bool itlmt7925::awdlStampSync(uint8_t *frame, uint32_t len, const AwdlTimeline &tl)
{
    if (!tl.periodUs || len < 40) return false;
    uint32_t pos = 40;                                       // 24-byte 802.11 header + 16-byte AWDL action header
    while (pos + 3 <= len) {
        uint8_t type = frame[pos];
        uint32_t l = (uint32_t)frame[pos + 1] | ((uint32_t)frame[pos + 2] << 8);
        if (pos + 3 + l > len) return false;
        if (type == 4 && l >= 31) {
            uint8_t *v = frame + pos + 3;
            uint64_t now = awdlUptimeUs();
            int64_t el = (int64_t)(now - tl.baseUs);
            int64_t per = (int64_t)tl.periodUs;
            int64_t n = el >= 0 ? el / per : -((-el + per - 1) / per);     // floor: windows since (or before) the base
            // The counter runs to the start of the next *extended* window (the next AW this node is present in, every
            // presence-mode AWs, +27): the iPhone's own TLVs carry 29 and 32 TU, more than one 16-TU AW (§10.29, §10.32).
            uint32_t pm = (v[27] >= 1 && v[27] <= 16) ? v[27] : 4;
            int64_t k = n + 1;
            while (((uint32_t)(uint16_t)(tl.baseSeq + k)) % pm) k++;
            uint64_t next = tl.baseUs + (uint64_t)(k * per);
            uint32_t tu = (uint32_t)((next - now + 1023) / 1024);
            uint16_t sn = (uint16_t)(tl.baseSeq + n + ((tl.flags & 1) ? 1 : 0));
            v[1] = (uint8_t)(tu & 0xff);
            v[2] = (uint8_t)(tu >> 8);
            v[29] = (uint8_t)(sn & 0xff);
            v[30] = (uint8_t)(sn >> 8);
            return true;
        }
        pos += 3 + l;
    }
    return false;
}

// DEV_INFO_UPDATE for OMAC 1 with the AWDL address (the same layout attach uses for OMAC 0: {omac_idx, band_idx, pad[2]} +
// {tag 0 ACTIVE, len 12, active, link_idx, omac_addr}). The RMAC acknowledges unicast frames whose addr1 matches an active
// OMAC; with only the station address programmed, a peer's unicast to awdl0 was never ACKed (the iPhone's SYN-ACK arrived
// up to 22 times, the same TCP timestamp each time: 802.11 retries). Sent on the link thread, between radio windows.
void itlmt7925::awdlOwnMacApply()
{
    uint8_t a[6];
    if (cmdLock) IOLockLock(cmdLock);
    memcpy(a, awdlOwnAddr, 6);
    if (cmdLock) IOLockUnlock(cmdLock);
    if (!(a[0] | a[1] | a[2] | a[3] | a[4] | a[5])) return;
    awdlOwnSent = true;
    uint8_t dev[16];
    bzero(dev, sizeof(dev));
    dev[0] = 1; dev[1] = 0xff;                               // omac_idx 1, band auto (as OMAC 0)
    put16(dev + 4, 0); put16(dev + 6, 12);
    dev[8] = 1; dev[9] = 1;                                  // active, link_idx 1
    memcpy(dev + 10, a, 6);
    uint8_t q = awdlQuiet;
    awdlQuiet = 0;                                           // always logged: once per address
    awdlOwnRc = m3Cmd(0x01, dev, sizeof(dev), "awdl own mac (omac 1)");
    awdlQuiet = q;
    m3log("awdl own mac %02x%02x%02x%02x%02x%02x rc=%d", a[0], a[1], a[2], a[3], a[4], a[5], awdlOwnRc);
}

// A null-data frame to the AP with the power-management bit set (doze) or clear (awake), sent before the radio leaves the AP's
// channel for an AWDL window and after it comes back (IEEE 802.11 power save: the AP buffers frames for a dozing station until
// it sees PM=0). ToDS, addr1 = addr3 = the BSSID, on the management queue at the basic rate like the other host-built frames.
void itlmt7925::awdlApPs(bool doze)
{
    if (!m3linkUp || !(tgtBssid[0] | tgtBssid[1] | tgtBssid[2] | tgtBssid[3] | tgtBssid[4] | tgtBssid[5])) return;
    uint8_t f[24];
    bzero(f, sizeof(f));
    f[0] = 0x48;                                             // data, subtype 4 (null function)
    f[1] = doze ? 0x11 : 0x01;                               // ToDS (+ PwrMgt)
    memcpy(f + 4, tgtBssid, 6);
    memcpy(f + 10, mac, 6);
    memcpy(f + 16, tgtBssid, 6);
    M3TxOpt o;
    o.qidx = 0x10;
    o.rateAdapt = false;
    if (m3Tx(f, sizeof(f), 2, 4, o)) awdlPsTx[doze ? 1 : 0]++;
}

// Ends a ROC kept open for a following window (§10.61): what winEnd() does for an off-channel window.
void itlmt7925::awdlHeldClose()
{
    uint8_t ch = awdlHeldCh;
    if (!ch) return;
    awdlHeldCh = 0;
    m3SnifferEnable(false);
    m3Roc(false, ch);
    m3RxFilter(0x80000000u);
    awdlApPs(false);
    uint32_t t = nowMs(), off = t - awdlHeldOffT0;
    awdlOffMs += off;
    awdlOffN++;
    t += off;
    awdlNextWinMs = t ? t : 1;
    awdlQuiet = 0;
    awdlRocActive = 0;
}

void itlmt7925::awdlSyncTx()
{
    if (recoverWanted) {                                     // the firmware is not answering: no more commands until the reset
        awdlHeldCh = 0;
        awdlFrameDone = __atomic_load_n(&awdlFrameSeq, __ATOMIC_ACQUIRE);
        return;
    }
    if (awdlBackoffUntilMs && (int32_t)(nowMs() - awdlBackoffUntilMs) < 0) {   // after a ROC the firmware never granted
        if (awdlHeldCh) awdlHeldClose();
        awdlFrameDone = __atomic_load_n(&awdlFrameSeq, __ATOMIC_ACQUIRE);
        return;
    }
    if (!awdlOwnSent) awdlOwnMacApply();
    uint32_t seq = __atomic_load_n(&awdlFrameSeq, __ATOMIC_ACQUIRE);
    if (seq == awdlFrameDone) return;
    if (awdlHeldCh && awdlChannel != awdlHeldCh) { awdlChainBreak++; awdlHeldClose(); }
    // The AP channel gets at least as long as the last absence before the radio leaves again (at most half the airtime off it).
    // Until §10.43 windows of 100 ms + the ROC came back to back: the AP's beacons fell from ~10/s to 1-6/s, its downlink
    // frames were lost while we were away, TCP stalled and airportd left the network ("broken backhaul").
    // A window on the AP's own channel costs the AP nothing (§10.58), so the share rule does not apply to it.
    // A scheduled window (flags bit 1, §10.62) is exempt: the sequence it belongs to already leaves the AP its EAWs.
    if (!awdlHeldCh && awdlNextWinMs && (int32_t)(nowMs() - awdlNextWinMs) < 0 && !(m3linkUp && awdlChannel == tgtCh) &&
        !(awdlAwFlags & 2)) {
        awdlFrameDone = seq;
        awdlDutySkip++;
        return;
    }
    // Snapshot under the mailbox lock: the interface may hand over the next frame (and timeline) while this one is on air.
    uint8_t *frame = awdlTxBuf, *frame2 = awdlTxBuf2;           // only the link thread touches these
    if (cmdLock) IOLockLock(cmdLock);
    uint32_t len = awdlFrameLen;
    uint32_t len2 = awdlFrame2Len;
    if (len2 && len2 <= sizeof(awdlTxBuf2)) memcpy(frame2, awdlFrame2, len2);
    else len2 = 0;
    uint8_t ch = awdlChannel;
    uint32_t dwellMs = awdlDwellMs;
    AwdlTimeline tl = { awdlAwBaseUs, awdlAwBaseSeq, awdlAwFlags, awdlAwPeriodUs };
    uint64_t startUs = awdlLeadOn ? awdlStartUs : 0;
    if (len && len <= sizeof(awdlTxBuf)) memcpy(frame, awdlFrame, len);
    if (cmdLock) IOLockUnlock(cmdLock);
    awdlFrameDone = seq;                                     // one attempt per submission (a missed ROC is not retried forever)
    if (len > sizeof(awdlTxBuf) || !txData.n) { if (awdlHeldCh) awdlHeldClose(); return; }
    // A ROC held from the last window on this channel (§10.61) is taken only if this window starts soon and ends inside it.
    bool chained = false;
    if (awdlHeldCh) {
        uint64_t now = awdlUptimeUs();
        if (ch == awdlHeldCh && startUs && startUs <= now + 8000 && startUs + (uint64_t)dwellMs * 1000 <= awdlHeldRocEndUs) {
            chained = true;
            awdlChainN++;
            // This window owns the ROC now. Until §10.64 awdlHeldCh stayed set: after this window had sent its own abort, the
            // link loop's hold timeout closed the same ROC a second time (sniffer off, roc_abort, rx_filter, PS null for a ROC
            // that no longer existed), and the firmware went silent within seconds (4 resets in 4 min, §10.63).
            awdlHeldCh = 0;
        } else {
            awdlChainBreak++;
            awdlHeldClose();
        }
    }
    // No window on a DFS channel (5 GHz 52-144). The station has done no channel availability check there, so the firmware
    // never grants the ROC (status 255). A peer's channel sequence can name one (an entry 0x8a/40 MHz-low = 136 was followed
    // on 2026-11-05): the ungranted request, then the sniffer-off and abort after it, are where the firmware went silent at
    // 11:01:33 and took the infrastructure link with it (docs/ROADMAP-AWDL.md §10.49).
    // Only a real, non-DFS 20 MHz channel: 1-13, 36-48 and 149-165 in steps of 4. A garbage entry from a peer's channel sequence
    // (26, 249 and 253 were asked for at 12:32:27 on 2026-11-05) is never granted either, and three of them in a row were
    // followed by the next hang (docs/ROADMAP-AWDL.md §10.50).
    bool okCh = (ch >= 1 && ch <= 13) || (ch >= 36 && ch <= 48 && !(ch % 4)) || (ch >= 149 && ch <= 165 && (ch % 4) == 1);
    if (!okCh) {
        awdlDfsSkip++;
        if (awdlDfsLastCh != ch || awdlDfsSkip <= 2)
            m3log("awdl: no window on channel %u (DFS or invalid, skipped %u)", ch, awdlDfsSkip);
        awdlDfsLastCh = ch;
        return;
    }
    // v5: wait (servicing the AP) until the setup has to begin for the radio to be on the channel when the AW opens. Measured
    // on 2026-11-05: every grant took 10-40 ms after the request, the AW is 16 TU, so a window set up when it was due came up
    // after the peer's PSF/MIF had gone and its unicast never found us (docs/ROADMAP-AWDL.md §10.56).
    if (startUs && !chained) {
        uint64_t now = awdlUptimeUs();
        if (now > startUs + (uint64_t)dwellMs * 500) { awdlLeadStale++; return; }   // more than half the dwell late
        uint32_t leadUs = (m3linkUp && ch == tgtCh) ? 5000u : awdlLeadUs;   // on the AP channel: filter + sniffer only
        uint64_t setupAt = startUs > leadUs ? startUs - leadUs : 0;
        uint32_t w0 = nowMs();
        while (awdlUptimeUs() < setupAt && nowMs() - w0 < 90) m3Pump(1);
        awdlLeadWaitMs += nowMs() - w0;
    }
    uint64_t setupT0 = awdlUptimeUs();
    bool listenOnly = !len;                                  // v4: park on the channel and receive, nothing to send
    if (listenOnly) awdlListenWin++;
    else awdlTxTry++;
    awdlRocCh = ch;
    awdlRocActive = 1;
    { uint32_t t = nowMs(); awdlLastWinMs = t ? t : 1; }
    // The sniffer has to be on for the WHOLE window. In station mode the firmware only hands the host its own BSS, so the
    // promiscuous mode is what makes a peer's PSF/MIF arrive at all; and a peer's frame shows up at an arbitrary point inside
    // its availability window. Leaving the sniffer on only for the few milliseconds between the ROC grant and our own
    // transmission threw away ~90% of the window, which is why no AWDL frame was ever decoded (docs/ROADMAP-AWDL.md §10.22).
    awdlQuiet = 1;
    // Power save toward the AP for the time off its channel (a null-data frame with PM=1 now, PM=0 once back): the AP buffers
    // our downlink instead of retrying it into the void. winEnd() closes every way out of this window.
    // On the AP's own channel (docs/ROADMAP-AWDL.md §10.58) the radio is already where the window is. The iPhone on the same
    // network puts nearly all of its AWDL slots on that channel (its sequence on 2026-11-05: 0x9f/0x97 = 157/149, one slot on
    // 6), yet every window went through the full off-channel cycle - PS null to the AP, a ROC granted after ~33 ms, the abort -
    // and the AP-share rule then kept the radio home as long again. Here: no ROC, no power save, no share penalty; only the
    // RX filter and the sniffer for the dwell, so the AP keeps being served the whole time.
    bool onCh = m3linkUp && ch == tgtCh;
    if (onCh) awdlOnChWin++;
    uint32_t offT0 = chained ? awdlHeldOffT0 : nowMs();
    uint64_t rocEndUs = chained ? awdlHeldRocEndUs : 0;
    // Where a window's time goes (§10.59): host uptime at each step, summed per phase over ROC windows. Phases:
    // 0 PS null + filter + sniffer on, 1 ROC request, 2 grant wait, 3 sniffer config, 4 wait for the AW start, 5 PSF/MIF +
    // dwell, 6 sniffer off, 7 ROC abort, 8 filter + PS null back (winEnd).
    uint64_t phT = awdlUptimeUs();
    auto ph = [&](int i) { uint64_t t = awdlUptimeUs(); if (!onCh && !chained) awdlPhUs[i] += t - phT; phT = t; };
    if (!onCh && !chained) awdlApPs(true);
    auto winEnd = [&]() {
        if (onCh) { m3RxFilter(0x80000000u); return; }
        // Back to the station's own RX filter (enable only, as Linux mt7925_configure_filter without FIF_OTHER_BSS): the
        // promiscuous filter of the window used to stay on between windows, on the AP's channel, for as long as AWDL paused.
        // Twice (12:39:29, 12:49:04 on 2026-11-05) the firmware was found dead at the first command after such a pause of
        // ~47 s (docs/ROADMAP-AWDL.md §10.51).
        m3RxFilter(0x80000000u);
        awdlApPs(false);
        uint32_t t = nowMs(), off = t - offT0;
        awdlOffMs += off;
        awdlOffN++;
        t += off;
        awdlNextWinMs = t ? t : 1;
    };
    // The "pass frames from other BSS" filter is reprogrammed in every window: the firmware resets it when the channel changes,
    // so a filter set once per link was gone by the time the radio parked on the next social channel. All four bits the chip
    // defines are set (docs/ROADMAP-AWDL.md §10.22).
    // BIT(31) enable | BIT(6) other BSS. No longer BIT(2) (FCS-failed frames) or BIT(5) (control frames): with them, frames with
    // bit errors reached the Family and wifip2pd ("Failed to parse event[48]", a TLV 4 whose channel sequence was noise) and
    // the listen timer asked for channels 26, 249 and 253, which the firmware never granted before it hung (§10.51).
    if (!chained) {
        m3RxFilter(0x80000040u);
        m3SnifferEnable(true);                               // promiscuous mode before the radio leaves the AP's channel
    }
    ph(0);
    // A timed ROC, as Linux asks for a remain-on-channel (MT7925_ROC_REQ_ROC, maxinterval = the duration). Until §10.40 every
    // window asked for a 5-second JOIN-type ROC and aborted it after 20-100 ms, about 10 times a second; after ~3100 windows
    // (133 never granted) the firmware stopped answering MCU commands and the infrastructure link died with it.
    {
        uint64_t now = awdlUptimeUs();
        uint32_t ahead = (startUs && startUs > now) ? (uint32_t)((startUs - now) / 1000) : 0;
        // + awdlChainMs: room to keep this ROC for the next window on the same channel (aborted early when there is none)
        uint32_t dur = dwellMs + 30 + ahead + awdlChainMs;
        if (!onCh && !chained) { m3Roc(true, ch, 1, dur); rocEndUs = now + (uint64_t)(dur - 10) * 1000; }
    }
    ph(1);
    // The grant is waited for up to 300 ms, never abandoned after 80 (Linux waits up to 4 s and never aborts a ROC before its
    // grant). Until §10.42 an ungranted request was aborted after 80 ms and the next window asked again 20-60 ms later; the
    // firmware went silent after ~300 windows with 19 such aborts. A grant that comes after the window's budget is still
    // taken (so the abort that follows releases a ROC the firmware really holds), but nothing is sent in it.
    uint32_t rocT0 = nowMs(), rocMs = 0;
    for (int i = 0; !onCh && !chained && i < 300 && rocWant && !rocGrant; i++) m3Pump(1);   // m3Pump keeps servicing the AP meanwhile
    rocMs = nowMs() - rocT0;
    ph(2);
    bool granted = onCh || chained || rocGrant, bad = !onCh && !chained && rocStatus;
    bool late = !onCh && !chained && granted && rocMs >= 80;
    if (!onCh && !chained) awdlRocLat[!granted ? 4 : late ? 3 : rocMs < 10 ? 0 : rocMs < 40 ? 1 : 2]++;
    if (granted) { awdlWinBand[ch > 14]++; awdlWinSeq++; }
    else awdlNeverBand[ch > 14]++;
    if (granted && !bad && !late && !chained) m3SnifferConfig(ch);       // the sniffer follows the radio to the social channel
    ph(3);
    if (!granted || bad || late) {
        awdlTxRocFail++;
        if (awdlTxRocFail <= 8)
            m3log("awdl sync: roc fail ch=%u grant=%d status=%u after %u ms", ch, rocGrant ? 1 : 0, rocStatus, rocMs);
        if (!rocGrant) { uint32_t t = nowMs() + 2000; awdlBackoffUntilMs = t ? t : 1; }   // let the firmware settle
        m3SnifferEnable(false);
        m3Roc(false, ch);
        winEnd();
        awdlQuiet = 0;
        awdlRocActive = 0;
        return;
    }
    if (startUs) {                                           // granted and sniffer set: learn the setup time, then meet the AW
        uint64_t g = awdlUptimeUs();
        uint32_t setup = (uint32_t)(g - setupT0);
        uint32_t lead = (7 * awdlLeadUs + setup + 3000) / 8;
        if (!onCh && !chained) awdlLeadUs = lead < 4000 ? 4000 : lead > 45000 ? 45000 : lead;   // learned from ROC windows only
        if (g < startUs) {
            awdlLeadEarly++;
            while (awdlUptimeUs() + 1000 <= startUs) m3Pump(1);
            while (awdlUptimeUs() < startUs) IODelay(50);
        } else {
            awdlLeadLateN++;
            awdlLeadLateUs += g - startUs;
        }
        uint64_t end = startUs + (uint64_t)dwellMs * 1000, now = awdlUptimeUs();
        dwellMs = end > now + 1000 ? (uint32_t)((end - now) / 1000) : 1;   // the dwell is counted from the AW start
    }
    ph(4);
    // §10.61: the firmware needs ~25 ms to leave the AP's channel and ~15 ms to come back for every ROC (§10.59), while the
    // Family hands over listen windows back to back on the same channel. When the next window is already in the mailbox, on
    // this channel, starts within 8 ms and ends inside the granted ROC, the radio stays: sniffer, filter and ROC are kept,
    // and that window skips the whole setup. The absence is bounded by awdlChainMs on top of one dwell (the ROC's duration),
    // and winEnd() bills the AP-share rule for all of it.
    auto hold = [&]() -> bool {
        if (onCh || !awdlChainMs || !startUs || !rocEndUs) return false;
        uint64_t now = awdlUptimeUs(), s2 = 0;
        uint32_t d2 = 0;
        bool take = false;
        if (cmdLock) IOLockLock(cmdLock);
        if (__atomic_load_n(&awdlFrameSeq, __ATOMIC_ACQUIRE) != awdlFrameDone && awdlChannel == ch && awdlStartUs) {
            s2 = awdlStartUs;
            d2 = awdlDwellMs;
        }
        if (cmdLock) IOLockUnlock(cmdLock);
        if (s2 && s2 <= now + 8000 && s2 + (uint64_t)d2 * 1000 <= rocEndUs) take = true;
        if (take ? awdlHoldLog < 16 : awdlHoldLog < 4) {
            awdlHoldLog++;
            m3log("awdl hold: take=%d chained=%d ch=%u s2-now=%dus d2=%u rocend-now=%dus seq=%u done=%u", take ? 1 : 0,
                  chained ? 1 : 0, ch, s2 ? (int)(int64_t)(s2 - now) : 0, d2, (int)(int64_t)(rocEndUs - now),
                  __atomic_load_n(&awdlFrameSeq, __ATOMIC_ACQUIRE), awdlFrameDone);
        }
        if (!take) return false;
        awdlHeldCh = ch;
        awdlHeldOffT0 = offT0;
        awdlHeldUntilUs = s2 + 3000;
        awdlHeldRocEndUs = rocEndUs;
        return true;                                         // awdlQuiet / awdlRocActive stay set: the radio is still away
    };
    M3TxOpt o;
    o.qidx = 0x10;                                           // MT_LMAC_ALTX0 (management)
    o.rateAdapt = false;                                     // fixed rate: the sync frame must go out as OFDM 6 Mb/s
    o.protect = false;
    // Per-transmission fields of the AWDL action header the interface built (docs/ROADMAP-AWDL.md §10.11): seq_ctrl at +22
    // and the two microsecond timestamps at +32/+36 (phy_tx / target_tx, both stamped with the send time, as OWL does).
    auto stamp = [&](uint8_t *f, uint32_t l) {
        if (l < 40) return;
        uint32_t s = (awdlAfSeq++ & 0xfff) << 4;
        f[22] = (uint8_t)(s & 0xff);
        f[23] = (uint8_t)((s >> 8) & 0xff);
        uint32_t us = (uint32_t)awdlUptimeUs();
        put32(f + 32, us);
        put32(f + 36, us);
        if (awdlStampSync(f, l, tl)) awdlStamped++;
        else if (tl.periodUs) awdlStampMiss++;
    };
    if (listenOnly) {
        awdlDwell(dwellMs);
        ph(5);
        if (hold()) return;
        m3SnifferEnable(false);
        ph(6);
        if (!onCh) m3Roc(false, ch);
        ph(7);
        winEnd();
        ph(8);
        if (!onCh && !chained) awdlPhN++;
        awdlQuiet = 0;
        awdlRocActive = 0;
        return;
    }
    stamp(frame, len);
    // The frame goes out as soon as the radio is on the channel - the interface started this window at an AW boundary, so
    // this is the start of the availability window the TLV describes - and the rest of the dwell is spent receiving: a
    // peer's PSF/MIF arrives at any point of its window (docs/ROADMAP-AWDL.md §10.22, §10.28).
    bool ok = m3Tx(frame, len, 0, 13, o);                // type 0 (management), subtype 13 (action)
    if (len2) {                                              // the master indication frame, right behind the PSF
        stamp(frame2, len2);
        awdlTx2Try++;
        if (m3Tx(frame2, len2, 0, 13, o)) awdlTx2Ok++;
    }
    awdlDwell(dwellMs);                                      // the whole dwell: data out, frames in
    ph(5);
    bool held = hold();
    if (!held) {
        m3SnifferEnable(false);
        ph(6);
        if (!onCh) m3Roc(false, ch);
        ph(7);
        winEnd();
        ph(8);
        if (!onCh && !chained) awdlPhN++;
        awdlQuiet = 0;
        awdlRocActive = 0;
    }
    if (ok) awdlTxOk++;
    if (awdlTxTry <= 10)
        m3log("awdl sync: tx ch=%u len=%u/%u ok=%d stamped=%u miss=%u mif=%u/%u", ch, len, len2, ok ? 1 : 0, awdlStamped,
              awdlStampMiss, awdlTx2Ok, awdlTx2Try);
    if ((awdlTxTry % 50) == 0) {                              // never evicted from the ring: tells RX from TX failures apart
        m3log("awdl stat: listen=%u try=%u tx=%u rocfail=%u rocframes=%u onch=%u bcn=%u af=%u ch=%u fc=%u/%u/%u/%u/%u",
              awdlListenWin, awdlTxTry, awdlTxOk, awdlTxRocFail, awdlRocFrames, awdlRocOnCh, awdlBcn, awdlAfSeen, awdlRocCh,
              awdlRxFc[0], awdlRxFc[1], awdlRxFc[2], awdlRxFc[3], awdlRxFc[4]);
        m3log("awdl roc latency <10=%u <40=%u <80=%u late=%u never=%u", awdlRocLat[0], awdlRocLat[1], awdlRocLat[2],
              awdlRocLat[3], awdlRocLat[4]);
        m3log("awdl band: af=%u/%u win=%u/%u never=%u/%u dfs=%u (2.4/5 GHz)", awdlAfBand[0], awdlAfBand[1], awdlWinBand[0],
              awdlWinBand[1], awdlNeverBand[0], awdlNeverBand[1], awdlDfsSkip);
        m3log("awdl lead: lead=%uus early=%u late=%u avglate=%uus stale=%u wait=%ums on=%u onch=%u apch=%u", awdlLeadUs,
              awdlLeadEarly, awdlLeadLateN, awdlLeadLateN ? (uint32_t)(awdlLeadLateUs / awdlLeadLateN) : 0, awdlLeadStale,
              awdlLeadWaitMs, awdlLeadOn, awdlOnChWin, tgtCh);
        m3log("awdl chain: held=%u break=%u timeout=%u chainms=%u", awdlChainN, awdlChainBreak, awdlChainTimeout, awdlChainMs);
        if (awdlPhN) {
            uint32_t a[9];
            for (int i = 0; i < 9; i++) a[i] = (uint32_t)(awdlPhUs[i] / awdlPhN / 100);   // 0.1 ms units
            m3log("awdl phase (0.1ms avg over %u): setup=%u roc=%u grant=%u scfg=%u wait=%u dwell=%u soff=%u abort=%u end=%u",
                  awdlPhN, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
        }
        m3log("awdl duty: off=%u ms in %u windows (avg %u) skip=%u ps=%u/%u apbcn=%u", awdlOffMs, awdlOffN,
              awdlOffN ? awdlOffMs / awdlOffN : 0, awdlDutySkip, awdlPsTx[1], awdlPsTx[0], tgtBcnCount);
        m3log("awdl rxstat: oui=%u sig=%u bss=%u nm=%u fx=%u stamp=%u/%u mif=%u/%u full=%u data rx=%u hdr=%u/%u tx=%u/%u/%u/%u stale=%u",
              awdlOuiHits, awdlSigHits, awdlBssidHits, awdlNearMiss, awdlForensics, awdlStamped, awdlStampMiss, awdlTx2Ok,
              awdlTx2Try, awdlRxRingFull, awdlDataRx, awdlDataRxHdr[0], awdlDataRxHdr[1], awdlDataTxQueued, awdlDataTxSent,
              awdlDataTxFail, awdlDataTxFull, awdlDataTxStale);
        uint32_t oo = 0, of = 0;
        for (int i = 0; i < 8; i++) if (i < 3 || i == 7) { oo += txsOk[i]; of += txsFail[i]; }
        m3log("awdl txs: pk=%u n=%u host=%u/%u uc=%u/%u ucheard=%u/%u grp=%u/%u other=%u/%u err q/rts/ack=%u/%u/%u late=%u "
              "uc24=%u/%u uc5=%u/%u guard=%u drain=%u/%ums rxuc=%u mine=%u own=%d omac1=%u",
              txsPkts, txsN, txsOk[3], txsFail[3], txsOk[4], txsFail[4], txsOk[6], txsFail[6], txsOk[5], txsFail[5], oo, of,
              txsErr[0], txsErr[1], txsErr[2], txsLate, txsUcBand[0][0], txsUcBand[0][1], txsUcBand[1][0], txsUcBand[1][1],
              awdlTxGuardHeld, awdlDrainTimeout, awdlDrainWaitMs, awdlDataRxUc, awdlDataRxUcMine, awdlOwnRc, awdlOmac1);
        m3log("awdl txfree: uc=%u/%u ucheard=%u/%u grp=%u/%u tries=%u late=%u uc24=%u/%u uc5=%u/%u pool=%u (ok/fail)",
              tfOk[0], tfFail[0], tfOk[2], tfFail[2], tfOk[1], tfFail[1], tfTries, tfLate, tfBand[0][0], tfBand[0][1],
              tfBand[1][0], tfBand[1][1], txPool ? 1 : 0);
    }
}

// PKT_TYPE_TXS. On this chip the packet carries a 4-dword header (RXD0 type | length, then three dwords seen as 0) and then
// 8-dword records: TXS0 bit 15 TX_STATUS_HOST, bits 18:16 ACK_ERROR_MASK (queue timeout, RTS timeout, ACK timeout), 13:0 the
// rate; TXS1 31:20 the 802.11 sequence number; TXS2 27:16 the WCID; TXS3 31:24 the PID from TXD5. Read from hardware records
// (round 22 logged them two dwords off: header zeros, then wcid 1 in the third dword and SN 0x40.. in the second).
void itlmt7925::m3Txs(const uint8_t *p, uint32_t len)
{
    uint32_t n = le32(p) & 0xffff;                          // RXD0 RX_BYTE_CNT: the packet's own length, not the buffer's
    if (n >= 16 && n < len) len = n;
    txsPkts++;
    if (txsPkts <= 4) {
        m3log("txs pkt len=%u %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x %08x %08x", len, le32(p), le32(p + 4),
              le32(p + 8), le32(p + 12), len >= 20 ? le32(p + 16) : 0, len >= 24 ? le32(p + 20) : 0, len >= 28 ? le32(p + 24) : 0,
              len >= 32 ? le32(p + 28) : 0, len >= 36 ? le32(p + 32) : 0, len >= 40 ? le32(p + 36) : 0,
              len >= 44 ? le32(p + 40) : 0, len >= 48 ? le32(p + 44) : 0);
    }
    for (uint32_t o = 16; o + 32 <= len; o += 32) {
        uint32_t t0 = le32(p + o), t1 = le32(p + o + 4), t2 = le32(p + o + 8), t3 = le32(p + o + 12);
        if (!(t0 | t1 | t2 | t3)) continue;                 // padding
        uint32_t pid = (t3 >> 24) & 7, err = (t0 >> 16) & 7;
        bool awdl = pid >= 4 && pid <= 6;
        txsN++;
        if (err) {
            txsFail[pid]++;
            for (int b = 0; b < 3; b++) if (err & (1u << b)) txsErr[b]++;
        } else txsOk[pid]++;
        if (awdl) {
            if (!awdlRocActive) txsLate++;
            if (pid != 5) txsUcBand[awdlRocCh > 14][err ? 1 : 0]++;
        }
        if (awdl && pid != 5 && (txsLogged < 6 || (err && txsLogged < 12))) {
            txsLogged++;
            m3log("txs awdl uc pid=%u err=%u wcid=%u sn=%u rate=%04x ch=%u win=%u %08x %08x %08x %08x %08x %08x %08x %08x", pid,
                  err, (t2 >> 16) & 0xfff, t1 >> 20, t0 & 0x3fff, awdlRocCh, awdlRocActive, t0, t1, t2, t3, le32(p + o + 16),
                  le32(p + o + 20), le32(p + o + 24), le32(p + o + 28));
        }
    }
}

// The firmware sniffer (MCU_UNI_CMD(SNIFFER), UNI_SNIFFER_ENABLE/UNI_SNIFFER_CONFIG, mt7925_mcu_set_sniffer /
// mt7925_mcu_config_sniffer). This is the mechanism the one public MT7925 AWDL implementation uses to see AWDL at all: in
// station mode the chip hands the host its own BSS (plus beacons), so an action frame from another device never arrives,
// however the MAC80211 RX filter is programmed. Sniffer mode is therefore turned on only for the duration of the ROC window
// - frames arrive while it is on, and the station keeps the AP link because the radio is already parked off-channel.
void itlmt7925::m3Sniffer(bool on, uint8_t ch)
{
    m3SnifferEnable(on);
    if (on) m3SnifferConfig(ch);
}

// The promiscuous mode itself (UNI_SNIFFER_ENABLE).
void itlmt7925::m3SnifferEnable(bool on)
{
    M3Msg m;
    m.init(4);                                               // band_idx + rsv[3]
    m.b[0] = 0;
    uint8_t *t = m.tlv(0, 8);                                // UNI_SNIFFER_ENABLE { enable, pad[3] }
    t[4] = on ? 1 : 0;
    m3Cmd(0x24, m.b, m.n, on ? "sniffer on" : "sniffer off");
}

// The sniffer's channel configuration (UNI_SNIFFER_CONFIG). It carries the channel the promiscuous mode listens on, so it has
// to be re-sent *after* the radio has been parked: the reference implementation updates it when the channel context changes
// (mt7925_change_chanctx -> mt7925_mcu_config_sniffer), not before (docs/ROADMAP-AWDL.md §10.22).
void itlmt7925::m3SnifferConfig(uint8_t ch)
{
    // On the AP's own channel the sniffer takes the link's real width (mt7925_mcu_config_sniffer fills bw / sco / center from
    // the channel definition): pinning the radio to 20 MHz there would cut the 80 MHz link for the whole dwell (§10.58).
    bool apWidth = m3linkUp && ch == tgtCh && fastActive && fastBw && fastCenter;
    M3Msg c;
    c.init(4);
    c.b[0] = 0;
    uint8_t *u = c.tlv(1, 16);                               // UNI_SNIFFER_CONFIG
    uint8_t *v = u + 4;                                      // aid, band, bw, control, sco, center, center2, drop_err
    put16(v + 0, 0);                                         // aid: not a station context
    v[2] = ch > 14 ? 2 : 1;                                  // band: 1 = 2.4 GHz, 2 = 5 GHz
    v[3] = apWidth ? fastBw : 0;                             // CMD_CBW_*: 20 MHz off the AP channel
    v[4] = ch;                                               // control channel
    v[5] = apWidth ? (ch < fastCenter ? 1 : ch > fastCenter ? 3 : 0) : 0;   // sco: secondary above / below (as bss_rlm)
    v[6] = apWidth ? fastCenter : ch;                        // center channel
    v[7] = 0;                                                // center channel 2
    v[8] = 1;                                                // drop_err: drop FCS-failed frames (they carried noise as TLVs, §10.51)
    m3Cmd(0x24, c.b, c.n, "sniffer cfg");
}

// BAND_CONFIG / UNI_BAND_CONFIG_SET_MAC80211_RX_FILTER (mt7925_mcu_set_rxfilter). The station's RX filter defaults to "only
// my own BSS", so frames from other devices never reached the host at all - which is why no AWDL action frame was ever seen,
// on the infrastructure channel or on the social one. BIT(31) enables filtering, BIT(6) lets other-BSS frames through
// (MT7925_FILTER_ENABLE / MT7925_FILTER_OTHER_BSS in the vendor driver); mode 0 replaces the whole filter.
void itlmt7925::m3RxFilter(uint32_t fif)
{
    M3Msg m;
    m.init(4);                                               // band_idx + rsv[3]
    m.b[0] = 0;
    uint8_t *t = m.tlv(0x0c, 68);                            // UNI_BAND_CONFIG_SET_MAC80211_RX_FILTER
    uint8_t *v = t + 4;                                      // mode, rsv[3], fif, bit_map, bit_op, pad[51]
    v[0] = 0;
    put32(v + 4, fif);
    put32(v + 8, 0);
    v[12] = 0;
    m3Cmd(0x08, m.b, m.n, "rx_filter");
}

// Pops one AWDL action frame the RX path queued (docs/ROADMAP-AWDL.md phase 3, receive half). Returns 1 when a frame was
// copied out, 0 when the queue is empty; the caller is the AWDL interface's work loop, the producer is the RX thread.
int itlmt7925::awdlRxFetch(uint8_t *out, uint32_t cap, uint32_t *outLen, uint32_t *outCh, int8_t *outRssi, uint32_t *outUs)
{
    if (!out || !outLen) return 0;
    uint32_t tail = awdlRxTail;
    if (tail == __atomic_load_n(&awdlRxHead, __ATOMIC_ACQUIRE)) return 0;
    const AwdlRxSlot &s = awdlRxRing[tail % kAwdlRxSlots];
    uint32_t n = s.len; if (n > cap) n = cap;
    memcpy(out, s.data, n);
    *outLen = n;
    if (outCh) *outCh = s.ch;                          // the social channel the frame was heard on
    if (outRssi) *outRssi = s.rssi;                    // from the RX vector, 0 when it could not be decoded
    if (outUs) *outUs = s.tUs;
    __atomic_store_n(&awdlRxTail, tail + 1, __ATOMIC_RELEASE);
    return 1;
}

// AWDL data TX ring (docs/ROADMAP-AWDL.md §10.33). Producer: the AWDL interface (any thread, under cmdLock); consumer: the
// link thread inside an AWDL window (awdlDataFlush).
int itlmt7925::awdlDataQueue(const uint8_t *frame, uint32_t len)
{
    if (!frame || len < 24 || len > sizeof(awdlDataTxRing[0].data)) return ITL_EINVAL;
    if (cmdLock) IOLockLock(cmdLock);
    uint32_t head = awdlDataTxHead;
    int rc = 0;
    if (head - awdlDataTxTail >= kAwdlTxSlots) {
        // Full. If radio windows are running this is backpressure and the frame waits in the stack; if none ran for a second
        // (AWDL enabled but no window yet, or windows stopped) nothing will drain the ring, so the frame is stale and dropped.
        uint32_t last = awdlLastWinMs;
        if (!last || nowMs() - last > 1000) { awdlDataTxStale++; rc = ITL_ESTALE; }
        else { awdlDataTxFull++; rc = ITL_EIO; }
    }
    else {
        AwdlTxSlot &s = awdlDataTxRing[head % kAwdlTxSlots];
        memcpy(s.data, frame, len);
        s.len = len;
        awdlDataTxHead = head + 1;
        awdlDataTxQueued++;
    }
    if (cmdLock) IOLockUnlock(cmdLock);
    return rc;
}

// The dwell of one AWDL window (docs/ROADMAP-AWDL.md §10.54). Data is handed to the hardware only while at least
// kGuardMs of the window are left: a frame posted at the very end was still queued or retrying (a 1500-byte frame at
// 6 Mb/s is ~2 ms per attempt, up to 15 attempts) when m3Roc(false) moved the radio back to the AP's channel, so it went
// out where the peer is not. Then, inside the 30 ms the ROC was asked for beyond the dwell, wait until the hardware has
// reported every AWDL data frame (TXS) before the window is closed.
void itlmt7925::awdlDwell(uint32_t dwellMs)
{
    static const uint32_t kGuardMs = 10, kDrainMs = 25;
    uint32_t t0 = nowMs();
    while (nowMs() - t0 < dwellMs) {
        if (nowMs() - t0 + kGuardMs < dwellMs) awdlDataFlush();
        else if (awdlDataTxTail != awdlDataTxHead) awdlTxGuardHeld++;
        m3Pump(1);
    }
    uint32_t t1 = nowMs();
    while (txPool && awdlDataInFlight && nowMs() - t1 < kDrainMs) m3Pump(1);   // released by TXFREE (m3TxFree)
    awdlDrainWaitMs += nowMs() - t1;
    if (awdlDataInFlight) { awdlDrainTimeout++; awdlDataInFlight = 0; }   // TXS lost or not generated: do not carry it over
}

void itlmt7925::awdlPeerHeard(const uint8_t *sa)
{
    int slot = 0;
    for (int i = 0; i < 4; i++) {
        if (memcmp(awdlPeers[i].mac, sa, 6) == 0) { slot = i; break; }
        if (awdlPeers[i].win < awdlPeers[slot].win) slot = i;   // else the stalest entry
    }
    memcpy(awdlPeers[slot].mac, sa, 6);
    awdlPeers[slot].ch = awdlRocCh;
    awdlPeers[slot].win = awdlWinSeq;
}

bool itlmt7925::awdlPeerInWindow(const uint8_t *da) const
{
    for (int i = 0; i < 4; i++)
        if (memcmp(awdlPeers[i].mac, da, 6) == 0) return awdlPeers[i].win == awdlWinSeq && awdlRocActive;
    return false;
}

// Sends every queued AWDL data frame. Called only while an AWDL ROC window is open (the radio is on the peer's channel).
void itlmt7925::awdlDataFlush()
{
    for (int n = 0; n < (int)kAwdlTxSlots; n++) {
        if (cmdLock) IOLockLock(cmdLock);
        uint32_t tail = awdlDataTxTail;
        bool have = tail != awdlDataTxHead;
        uint32_t len = 0;
        if (have) {
            AwdlTxSlot &s = awdlDataTxRing[tail % kAwdlTxSlots];
            len = s.len;
            memcpy(awdlTxBuf2, s.data, len);                 // awdlTxBuf2 is free: the MIF has already gone out
            awdlDataTxTail = tail + 1;
        }
        if (cmdLock) IOLockUnlock(cmdLock);
        if (!have) return;
        M3TxOpt o;
        o.qidx = 0x10;                                       // ALTX0, as the PSF/MIF: no BA, no station record for the peer
        o.rateAdapt = false;
        o.protect = false;
        // TXS tells unicast (ACKed or not) from group frames, and unicast to a peer heard in this very window from the rest
        o.pid = (awdlTxBuf2[4] & 1) ? 5 : awdlPeerInWindow(awdlTxBuf2 + 4) ? 6 : 4;
        // From the AWDL address: OWN_MAC 1 is the OMAC awdlOwnMacApply() programmed with it (mt7925_mac_write_txwi uses the
        // interface's omac_idx), so the MAC matches the peer's ACK to that address. OMAC 0 is the station's.
        o.omac = (awdlOmac1 && awdlOwnSent && awdlOwnRc == 0) ? 1 : 0;
        if (m3Tx(awdlTxBuf2, len, 2, 0, o)) { awdlDataTxSent++; if (txPool) awdlDataInFlight++; }   // type 2 (data), subtype 0
        else awdlDataTxFail++;
    }
}

// ---------------------------------------------------------------------------------------------
// Management frame TX / RX (mt7925e_tx_prepare_skb + mt7925_mac_write_txwi, CT path with a token in the hw txp)
// ---------------------------------------------------------------------------------------------
static inline void membar() { __sync_synchronize(); }
#define M3_RATE_IDX 18                                           // fixed-rate table: OFDM 6 Mb/s (see m3HwPrep)

// TXD (32) + hw txp (32) in the ring buffer, the 802.11 frame 128 bytes further; the ring entry covers TXD + txp only.
struct M3Lock {
    IOLock *l;
    explicit M3Lock(IOLock *x) : l(x) { if (l) IOLockLock(l); }
    ~M3Lock() { if (l) IOLockUnlock(l); }
};

bool itlmt7925::m3Tx(const uint8_t *frame, uint32_t len, uint8_t type, uint8_t subtype, const M3TxOpt &o)
{
    if (!txData.n || len > 1900 || len < 24) return false;
    M3Lock lk(txLock);                                           // management frames from the m3 thread, data from the network stack
    uint32_t slot;
    uint16_t tok;
    if (txPool) {
        // staging buffer owned until the firmware reports it free (TXFREE); no slot or no descriptor: the Ethernet output queue
        // keeps the frame (o.full), anything else is dropped
        bool got = txSlotAlloc(&slot, &tok);
        if (got && (txData.head + 1) % txData.n == (int)(rd(txData.regs + Q_DMA_IDX) & 0xffff)) {
            txClrBusy(slot);
            got = false;
        }
        if (!got) {
            stTxNoSlot++;
            if (o.full) { *o.full = true; ethStalled = true; ethStallMs = nowMs(); }   // m3TxFree() restarts the queue
            return false;
        }
    } else {
        slot = (uint32_t)(m3txSlot++ & 63);
        m3tok = (m3tok + 1) & 0x1fff;
        tok = m3tok;
    }
    if (txPool) txPid[slot] = o.rateAdapt ? 0 : o.pid;
    size_t base = (size_t)slot * 2048;
    uint8_t *w = txData.buf.va + base;
    uint64_t pa = txData.buf.pa + base;
    bzero(w, 128);
    memcpy(w + 128, frame, len);

    // Group-addressed data: default goes out on the AP wcid (1) - the path that has worked on hardware, where the
    // hardware evidently picks the BSS group key for the frame anyway. itl_grp19=1 switches to mt76's choice (the BSS
    // wcid 19, mvif->sta.deflink.wcid, the wcid the group key is installed on) together with its STA_REC records.
    // MT_TXD3_BCM stays clear either way: mt7925_mac_write_txwi_80211 raises it only when addr1 (the RA) is group
    // addressed, which never happens for these ToDS frames (RA = AP BSSID, DA = addr3).
    bool grp = type == 2 && len >= 18 && (frame[16] & 1);
    uint16_t wcid = (grp && grpWcid19) ? M3_BMC_WCID : M3_AP_WCID;
    put32(w + 0, (len + 32) | (0u << 23) | ((uint32_t)o.qidx << 25));         // TX_BYTES | PKT_FMT=CT | Q_IDX
    uint32_t hdrLen = (type == 2 && (frame[0] & 0x80)) ? 26 : 24;               // QoS data frames carry the 2-byte QoS control field
    // wlan idx, HDR_FORMAT=802.11, HDR_INFO, TID; FIXED_RATE unless the firmware's rate control (STA_REC_RA) picks the rate
    // MT7927: TGID (bits 13:12) = band_idx, as mt7925_mac_write_txwi() does ("0xff means BAND_AUTO ... TGID cannot encode it").
    // Without it the frame is queued for band 0 (2.4 GHz) while the ROC / BSS hold band 1 (5 GHz): it never goes on air.
    // AWDL (MT7927 only): during an AWDL window the radio sits on the ROC's channel, which can be on the other band than the
    // AP (ch 6 vs a 5 GHz AP): queue on the band the firmware granted for that ROC (rocBand), not the station's vifBand.
    uint8_t txBand = (is7927 && awdlRocActive && rocBand != 0xff) ? rocBand : vifBand;
    uint32_t tgid = (txBand && txBand != 0xff) ? ((uint32_t)(txBand & 3) << 12) : 0;
    put32(w + 4, wcid | tgid | (2u << 14) | ((hdrLen / 2) << 16) | ((uint32_t)(o.tid & 0xf) << 21) | ((uint32_t)(o.omac & 0x3f) << 25) |
                     (o.rateAdapt ? 0u : (1u << 31)));
    put32(w + 8, ((uint32_t)type << 4) | subtype);
    // REM_TX_COUNT=15, BA_DISABLE unless this frame may be aggregated, PROTECT_FRAME, SN_VALID + SEQ for a host-chosen sequence number
    put32(w + 12, (15u << 11) | (o.aggr ? 0u : (1u << 28)) | (o.protect ? 2u : 0) |
                      (o.sn >= 0 ? ((1u << 31) | ((uint32_t)(o.sn & 0xfff) << 16)) : 0u));
    if (grp && stGrpTx < 6) { stGrpTx++; m3log("tx grp len=%u wc=%u", len, wcid); }
    put32(w + 20, o.rateAdapt ? 0u : (o.pid | (1u << 10)));      // PID (3 = MT_PACKET_ID_FIRST), TX_STATUS_HOST: report completion (none for rate-adapted data)
    put32(w + 24, (1u << 2) | (1u << 4) | (1u << 3) | (o.rateAdapt ? 0u : ((uint32_t)M3_RATE_IDX << 16)));   // DAS, MSDU_CNT=1, DIS_MAT, TX_RATE

    put16(w + 32, (uint16_t)(tok | 0x8000));                     // txp.msdu_id[0] | VALID
    put32(w + 40, (uint32_t)(pa + 128));                         // txp.ptr[0].buf0
    put16(w + 44, (uint16_t)(len | 0x8000));                     // len0 | LAST
    membar();

    uint8_t *d = txData.desc.va + txData.head * 16;
    put32(d + 0, (uint32_t)pa);
    put32(d + 8, 0);
    put32(d + 12, 0);
    put32(d + 4, MT_DMA_CTL_SD_LEN0(64) | MT_DMA_CTL_LAST_SEC0);
    txData.head = (txData.head + 1) % txData.n;
    membar();
    wr(txData.regs + Q_CPU_IDX, (uint32_t)txData.head);
    if (txPool) return true;
    for (int us = 0; us < 200000; us += 20) {
        if ((int)(rd(txData.regs + Q_DMA_IDX) & 0xffff) == txData.head) return true;
        IODelay(20);
    }
    m3log("tx stall cpu=%u dma=%u", txData.head, rd(txData.regs + Q_DMA_IDX) & 0xffff);
    return false;
}

// Looks for a management frame (header after the RXD groups) from the AP to us whose fc byte 0 is m3wantFc.
bool itlmt7925::m3RxMgmt(const uint8_t *p, uint32_t len)
{
    uint32_t pt = len >= 8 ? le32(p) >> 27 : 0;
    if ((pt == 0 || pt == 6) && len >= 24 && m3rxOther < 8) {      // TXS / TXRX_NOTIFY: did the firmware transmit our frame?
        m3rxOther++;
        m3log("rx pt=%u len=%u %08x %08x %08x %08x %08x %08x", pt, len, le32(p), le32(p + 4), le32(p + 8), le32(p + 12),
              le32(p + 16), le32(p + 20));
    }
    if (len < 32 + 24 || m3rxLen) return false;
    for (uint32_t off = 32; off + 24 <= len && off < 160; off++) {
        if (p[off] != m3wantFc) continue;
        if (memcmp(p + off + 4, mac, 6) != 0 || memcmp(p + off + 10, tgtBssid, 6) != 0) continue;
        uint32_t n = len - off;
        if (n > sizeof(m3rx)) n = sizeof(m3rx);
        memcpy(m3rx, p + off, n);
        m3rxLen = n;
        return true;
    }
    // Anything else coming from the AP while we wait (not beacons / probe responses): log a few for diagnosis
    static int dumped = 0;
    for (uint32_t off = 32; dumped < 6 && off + 24 <= len && off < 160; off++) {
        if (memcmp(p + off + 10, tgtBssid, 6) != 0 || p[off] == 0x80 || p[off] == 0x50 || p[off + 1] > 0x7f) continue;
        dumped++;
        m3log("other frame off=%u fc=%02x%02x a1=%02x%02x%02x%02x%02x%02x len=%u", off, p[off], p[off + 1], p[off + 4],
              p[off + 5], p[off + 6], p[off + 7], p[off + 8], p[off + 9], len);
        break;
    }
    return false;
}

// Sends `frame` (up to 4 times, 150 ms apart) until a frame with fc byte 0 == wantFc comes back.
bool itlmt7925::m3Xchg(const uint8_t *frame, uint32_t len, uint8_t subtype, uint8_t wantFc, uint32_t waitMs)
{
    m3rxLen = 0;
    m3wantFc = wantFc;
    bool got = false;
    for (int attempt = 1; attempt <= 4 && !got; attempt++) {
        bool ok = m3Tx(frame, len, 0, subtype);
        m3log("tx %02x len=%u #%d %s", frame[0], len, attempt, ok ? "queued" : "FAILED");
        for (uint32_t i = 0; i < waitMs && !m3rxLen; i++) m3Pump(1);
        got = m3rxLen != 0;
    }
    m3wantFc = 0;
    if (!got) m3log("no response fc=%02x rxpk=%u txcpu=%u txdma=%u", wantFc, rxPkts, txData.head, rd(txData.regs + Q_DMA_IDX) & 0xffff);
    return got;
}

static void m3Hdr(uint8_t *h, uint8_t fc0, const uint8_t *bssid, const uint8_t *sa)
{
    bzero(h, 24);
    h[0] = fc0;
    memcpy(h + 4, bssid, 6);
    memcpy(h + 10, sa, 6);
    memcpy(h + 16, bssid, 6);
}

int itlmt7925::m3Auth()
{
    uint8_t f[30];
    m3Hdr(f, 0xb0, tgtBssid, mac);
    put16(f + 24, 0);                                            // open system
    put16(f + 26, 1);                                            // transaction 1
    put16(f + 28, 0);
    if (!m3Xchg(f, sizeof(f), 11, 0xb0, 150)) return ITL_TIMEOUT;
    uint16_t alg = (uint16_t)(m3rx[24] | (m3rx[25] << 8)), seq = (uint16_t)(m3rx[26] | (m3rx[27] << 8)),
             st = (uint16_t)(m3rx[28] | (m3rx[29] << 8));
    m3log("auth resp alg=%u seq=%u status=%u", alg, seq, st);
    return (alg == 0 && seq == 2 && st == 0) ? 0 : ITL_EIO;
}

int itlmt7925::m3Assoc()
{
    uint8_t f[320];
    m3Hdr(f, 0x00, tgtBssid, mac);
    uint8_t *p = f + 24;
    // 2.4 GHz: advertise the CCK rates the AP's basic set usually contains also on the legacy path (an AP whose basic rates are
    // 1/2/5.5/11 refuses a station that offers OFDM only)
    bool g2 = tgtCh <= 14;
    put16(p, g2 ? 0x0431 : 0x0411); p += 2;                     // ESS | PRIVACY | short slot (| short preamble on 2.4 GHz)
    put16(p, 10); p += 2;                                        // listen interval
    uint8_t sl = (uint8_t)strlen(m3ssid);
    *p++ = 0; *p++ = sl; memcpy(p, m3ssid, sl); p += sl;
    static const uint8_t rates[8] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
    static const uint8_t rates2g[8] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};     // 1, 2, 5.5, 11 (CCK), 6, 9, 12, 18
    static const uint8_t ext2g[4] = {0x30, 0x48, 0x60, 0x6c};                               // 24, 36, 48, 54
    *p++ = 1; *p++ = 8; memcpy(p, g2 ? rates2g : rates, 8); p += 8;
    if (g2) { *p++ = 50; *p++ = 4; memcpy(p, ext2g, 4); p += 4; }
    m3RsnIe(p, apGroupCipher, apPairCipher);
    p += 22;
    if (fastActive) {
        p += itl_caps_build_ht(p);
        if (apCaps.vht) p += itl_caps_build_vht(p, fastAllow160);
        if (fastHe) p += itl_caps_build_he(p, !g2, fastAllow160);
        static const uint8_t wmm[9] = {0xdd, 0x07, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00};   // WMM information element: we are QoS-capable
        memcpy(p, wmm, sizeof(wmm)); p += sizeof(wmm);
    }
    if (!m3Xchg(f, (uint32_t)(p - f), 0, 0x10, 150)) return ITL_TIMEOUT;
    uint16_t st = (uint16_t)(m3rx[26] | (m3rx[27] << 8)), aid = (uint16_t)((m3rx[28] | (m3rx[29] << 8)) & 0x3fff);
    m3log("assoc resp status=%u aid=%u", st, aid);
    if (st) return ITL_EIO;
    m3aid = aid;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// M3 sequence. Each step is added behind its own log line so a partial run is still informative.
// ---------------------------------------------------------------------------------------------
int itlmt7925::m3Run1()
{
    m3log("armed");
    m3stepDown = m3stepRadio = false;                        // per-attempt state (an earlier attempt may have failed half way)
    m3reachedAssoc = false;
    m3cipher = 0;
    m3dataOn = false;
    m3linkUp = false;
    m3wantEapol = false;
    m3eapolLen = 0;
    m3eapolHead = m3eapolTail = m3eapolDrops = 0;
    m3rxLen = 0;
    fastActive = false;
    txPool = false;

    // 0. credentials: the candidates m3Supervise() put in nets[] (a joined network, or itl_ssid / itl_psk .. itl_ssid4 / itl_psk4
    // from the personality in that order)
    Net *nets = candNets;
    int nn = candN;
    if (!nn) {
        m3log("no usable itl_ssid/itl_psk in the personality");
        m3Publish("nocred");
        return ITL_EINVAL;
    }
    m3log("credentials: %d network(s)", nn);
    for (int i = 0; i < nn; i++)
        m3log("net%d ssid len=%u psk len=%u", i + 1, (unsigned)strlen(nets[i].ssid), (unsigned)strlen(nets[i].psk));

    // The scan already ran (M2). m3Capture only keeps beacons of the SSID in m3ssid, so look for each configured network in turn
    // (directed probe request) until one is seen; a network that the wildcard scan already listed goes first.
    int order[4], no = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nn; i++) {
            bool seen = false;
            for (int j = 0; j < nAps; j++) if (!strcmp(aps[j].ssid, nets[i].ssid)) seen = true;
            if (seen == (pass == 0)) order[no++] = i;
        }
    int sel = -1;
    for (int attempt = 1; attempt <= 3 && sel < 0; attempt++) {
        for (int k = 0; k < no && sel < 0 && !cmdPending(CMD_JOIN | CMD_DISCONNECT); k++) {
            int i = order[k];
            strlcpy(m3ssid, nets[i].ssid, sizeof(m3ssid));
            tgtFound = false;
            m3log("scan #%d for net%d (directed)", attempt, i + 1);
            m2Scan(m3ssid);
            for (int w = 0; w < 5 && !tgtFound; w++) { rxDataPoll(); IOSleep(100); }
            if (tgtFound) sel = i;
        }
    }
    m3Publish("scan");
    if (sel < 0) { m3log("no configured network seen in the scan"); return ITL_EIO; }
    strlcpy(m3ssid, nets[sel].ssid, sizeof(m3ssid));
    strlcpy(m3psk, nets[sel].psk, sizeof(m3psk));
    // candNets stays: the fall-back ladder in m3Run() calls this again for the next level; m3Supervise() wipes it afterwards
    m3log("selected net%d ssid len=%u", sel + 1, (unsigned)strlen(m3ssid));
    m3log("target %02x:%02x:%02x:%02x:%02x:%02x ch=%u cap=%04x bi=%u ies=%u", tgtBssid[0], tgtBssid[1], tgtBssid[2],
          tgtBssid[3], tgtBssid[4], tgtBssid[5], tgtCh, tgtCap, tgtBcnInt, tgtIeLen);

    // A level given up on 2.4 GHz says nothing about 5 GHz (and the other way round): another band starts from the top again.
    if (m3fast && capBand && capBand != (tgtCh <= 14 ? 1 : 2)) {
        evLog("band changed (cap learned on %s): level %u -> %u", capBand == 1 ? "2.4 GHz" : "5 GHz", lvl, fastLevel);
        lvlCap = 255;
        capBand = 0;
        lvl = fastLevel;
    }

    // PMK now (4096 PBKDF2 iterations), kept for the 4-way handshake; native Wi-Fi hands over the key its supplicant derived
    uint8_t pmk[32];
    if (nets[sel].hasPmk) memcpy(pmk, nets[sel].pmk, sizeof(pmk));
    else itl_wpa_pmk(m3psk, (const uint8_t *)m3ssid, strlen(m3ssid), pmk);
    memcpy(m3pmk, pmk, sizeof(m3pmk));
    bzero(pmk, sizeof(pmk));
    m3log("pmk ready");
    m3Publish("pmk");

    // step 2: firmware / hardware prep, BSS + STA records in the "not associated" state, ROC on the AP's channel
    m3ParseAp();
    int prepRc = m3HwPrep();
    if (prepRc) { m3Publish("hw-prep-fail"); return prepRc; }
    reconfigBand(tgtCh);                                     // MT7927: interface onto the AP's band first (no-op on MT7925)
    m3Bss(M3_BSS_ADD);
    m3Sta(0, true, 0);
    m3Roc(true);
    for (int i = 0; i < 1000 && !rocGrant; i++) m3Pump(1);
    m3log("roc grant=%d status=%u", rocGrant ? 1 : 0, rocStatus);
    if (!rocGrant || rocStatus != 0) { m3Publish("roc-fail"); m3Roc(false); return ITL_EIO; }

    // step 3: open-system authentication and association, both on the channel held by the ROC
    m3log("sta mac %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    int rc = m3Auth();
    m3Publish(rc ? "auth-fail" : "auth-ok");
    if (!rc) {
        m3reachedAssoc = true;
        rc = m3Assoc();
        m3Publish(rc ? "assoc-fail" : "assoc-ok");
    }

    // step 4: BSS up, station state "associated", 4-way handshake with key install, Ethernet interface
    if (!rc) {
        m3Bss(M3_BSS_ASSOC);
        m3Sta(2, false, m3aid);
        if (grpWcid19) m3StaBss();                           // mt76 parity: wcid 19 in the firmware table before any group TX
        rc = m3Handshake();
        m3wantEapol = false;
        m3Publish(rc ? "hs-fail" : "hs-ok");
    }
    m3Roc(false);
    bzero(m3pmk, sizeof(m3pmk));
    if (!rc) {
        m3log("RESULT rc=0 link up");
        stGrpTx = 0;                                         // per-session: log the first few group TX frames again
        stEcho = 0;
        m3Link();                                            // returns only when this level has to be given up (m3stepDown)
        return ITL_EIO;
    }
    if (fastActive && lvl >= 1 && m3reachedAssoc) {         // association / handshake failed with HT/VHT: try one level lower
        m3stepDown = true;
        baLog("lvl %u: assoc/handshake rc=%d", lvl, rc);
        evLog("lvl %u: association/handshake failed rc=%d", lvl, rc);
    }
    return rc;
}

// The connection sequence with a fallback ladder: itlmt_fast=<level> is tried first; when it does not work (association or
// handshake rejected, or the link comes up but nothing unicast ever comes back) the driver leaves the AP and connects again
// one level lower, down to the legacy 6 Mb/s path that was verified on the reference machine.
int itlmt7925::m3Run()
{
    // start at the highest level that has not failed for this network in this session (lvlCap), not always at itlmt_fast
    lvl = m3fast ? (fastLevel < lvlCap ? fastLevel : lvlCap) : 0;
    for (;;) {
        int rc = m3Run1();
        if (!m3stepDown || lvl == 0) return rc;
        uint8_t next = lvl - 1;
        const bool forced = lvlForce != 0;                   // run-time control (itl_lvl): exactly the level asked for
        if (forced) {
            next = lvlForce;
            lvlForce = 0;
        } else {
            // levels 4..6 add features only where the AP offers them: skip the ones that would repeat the configuration that just failed
            while (next >= 4 && fastSig(next) == fastSig(lvl)) next--;
            if (next == 2) next = 1;                         // RX Block Ack alone buys nothing without TX Block Ack: one attempt less
        }
        baLog("level %u given up, connecting again at %u", lvl, next);
        evLog("level %u -> %u%s", lvl, next, forced ? " (asked for)" : m3stepRadio ? " (radio; next connection starts high again)" : "");
        m3Teardown();
        lvl = next;
        // A level the AP does not work with stays given up for this network. Failing transmissions are radio conditions
        // (distance, interference, a busy 160 MHz channel): run lower now, but let the next connection try the full level again,
        // instead of staying slow for the rest of the boot after one bad minute.
        if (!m3stepRadio || forced) {                        // (a level asked for at run time sticks: a re-join starts there too)
            lvlCap = next;
            capMs = nowMs() ? nowMs() : 1;
            capBand = tgtCh <= 14 ? 1 : 2;
        }
    }
}

void itlmt7925::m3Teardown()
{
    m3linkUp = false;
    ethLink(false);
    baReset();
    uint8_t f[26];
    m3Hdr(f, 0xc0, tgtBssid, mac);                           // deauthentication, reason 3: station is leaving
    put16(f + 24, 3);
    m3Tx(f, sizeof(f), 0, 12);
    IOSleep(100);
    m3Bss(M3_BSS_REMOVE);                                    // BSS back to "not connected"
    m3dataOn = false;
    m3cipher = 0;
    IOSleep(100);
}

// ---------------------------------------------------------------------------------------------
// Step 4: data frames, 4-way handshake, keys, link
// ---------------------------------------------------------------------------------------------
static inline uint16_t be16p(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// One record per Ethernet frame (ring of the last kFrRing): what went through just before the firmware stopped answering.
void itlmt7925::frameNote(char dir, const uint8_t *e, uint32_t len)
{
    if (len < 14) return;
    FrameRec &f = frRing[frHead++ % kFrRing];
    f.ms = nowMs();
    f.len = (uint16_t)len;
    f.dir = (uint8_t)dir;
    f.et = (uint16_t)(e[12] << 8 | e[13]);
    f.proto = 0;
    f.a = f.b = 0;
    if (f.et == 0x0806 && len >= 22) {
        f.a = e[21];                                         // ARP operation
    } else if (f.et == 0x0800 && len >= 34) {
        const uint8_t *ip = e + 14;
        uint32_t ihl = (ip[0] & 15) * 4;
        f.proto = ip[9];
        if ((f.proto == 17 || f.proto == 6) && len >= 14 + ihl + 4) {
            f.a = (uint16_t)(ip[ihl] << 8 | ip[ihl + 1]);
            f.b = (uint16_t)(ip[ihl + 2] << 8 | ip[ihl + 3]);
        } else if (f.proto == 1 && len > 14 + ihl) {
            f.a = ip[ihl];
        }
    }
}

// Everything that could explain a silent firmware, in one go (no secrets: registers, ring indices, frame kinds).
void itlmt7925::m3Postmortem(const char *tag)
{
    uint8_t cap = 0;
    uint16_t lnkCtl = 0xffff, lnkSta = 0xffff, cmd = pci->configRead16(kIOPCIConfigCommand), sta = pci->configRead16(kIOPCIConfigStatus);
    if (pci->findPCICapability(0x10, &cap) && cap) {
        lnkCtl = pci->configRead16(cap + 0x10);
        lnkSta = pci->configRead16(cap + 0x12);
    }
    uint32_t doneEvt = 0, doneDat = 0;
    for (int i = 0; i < rxEvt.n; i++) if (le32(rxEvt.desc.va + i * 16 + 4) & MT_DMA_CTL_DMA_DONE) doneEvt++;
    for (int i = 0; i < rxData.n; i++) if (le32(rxData.desc.va + i * 16 + 4) & MT_DMA_CTL_DMA_DONE) doneDat++;
    char b[700];
    snprintf(b, sizeof(b),
             "%s: chip=%08x lp=%08x misc=%08x dummy=%08x glo=%08x ints=%08x inte=%08x pci cmd=%04x sta=%04x lnkctl=%04x lnksta=%04x "
             "txwm %u/%u txdat %u/%u txfwdl %u/%u rxevt %u/%u t=%d done=%u/%d rxdat %u/%u t=%d done=%u/%d fwlog=%u ev=%u",
             tag, rdA(MT_HW_CHIPID), rdA(MT_CONN_ON_LPCTL), rdA(MT_CONN_ON_MISC), rdA(MT_WFDMA_DUMMY_CR), rdA(MT_WFDMA0_GLO_CFG),
             rdA(MT_WFDMA0_HOST_INT_STA), rdA(MT_WFDMA0_HOST_INT_ENA), cmd, sta, lnkCtl, lnkSta, rd(txWm.regs + Q_CPU_IDX),
             rd(txWm.regs + Q_DMA_IDX) & 0xffff, txData.n ? rd(txData.regs + Q_CPU_IDX) : 0,
             txData.n ? rd(txData.regs + Q_DMA_IDX) & 0xffff : 0, rd(txFwdl.regs + Q_CPU_IDX), rd(txFwdl.regs + Q_DMA_IDX) & 0xffff,
             rd(rxEvt.regs + Q_CPU_IDX), rd(rxEvt.regs + Q_DMA_IDX) & 0xffff, rxEvt.tail, doneEvt, rxEvt.n,
             rxData.n ? rd(rxData.regs + Q_CPU_IDX) : 0, rxData.n ? rd(rxData.regs + Q_DMA_IDX) & 0xffff : 0, rxData.tail, doneDat,
             rxData.n, fwLogCnt, evCount);
    m3log("postmortem %s", b);
    setProperty("wifi-postmortem", b);
    // the last frames, newest last: "<ms ago><t|r> <ethertype>/<proto> <a>><b> <len>"
    char fr[800];
    size_t o = 0;
    uint32_t now = nowMs();
    fr[0] = 0;
    uint32_t cnt = frHead < (uint32_t)kFrRing ? frHead : (uint32_t)kFrRing;
    for (uint32_t i = 0; i < cnt; i++) {
        const FrameRec &f = frRing[(frHead - cnt + i) % kFrRing];     // oldest first
        int n = snprintf(fr + o, sizeof(fr) - o, "%u%c %04x/%u %u>%u %u;", now - f.ms, f.dir, f.et, f.proto, f.a, f.b, f.len);
        if (n <= 0 || o + (size_t)n >= sizeof(fr)) break;
        o += (size_t)n;
    }
    setProperty("wifi-frames", fr);
}

// Redo what a cold start does (wfsys reset, rings, firmware, interface, first scan). Runs on the worker thread only.
bool itlmt7925::chipRecover()
{
    recoverCnt++;
    evLog("chip not answering: reset and firmware reload #%u", recoverCnt);
    m3CmdTimeouts = 0;
    awdlOwnSent = false;                                     // the reloaded firmware has no second OMAC
    m3log("recover #%u: wfsys reset, rings, firmware", recoverCnt);
    m1len = 0;                                               // m1-log then holds this recovery's M1 steps, not the boot's
    m1buf[0] = 0;
    m3linkUp = false;
    m3dataOn = false;
    m3cipher = 0;
    ethStalled = false;
    fastActive = false;
    txPool = false;
    txClrAll();
    m3hw = false;                                            // a reloaded firmware has none of the one-time setup (fw_log, efuse, RTS): redo it
    clcCc[0] = 0;                                            // ... nor the CLC / SKU power tables
    // The rings keep their DMA memory: m1Run() -> ringInit() reuses it and only resets descriptors and indices (as Linux's
    // mt792x_wpdma_reset). Freeing and allocating ~4 MB of physically contiguous memory below 4 GB on every recovery failed
    // after a few of them ("ring alloc failed", M1 rc=-5, recover #3..#11 on 2026-11-05), and the chip stayed dead.
    int rc = m1Run();
    m3log("recover: M1 rc=%d", rc);
    if (rc) {                                                // the step it stopped at (the tail of m1-log)
        size_t from = m1len > 180 ? m1len - 180 : 0;
        m3log("recover: M1 tail ...%s", m1buf + from);
    }
    if (!rc) {
        rc = m2Run();
        m3log("recover: M2 rc=%d", rc);
    }
    evLog("recover #%u rc=%d", recoverCnt, rc);
    return rc == 0;
}

// First few interesting packets (ARP, UDP incl. DHCP, ICMP) in either direction, to see how far traffic gets.
void itlmt7925::m3Note(const char *dir, const uint8_t *e, uint32_t len)
{
    frameNote(dir[0], e, len);
    if (stNote >= 24 || len < 34) return;
    uint16_t et = be16p(e + 12);
    if (et == 0x0806) {
        stNote++;
        m3log("%s arp op=%u", dir, e[21]);
    } else if (et == 0x0800) {
        const uint8_t *ip = e + 14;
        uint32_t ihl = (ip[0] & 15) * 4;
        if (ip[9] == 17 && len >= 14 + ihl + 8) {
            stNote++;
            m3log("%s udp %u>%u len=%u", dir, be16p(ip + ihl), be16p(ip + ihl + 2), len);
        } else if (ip[9] == 1 && len > 14 + ihl) {
            stNote++;
            m3log("%s icmp type=%u", dir, ip[ihl]);
        }
    }
}

// ---- receive: Ethernet frames to the stack, through the reorder buffer of the TID when there is one ----
void itlmt7925::m3Hex(char *out, size_t outLen, const uint8_t *p, uint32_t n)
{
    static const char hx[] = "0123456789abcdef";
    size_t k = 0;
    for (uint32_t i = 0; i < n && k + 3 < outLen; i++) { out[k++] = hx[p[i] >> 4]; out[k++] = hx[p[i] & 15]; }
    out[k] = 0;
}

// Hands the frames collected for one MPDU to the reorder buffer (or, if the agreement is gone, to the stack).
void itlmt7925::amsduFlush()
{
    AmsduAsm &a = amsduAsm;
    if (!a.active) return;
    a.active = false;
    uint32_t used = a.used;
    a.used = 0;
    if (rxBa[a.tid].r.active) itl_reorder_rx(&rxBa[a.tid].r, a.sn, used ? a.buf : nullptr, used, nowMs(), baDeliverCb, this);   // used == 0 still takes up the sequence number
    else if (used) itl_bundle_each(a.buf, used, ethDeliverCb, this);
}

void itlmt7925::rxStore(const uint8_t *eb, uint32_t n, bool reorder, uint8_t tid, uint16_t sn, bool last)
{
    AmsduAsm &a = amsduAsm;
    szParsed[szClass(n)]++;
    if (isBigEcho(eb, n, 0)) icmpRepParsed++;
    if (!reorder) {
        amsduFlush();
        ethDeliver(eb, n);
        return;
    }
    if (a.active && (a.tid != tid || a.sn != sn)) amsduFlush();
    if (!a.active) { a.active = true; a.tid = tid; a.sn = sn; a.used = 0; a.t = nowMs(); }
    uint32_t u = itl_bundle_add(a.buf, sizeof(a.buf), a.used, eb, n);
    if (u == a.used) { stRxDrop++; stBundleOvf++; } else a.used = u;
    if (last) amsduFlush();
}

// The body of an A-MSDU subframe packet: [2 bytes pad when the hardware split the A-MSDU] then the subframes (itl_amsdu_each); a
// whole A-MSDU the hardware left alone has no pad. Returns the frames unpacked.
struct M3AmsduCtx { itlmt7925 *d; bool reorder; uint8_t tid; uint16_t sn; uint32_t good; };

void itlmt7925::amsduFrameCb(void *ctx, const uint8_t *da, const uint8_t *sa, uint16_t et, const uint8_t *pl, uint32_t pn)
{
    M3AmsduCtx *x = (M3AmsduCtx *)ctx;
    itlmt7925 *d = x->d;
    static uint8_t eb[14 + 1600];
    if (memcmp(sa, d->mac, 6) == 0) return;                                  // our own broadcast echoed back
    memcpy(eb, da, 6);
    memcpy(eb + 6, sa, 6);
    eb[12] = (uint8_t)(et >> 8); eb[13] = (uint8_t)et;
    memcpy(eb + 14, pl, pn);
    if (!(da[0] & 1)) d->stRxUni++;
    x->good++;
    d->rxStore(eb, 14 + pn, x->reorder, x->tid, x->sn, false);
}

uint32_t itlmt7925::m3RxAmsdu(const uint8_t *b, uint32_t n, uint8_t ai, bool reorder, uint8_t tid, uint16_t sn)
{
    M3AmsduCtx x = { this, reorder, tid, sn, 0 };
    if (reorder) {
        // a new MPDU (first subframe, or another sequence number) closes the previous bundle; an MPDU whose subframes all fail
        // to parse still has to take up its sequence number, so the bundle is opened before parsing
        if (amsduAsm.active && (ai == 3 || amsduAsm.tid != tid || amsduAsm.sn != sn)) amsduFlush();
        if (!amsduAsm.active) { amsduAsm.active = true; amsduAsm.tid = tid; amsduAsm.sn = sn; amsduAsm.used = 0; amsduAsm.t = nowMs(); }
    }
    const uint32_t pads[2] = { ai ? 2u : 0u, ai ? 0u : 2u };
    for (int attempt = 0; attempt < 2 && !x.good; attempt++) {
        if (pads[attempt] >= n) continue;
        uint32_t found = itl_amsdu_each(b + pads[attempt], n - pads[attempt], 1600, amsduFrameCb, &x);
        if (found > x.good) stRxBigDrop += found - x.good;                   // well-formed but too large (or echoed)
    }
    // The last subframe closes the bundle. With ai == 0 the packet is either a whole A-MSDU or a split subframe whose position the
    // hardware did not say: leave the bundle open, so that further subframes of the same MPDU still join it. The next MPDU or the
    // 4 ms timer in m3Link() flushes it.
    if (reorder && ai == 1) amsduFlush();
    return x.good;
}

// RX data frame from the AP (802.11 header + LLC/SNAP after the RXD groups; header translation is off, decryption is done by
// the hardware once the keys are installed). EAPOL goes to the handshake, everything else to the Ethernet interface.
// The header sits after up to 32 + 4 x 16 + 96 bytes of RXD groups (mt7925_mac_fill_rx), so look for it from byte 32 on.
bool itlmt7925::m3RxData(const uint8_t *p, uint32_t len)
{
    szRing[szClass(len > 120 ? len - 120 : 0)]++;            // the RXD groups, 802.11 header and LLC are ~120 bytes more than the Ethernet frame
    for (uint32_t off = 32; off + 24 <= len && off < 240; off++) {
        uint8_t fc0 = p[off], fc1 = p[off + 1];
        if ((fc0 & 0x0c) != 0x08 || (fc1 & 3) != 2) continue;               // data frame, From DS
        if (memcmp(p + off + 10, tgtBssid, 6) != 0) continue;
        bool group = p[off + 4] & 1;
        if (!group && memcmp(p + off + 4, mac, 6) != 0) continue;
        bool qos = fc0 & 0x80;
        uint8_t qc = qos ? p[off + 24] : 0;                                  // QoS control: TID 0-3, ack policy 5-6, A-MSDU 7
        uint8_t tid = qc & 7;
        uint16_t sn = (uint16_t)((p[off + 22] | (p[off + 23] << 8)) >> 4);
        bool reorder = qos && !group && rxBa[tid].r.active && (((qc >> 5) & 3) == 0 || ((qc >> 5) & 3) == 3);
        uint8_t ai = len >= 20 ? (uint8_t)(le32(p + 16) & 3) : 0;           // RXD4 payload format: 3 first, 2 middle, 1 last subframe of an A-MSDU
        if (rxdLogged < 3 && m3linkUp && fastActive) {
            rxdLogged++;
            baLog("rxd len=%u d1=%08x d2=%08x d4=%08x hdr@%u fc=%02x%02x qc=%02x", len, le32(p + 4), le32(p + 8), le32(p + 16), off, fc0, fc1, qc);
        }
        if (qos && !group && !(fc0 & 0x40) && !(((qc >> 7) & 1) && ai && ai != 3)) {   // one count per MPDU (A-MSDU: first subframe)
            stRxPkt++;
            if (fc1 & 0x08) stRxRetry++;
        }
        if (fc0 & 0x40) {                                                    // null / QoS null: no payload, but it owns a sequence number
            amsduFlush();
            if (reorder) itl_reorder_rx(&rxBa[tid].r, sn, nullptr, 0, nowMs(), baDeliverCb, this);
            return true;
        }
        uint32_t hl = qos ? 26 : 24;
        if (qos && (fc1 & 0x80)) hl += 4;                                    // +HTC
        if (qos && (qc & 0x80) && m3linkUp) {                                // A-MSDU (a subframe, or a whole one)
            stAmsduPkt++;
            uint32_t bo = off + hl;
            uint32_t n = bo < len ? m3RxAmsdu(p + bo, len - bo, ai, reorder, tid, sn) : 0;
            stAmsduSub += n;
            stAmsduAi[ai & 3]++;
            stAmsduN[n > 4 ? 4 : n]++;
            if (!n) {
                stAmsduBad++;
                if (amsduBadLogged < 3) {
                    amsduBadLogged++;
                    char hx[72];
                    m3Hex(hx, sizeof(hx), p + bo, bo < len ? (len - bo > 32 ? 32 : len - bo) : 0);
                    baLog("amsdu BAD ai=%u len=%u hdr@%u hl=%u sn=%u d=%s", ai, len, off, hl, sn, hx);
                }
            } else if (amsduLogged < 3) {
                amsduLogged++;
                baLog("amsdu ok ai=%u len=%u hdr@%u hl=%u sn=%u sub=%u", ai, len, off, hl, sn, n);
            }
            return true;
        }
        if (!group && !reorder && m3linkUp) {
            // Retry bit + the sequence number of the previous frame of this TID = the AP resent an MPDU we already delivered
            // (its ACK was lost). A frame without the Retry bit is always new, so an AP restart cannot be mistaken for one.
            const uint8_t di = qos ? tid : 8;
            const bool seen = (rxLastValid >> di) & 1;
            if ((fc1 & 0x08) && seen && rxLastSn[di] == sn) { stRxDupL++; return true; }
            rxLastSn[di] = sn;
            rxLastValid |= (uint16_t)(1u << di);
        }
        static const uint8_t llc[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
        for (uint32_t g = 0; g <= 12 && off + hl + g + 8 <= len; g++) {
            const uint8_t *l = p + off + hl + g;
            if (memcmp(l, llc, 6) != 0) continue;
            uint16_t et = be16p(l + 6);
            const uint8_t *pl = l + 8;
            uint32_t pn = len - (uint32_t)(pl - p);
            stRxData++;
            lastRxDataMs = nowMs();
            if (et == 0x888e) {
                stRxEapol++;
                if (m3wantEapol || m3linkUp) m3Eapol(pl, pn);          // on the link: the AP's group key rekeys
                return true;
            }
            if (!m3linkUp || pn > 1600) { stRxDrop++; return true; }
            if (memcmp(p + off + 16, mac, 6) == 0) {                        // our own broadcast echoed back: the AP relayed it
                if (stEcho < 6) { stEcho++; m3log("rx echo et=%04x len=%u", et, len); }
                return true;
            }
            static uint8_t eb[14 + 1600];
            memcpy(eb, p + off + 4, 6);                                      // DA
            memcpy(eb + 6, p + off + 16, 6);                                 // SA = addr3
            eb[12] = (uint8_t)(et >> 8); eb[13] = (uint8_t)et;
            memcpy(eb + 14, pl, pn);
            if (!group) stRxUni++;
            rxStore(eb, 14 + pn, reorder, tid, sn, true);
            return true;
        }
        return false;
    }
    return false;
}

bool itlmt7925::m3TxData(const uint8_t *da, uint16_t ethertype, const uint8_t *payload, uint32_t plen, bool protect, bool *full)
{
    uint8_t f[34 + 1560];
    if (plen > 1560) return false;
    // With HT/VHT negotiated, unicast data (not the EAPOL handshake) goes out as QoS data (TID 0) at the rate the firmware
    // chooses; group-addressed frames and everything else keep the fixed basic rate.
    bool ra = fastActive && m3linkUp && ethertype != 0x888e && !(da[0] & 1);
    uint32_t hl = ra ? 26 : 24;
    bzero(f, hl);
    f[0] = ra ? 0x88 : 0x08;                                 // data / QoS data
    f[1] = protect ? 0x41 : 0x01;                            // To DS (+ Protected: the hardware adds the CCMP header)
    memcpy(f + 4, tgtBssid, 6);
    memcpy(f + 10, mac, 6);
    memcpy(f + 16, da, 6);                                   // QoS control (bytes 24-25) stays 0: TID 0, normal ack
    M3TxOpt o;
    o.protect = protect;
    o.rateAdapt = ra;
    o.full = full;
    uint16_t sn = txSn;
    if (ra && lvl >= 3) {
        // Level 3: access-category queue (AC_BE -> LMAC queue 1, mt76_connac_lmac_mapping), sequence numbers chosen here so that the
        // starting sequence number of the ADDBA request is known, and A-MPDU once the agreement is up.
        o.qidx = 1;
        o.sn = sn;
        o.aggr = txBaState == TXBA_ON;
        f[22] = (uint8_t)((sn & 0xf) << 4);                  // sequence control: SN << 4, fragment 0
        f[23] = (uint8_t)(sn >> 4);
    }
    static const uint8_t llc[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
    memcpy(f + hl, llc, 6);
    f[hl + 6] = (uint8_t)(ethertype >> 8); f[hl + 7] = (uint8_t)ethertype;
    memcpy(f + hl + 8, payload, plen);
    bool ok = m3Tx(f, hl + 8 + plen, 2, ra ? 8 : 0, o);
    if (ok && ra) {
        stTxUni++;
        if (lvl >= 3) txSn = itl_sn_inc(sn);                 // only a frame that really went out uses up a sequence number
    }
    return ok;
}

// Ethernet frame from the network stack (output queue thread): 0 sent, 1 TX pool full (retried after ethTxKick()), -1 dropped
int itlmt7925::ethTx(const uint8_t *e, uint32_t len)
{
    if (!m3linkUp || len < 14) return -1;
    bool full = false;
    bool ok = m3TxData(e, be16p(e + 12), e + 14, len - 14, true, &full);
    if (full) return 1;
    if (isBigEcho(e, len, 8)) icmpReqTx++;
    if (ok) stTx++; else stTxFail++;
    m3Note("tx", e, len);
    return ok ? 0 : -1;
}

// STA_REC_KEY_V3 (mt7925_mcu_sta_key_tlv): key on the AP's wcid (pairwise) or on the BSS's broadcast wcid (group).
// TKIP carries 32 bytes (key + TX MIC + RX MIC) and the firmware wants the two MIC keys the other way round
// (mt7925_mcu_sta_key_tlv: "Rx/Tx MIC keys are swapped").
int itlmt7925::m3Key(uint16_t wcid, bool pairwise, uint8_t keyId, const uint8_t *key, uint8_t keyLen, uint8_t cipher)
{
    M3Msg m;
    m.init(8);
    m.b[0] = M3_BSS_IDX;
    m.b[1] = wcid & 0xff;
    m.b[4] = 1;                                              // is_tlv_append
    m.b[5] = pairwise ? 0 : 0xe;                             // muar_idx: omac for a station, 0xe for the broadcast wcid
    m.b[6] = wcid >> 8;
    uint8_t *s = m.tlv(0x27, 68);                            // STA_REC_KEY_V3
    s[4] = 1;                                                // add
    s[5] = pairwise ? 1 : 0;                                 // tx_key
    s[6] = pairwise ? 1 : 0;                                 // key_type
    s[7] = 0;                                                // is_authenticator
    memcpy(s + 8, tgtBssid, 6);                              // peer_addr
    s[14] = M3_BSS_IDX;
    s[15] = cipher ? cipher : 4;                             // CONNAC3_CIPHER_AES_CCMP (4) / CONNAC3_CIPHER_TKIP (2)
    s[16] = keyId;
    s[17] = keyLen;
    s[18] = wcid & 0xff;
    s[19] = 1;                                               // mgmt_prot
    if (s[15] == 2 && keyLen == 32) {
        memcpy(s + 20, key, 16);
        memcpy(s + 36, key + 24, 8);
        memcpy(s + 44, key + 16, 8);
    } else {
        memcpy(s + 20, key, keyLen);
    }
    char nm[24];
    snprintf(nm, sizeof(nm), pairwise ? "key(ptk,c%02x)" : "key(gtk,c%02x)", s[15]);
    int rc = m3Cmd(UNI_STA_REC_UPDATE, m.b, m.n, nm);
    bzero(m.b, sizeof(m.b));
    return rc;
}

// Preserve every complete key frame in a poll batch. The previous single mailbox silently
// discarded M3 when a retransmitted M1 happened to arrive first in the same batch.
bool itlmt7925::m3Eapol(const uint8_t *p, uint32_t len)
{
    itl_eapol_key k;
    int rc = itl_eapol_parse(p, len, &k);
    if (rc) { m3log("eapol rx rejected parse=%d len=%u", rc, len); return false; }
    uint32_t n = 4u + be16p(p + 2);
    if (n > sizeof(m3eapol) || m3eapolHead - m3eapolTail >= kEapolQueue) {
        m3eapolDrops++;
        m3log("eapol rx dropped len=%u depth=%u drops=%u", n, m3eapolHead - m3eapolTail, m3eapolDrops);
        return false;
    }
    EapolRx &r = m3eapolQueue[m3eapolHead++ % kEapolQueue];
    r.len = n;
    memcpy(r.data, p, n);
    return true;
}

bool itlmt7925::m3EapolNext()
{
    if (m3eapolTail == m3eapolHead) return false;
    EapolRx &r = m3eapolQueue[m3eapolTail++ % kEapolQueue];
    m3eapolLen = r.len;
    memcpy(m3eapol, r.data, r.len);
    bzero(&r, sizeof(r));
    return true;
}

int itlmt7925::m3Handshake()
{
    uint8_t rsnIe[22];
    m3RsnIe(rsnIe, apGroupCipher, apPairCipher);
    uint8_t snonce[32], zero[32], replay[8] = {}, anonce[32] = {}, out[160], kd[8 * 40];
    itl_eapol_key k;
    itl_ptk ptk = {};
    bzero(zero, sizeof(zero));
    read_random(snonce, sizeof(snonce));
    m3dataOn = true;
    m3eapolLen = 0;
    m3eapolHead = m3eapolTail = m3eapolDrops = 0;
    m3hsApLeave = false;
    m3hsApReason = 0;
    removeProperty("wifi-pmkid-match");
    removeProperty("wifi-handshake");
    m3wantEapol = true;

    bool have1 = false, got3 = false;
    uint32_t m1Count = 0, m2Count = 0, badMic = 0, rejected = 0;
    uint32_t started = nowMs(), phaseStart = started, lastM2 = 0;
    uint32_t rxBefore = stRxEapol, freeBefore = stTxFree, failBefore = stTxFwFail;
    size_t n2 = 0;
    auto sendM2 = [&]() {
        bool ok = m3TxData(tgtBssid, 0x888e, out, (uint32_t)n2, false);
        m2Count++;
        lastM2 = nowMs();
        m3log("eapol m2 #%u queued=%u replay=%02x%02x%02x%02x%02x%02x%02x%02x", m2Count, ok,
              replay[0], replay[1], replay[2], replay[3], replay[4], replay[5], replay[6], replay[7]);
    };
    // Six seconds for M1 and, after the first M1, six for M3. Retransmitted M1s
    // do not extend the deadline. Each one gets a response with ITS counter and ANonce.
    while ((uint32_t)(nowMs() - phaseStart) < 6000 && !got3 && !m3hsApLeave) {
        m3Pump(1);
        if (m3hsApLeave) break;
        while (m3EapolNext()) {
            int rc = itl_eapol_parse(m3eapol, m3eapolLen, &k);
            if (rc) { rejected++; continue; }
            m3log("eapol rx info=%04x len=%u replay=%02x%02x%02x%02x%02x%02x%02x%02x",
                  k.info, m3eapolLen, k.replay[0], k.replay[1], k.replay[2], k.replay[3],
                  k.replay[4], k.replay[5], k.replay[6], k.replay[7]);
            const uint16_t required = ITL_KI_PAIRWISE | ITL_KI_ACK;
            if ((k.info & ITL_KI_VER_MASK) != 2 || (k.info & required) != required ||
                (k.info & (ITL_KI_ERROR | ITL_KI_REQUEST))) {
                rejected++;
                m3log("eapol rejected unsupported flags=%04x", k.info);
                continue;
            }
            if (!(k.info & ITL_KI_MIC) && !(k.info & ITL_KI_INSTALL)) {
                int cmp = have1 ? memcmp(k.replay, replay, 8) : 1;
                if (cmp < 0 || (have1 && cmp == 0 && memcmp(k.nonce, anonce, 32))) {
                    rejected++;
                    m3log("eapol m1 rejected stale/inconsistent replay");
                    continue;
                }
                if (!have1) {
                    phaseStart = nowMs();
                    // An AP may include its PMKID in M1. Compare without publishing either
                    // key or hash: this separates a PMK/station-identity mismatch from lost M2/M3.
                    bool pmkidPresent = false;
                    const uint8_t *e = k.data, *end = k.data + k.datalen;
                    for (; e + 2 <= end && e + 2 + e[1] <= end; e += 2 + e[1]) {
                        if (e[0] != 0xdd || e[1] != 20 || e[2] != 0 || e[3] != 0x0f || e[4] != 0xac || e[5] != 4) continue;
                        uint8_t name[20], hash[20], diff = 0;
                        memcpy(name, "PMK Name", 8);
                        memcpy(name + 8, tgtBssid, 6);
                        memcpy(name + 14, mac, 6);
                        itl_hmac_sha1(m3pmk, 32, name, sizeof(name), nullptr, 0, hash);
                        for (int i = 0; i < 16; i++) diff |= hash[i] ^ e[6 + i];
                        setProperty("wifi-pmkid-match", diff == 0);
                        m3log("eapol m1 PMKID present match=%u", diff == 0);
                        bzero(hash, sizeof(hash));
                        pmkidPresent = true;
                        break;
                    }
                    if (!pmkidPresent) m3log("eapol m1 PMKID absent");
                }
                bool changed = !have1 || memcmp(anonce, k.nonce, 32);
                memcpy(replay, k.replay, 8);
                memcpy(anonce, k.nonce, 32);
                if (changed) itl_wpa_ptk(m3pmk, tgtBssid, mac, anonce, snonce, &ptk, apPairCipher == 2);
                have1 = true;
                m1Count++;
                n2 = itl_eapol_build(out, 0x010a, 0, replay, snonce, rsnIe, sizeof(rsnIe));
                itl_eapol_mic(ptk.kck, out, n2);
                m3log("eapol m1 #%u anonceChanged=%u", m1Count, changed);
                sendM2();
                continue;
            }
            if (!have1 || (k.info & (ITL_KI_MIC | ITL_KI_INSTALL)) != (ITL_KI_MIC | ITL_KI_INSTALL)) {
                rejected++;
                m3log("eapol rejected unexpected state/flags");
                continue;
            }
            if (memcmp(k.replay, replay, 8) <= 0 || memcmp(k.nonce, anonce, 32)) {
                rejected++;
                m3log("eapol m3 rejected replay/ANonce");
                continue;
            }
            uint32_t elen = 4u + be16p(m3eapol + 2);
            if (!itl_eapol_mic_ok(ptk.kck, m3eapol, elen)) {
                badMic++;
                m3log("eapol m3 MIC rejected count=%u", badMic);
                continue;
            }
            got3 = true;
            break;  // k.data points into m3eapol: do not pop another frame before GTK processing.
        }
        if (!got3 && have1 && (uint32_t)(nowMs() - lastM2) >= 1500) sendM2();
    }
    char summary[200];
    snprintf(summary, sizeof(summary), "m1=%u m2=%u m3=%u rx=%u micBad=%u reject=%u drop=%u txFree=%u txFail=%u apLeave=%u reason=%u ms=%u",
             m1Count, m2Count, got3, stRxEapol - rxBefore, badMic, rejected, m3eapolDrops,
             stTxFree - freeBefore, stTxFwFail - failBefore, m3hsApLeave, m3hsApReason, nowMs() - started);
    setProperty("wifi-handshake", summary); // survives saturation of the older 3 KB m3-log
    m3log("eapol summary %s", summary);
    if (!got3) {
        bzero(&ptk, sizeof(ptk));
        bzero(snonce, sizeof(snonce));
        m3log("eapol: %s", have1 ? "no valid msg3" : "no msg1");
        evLog("handshake failed: %s", summary);
        return (badMic || m3hsApLeave) ? ITL_EIO : ITL_TIMEOUT;
    }
    m3log("eapol m3 info=%04x mic ok datalen=%u", k.info, k.datalen);

    // key data: wrapped RSN IE + GTK KDE
    uint8_t gtk[32], gtkId = 0, gtkLen = 0;
    if ((k.info & ITL_KI_ENCRYPTED) && k.datalen >= 24 && k.datalen <= sizeof(kd) + 8 &&
        itl_aes128_unwrap(ptk.kek, k.data, k.datalen, kd) == 0) {
        const uint8_t *e = kd, *end = kd + k.datalen - 8;
        while (e + 2 <= end && e + 2 + e[1] <= end) {
            if (e[0] == 0xdd && e[1] >= 22 && e[2] == 0x00 && e[3] == 0x0f && e[4] == 0xac && e[5] == 1) {
                gtkLen = (uint8_t)(e[1] - 6);
                if (gtkLen > sizeof(gtk)) gtkLen = 0;
                else { gtkId = e[6] & 3; memcpy(gtk, e + 8, gtkLen); }
                break;
            }
            e += 2 + e[1];
        }
    } else {
        m3log("eapol: key data not usable (info=%04x len=%u)", k.info, k.datalen);
    }
    m3log("gtk id=%u len=%u", gtkId, gtkLen);
    if (gtkLen != 16 && gtkLen != 32) return ITL_EIO;          // 16 = CCMP, 32 = TKIP (key + TX/RX MIC keys)

    // msg4: ver 2 | pairwise | MIC | secure
    uint8_t replay3[8];
    memcpy(replay3, k.replay, 8);
    size_t n4 = itl_eapol_build(out, 0x030a, 0, replay3, zero, nullptr, 0);
    itl_eapol_mic(ptk.kck, out, n4);
    bool ok = m3TxData(tgtBssid, 0x888e, out, (uint32_t)n4, false);
    m3log("eapol m4 tx %s", ok ? "queued" : "FAILED");
    memcpy(m3replay, replay3, 8);
    // M4 must be on air (TXFREE for it) before the PTK is installed: with the key in place, a frame still in the queue is lost
    // or encrypted, the AP never sees M4 and resends M3 once the link is up (§10.46: `txFree=0` in the summary, the AP
    // dropping the link every 15-30 s). Up to 300 ms, at least the old 30 ms.
    {
        uint32_t f0 = stTxFree;
        for (int i = 0; i < 300 && (i < 30 || stTxFree == f0); i++) m3Pump(1);
        m3log("eapol m4 %s after wait", stTxFree != f0 ? "freed" : "NOT freed");
    }

    bool tkipPw = apPairCipher == 2, tkipGrp = gtkLen == 32;
    m3cipher = tkipPw ? 2 : 4;
    m3Bss(M3_BSS_ASSOC);                                       // BSS now secured (CCMP, or TKIP on a WPA/WPA2 mixed AP)
    uint8_t tk32[32];
    if (tkipPw) { memcpy(tk32, ptk.tk, 16); memcpy(tk32 + 16, ptk.mic, 16); }
    int r1 = m3Key(M3_AP_WCID, true, 0, tkipPw ? tk32 : ptk.tk, tkipPw ? 32 : 16, m3cipher);
    int r2 = m3Key(M3_BMC_WCID, false, gtkId, gtk, gtkLen, tkipGrp ? 2 : 4);
    memcpy(m3ptk, &ptk, sizeof(m3ptk));
    bzero(&ptk, sizeof(ptk));
    bzero(gtk, sizeof(gtk));
    bzero(kd, sizeof(kd));
    return (r1 || r2) ? ITL_EIO : 0;
}

// EAPOL-Key frames the AP sends once the link is up: the group key handshake (IEEE 802.11 12.7.7). Message 1/2 carries the new
// GTK, AES-key-wrapped with the KEK; the station checks its MIC (KCK), installs the key and answers with message 2/2 (ver 2 |
// MIC | secure, the same replay counter), encrypted with the PTK. Until §10.44 these frames were dropped once the 4-way
// handshake was over: an AP with a short group rekey interval resent message 1/2 a few times and then deauthenticated us
// (reason 1 / 2, every 30-80 s on a 5 GHz AP), and each drop cost a full chip reset and a rejoin.
void itlmt7925::m3EapolOnLink()
{
    const uint8_t *kck = m3ptk, *kek = m3ptk + 16;
    while (m3EapolNext()) {
        itl_eapol_key k;
        if (itl_eapol_parse(m3eapol, m3eapolLen, &k)) { m3gtkRekeyBad++; continue; }
        if (k.info & ITL_KI_PAIRWISE) {
            // A retransmitted message 3 (MIC | ACK | INSTALL, pairwise): the AP did not get our M4. Answer with M4 again, MIC'd
            // with the KCK we hold and sent in the clear like the first one; the keys are NOT installed again (a reinstall would
            // reset the PN: the KRACK pattern). A pairwise message 1 (a PTK rekey) is still not handled on the link.
            const uint16_t m3bits = ITL_KI_MIC | ITL_KI_ACK | ITL_KI_INSTALL;
            if ((k.info & ITL_KI_VER_MASK) == 2 && (k.info & m3bits) == m3bits && !(k.info & (ITL_KI_ERROR | ITL_KI_REQUEST))) {
                uint32_t elen3 = 4u + be16p(m3eapol + 2);
                if (memcmp(k.replay, m3replay, 8) <= 0 || !itl_eapol_mic_ok(kck, m3eapol, elen3)) {
                    m3gtkRekeyBad++;
                    m3log("eapol on link: m3 again rejected (replay/MIC)");
                    continue;
                }
                memcpy(m3replay, k.replay, 8);
                uint8_t out4[160], zero4[32];
                bzero(zero4, sizeof(zero4));
                size_t n4 = itl_eapol_build(out4, 0x030a, 0, k.replay, zero4, nullptr, 0);
                itl_eapol_mic(kck, out4, n4);
                bool ok4 = m3TxData(tgtBssid, 0x888e, out4, (uint32_t)n4, false);   // in the clear: the AP installs its PTK only on M4
                m3M4Again++;
                m3log("eapol on link: m3 again #%u, m4 resent=%s", m3M4Again, ok4 ? "queued" : "FAILED");
                continue;
            }
            m3gtkRekeyBad++;
            m3log("eapol on link: pairwise info=%04x ignored", k.info);
            continue;
        }
        const uint16_t need = ITL_KI_MIC | ITL_KI_ACK | ITL_KI_SECURE;
        if ((k.info & ITL_KI_VER_MASK) != 2 || (k.info & need) != need || (k.info & (ITL_KI_ERROR | ITL_KI_REQUEST))) {
            m3gtkRekeyBad++;
            m3log("eapol on link: group info=%04x unsupported", k.info);
            continue;
        }
        if (memcmp(k.replay, m3replay, 8) <= 0) {
            m3gtkRekeyBad++;
            m3log("eapol on link: group replay not newer");
            continue;
        }
        uint32_t elen = 4u + be16p(m3eapol + 2);
        if (!itl_eapol_mic_ok(kck, m3eapol, elen)) {
            m3gtkRekeyBad++;
            m3log("eapol on link: group MIC rejected");
            continue;
        }
        uint8_t kd[8 * 40], gtk[32], gtkId = 0, gtkLen = 0;
        if ((k.info & ITL_KI_ENCRYPTED) && k.datalen >= 24 && k.datalen <= sizeof(kd) + 8 &&
            itl_aes128_unwrap(kek, k.data, k.datalen, kd) == 0) {
            const uint8_t *e = kd, *end = kd + k.datalen - 8;
            while (e + 2 <= end && e + 2 + e[1] <= end) {
                if (e[0] == 0xdd && e[1] >= 22 && e[2] == 0x00 && e[3] == 0x0f && e[4] == 0xac && e[5] == 1) {
                    gtkLen = (uint8_t)(e[1] - 6);
                    if (gtkLen > sizeof(gtk)) gtkLen = 0;
                    else { gtkId = e[6] & 3; memcpy(gtk, e + 8, gtkLen); }
                    break;
                }
                e += 2 + e[1];
            }
        }
        if (gtkLen != 16 && gtkLen != 32) {
            m3gtkRekeyBad++;
            m3log("eapol on link: group key data not usable (info=%04x len=%u)", k.info, k.datalen);
            bzero(kd, sizeof(kd));
            continue;
        }
        memcpy(m3replay, k.replay, 8);
        uint8_t out[160], zero[32];
        bzero(zero, sizeof(zero));
        size_t n = itl_eapol_build(out, 0x0302, 0, k.replay, zero, nullptr, 0);   // ver 2 | MIC | secure (group)
        itl_eapol_mic(kck, out, n);
        bool ok = m3TxData(tgtBssid, 0x888e, out, (uint32_t)n, true);
        int rc = m3Key(M3_BMC_WCID, false, gtkId, gtk, gtkLen, gtkLen == 32 ? 2 : 4);
        m3gtkRekey++;
        m3log("eapol group rekey #%u id=%u len=%u msg2=%s key rc=%d", m3gtkRekey, gtkId, gtkLen, ok ? "queued" : "FAILED", rc);
        evLog("group rekey #%u id=%u rc=%d", m3gtkRekey, gtkId, rc);
        bzero(gtk, sizeof(gtk));
        bzero(kd, sizeof(kd));
    }
}

// Link is up: expose the Ethernet interface and keep servicing the receive rings. Returns only to give up the current fast level.
void itlmt7925::m3Link()
{
    bool eok = nativeMode || eth || ethCreate();
    m3log("%s interface %s", nativeMode ? "native" : "ethernet", eok ? "ready" : "FAILED");
    if (!nativeMode) ethLink(true);
    baReset();
    txSn = 0;
    rxLastValid = 0;
    stRxDupL = 0;
    stRxRetry = stRxPkt = 0;
    stRxUni = stTxUni = 0;
    stAmsduPkt = stAmsduSub = stAmsduBad = 0;
    txfWinFree = stTxFree;
    txfWinFail = stTxFwFail;
    txfBad = 0;
    uint32_t lastTxf = nowMs();
    m3linkUp = true;
    m3sleepy = true;
    uint32_t t0 = nowMs(), lastStat = 0, lastActive = t0;
    linkUpMs = t0;
    lastReorderMs = t0;
    m3lost = false;
    tgtLastBcnMs = 0;                                        // beacon watch is armed by the first beacon of this link
    lastRxDataMs = t0;
    tgtRssi = 0;
    wifiState("connected", "");
    wifiLink();
    if (njState == ITL_NATIVE_CONNECTING) njState = ITL_NATIVE_UP;   // the join the Family asked for (before its link callback)
    // Native publication may wake DHCP immediately. Radio TX/session state
    // must already be ready before entering the Family link transition.
    if (nativeMode) ethLink(true);
    evLog("up lvl %u ch %u bw %u", lvl, tgtCh, fastActive ? fastBw : 0);
    bool watchdogDone = false, silenceDone = false;
    uint32_t lastPulse = nowMs();
    for (;;) {
        int act = m3PumpOnce();
        if (m3eapolTail != m3eapolHead) m3EapolOnLink();
        uint32_t now = nowMs();
        if (now - lastPulse >= 5000) {                       // heartbeat: RX liveness (beacons, data, group frames the AP relayed back)
            lastPulse = now;
            m3log("pulse bcn=%u rxd=%u rxe=%u eth=%u tx=%u echo=%u", tgtBcnCount, stRxData, stRxEapol, stRxEth, stTx, stEcho);
        }
        if (awdlHeldCh && !cmdPending(CMD_AWDL_SYNC) &&
            (awdlUptimeUs() > awdlHeldUntilUs || cmdPending(CMD_SCAN | CMD_JOIN | CMD_DISCONNECT))) {
            awdlChainTimeout++;                              // the next window never came: give the radio back to the AP
            if (awdlChainTimeout <= 8)
                m3log("awdl hold timeout: late=%dus seq=%u done=%u ch=%u/%u", (int)(int64_t)(awdlUptimeUs() - awdlHeldUntilUs),
                      __atomic_load_n(&awdlFrameSeq, __ATOMIC_ACQUIRE), awdlFrameDone, awdlChannel, awdlHeldCh);
            awdlHeldClose();
        }
        if (cmdPending(CMD_SCAN)) {                          // scan on the link: the firmware returns to the AP's channel between dwells
            cmdTake(CMD_SCAN, nullptr);
            wifiScan();
            wifiState("connected", "");
            now = nowMs();
        }
        if (cmdPending(CMD_AWDL_SYNC)) {                     // AWDL PSF frame: one ROC window on the social channel, then back
            cmdTake(CMD_AWDL_SYNC, nullptr);
            awdlSyncTx();
            now = nowMs();
        }
        if (cmdPending(CMD_JOIN | CMD_DISCONNECT)) {         // another network, or off: m3Supervise() takes it from here
            m3log("link: leaving for a command");
            m3Teardown();
            return;
        }
        if (ctlLvl || ctlPwr) ctlPoll();
        tsSample(now);
        if (!silenceDone && tgtLastBcnMs && now - tgtLastBcnMs > 4000 && now - lastRxDataMs > 4000) {
            // No beacon for 4 s (the AP sends ten a second): snapshot, then take driver ownership again in case the firmware
            // went to sleep on its own, and snapshot once more to see whether that changed anything.
            silenceDone = true;
            m3Postmortem("silent 4s");
            bool own = driverOwn();
            wake7927();
            evLog("silent for 4 s: driverOwn=%d", own ? 1 : 0);
            IOSleep(50);
            m3Postmortem("after driverOwn");
        }
        if (!m3lost && recoverWanted) {                      // two MCU timeouts in a row: reset now, not 15 s later
            snprintf(m3lostWhy, sizeof(m3lostWhy), "the firmware stopped answering MCU commands");
            m3lost = true;
        }
        if (!m3lost && tgtLastBcnMs && now - tgtLastBcnMs > 15000 && now - lastRxDataMs > 15000) {
            snprintf(m3lostWhy, sizeof(m3lostWhy), "no beacon or data from the AP for 15 s");
            m3lost = true;
        }
        if (m3lost) {
            m3log("link lost: %s", m3lostWhy);
            evLog("lost after %us at lvl %u: %s", (now - t0) / 1000, lvl, m3lostWhy);
            m3Postmortem("lost");
            recoverWanted = true;                            // the supervisor resets the chip once the link state is torn down
            m3Teardown();
            return;
        }
        baService();
        if (ethStalled && now - ethStallMs >= 20) { ethStalled = false; ethTxKick(); }   // no TXFREE came: retry (txSlotAlloc reclaims)
        if (amsduAsm.active && now - amsduAsm.t >= 4) amsduFlush();      // the last subframe of an A-MSDU never came
        if (now - lastReorderMs >= 10) {                     // frames waiting behind a hole in a Block Ack window for too long
            lastReorderMs = now;
            for (int t = 0; t < 8; t++)
                if (rxBa[t].r.active) itl_reorder_tick(&rxBa[t].r, now, reoMs, baDeliverCb, this);
        }
        ethFlush();
        uint32_t up = (now - t0) / 1000;
        if (fastActive && !watchdogDone && up >= 15) {
            watchdogDone = true;
            if (!stRxUni && stTx >= 2) {                     // we transmitted, and no unicast frame ever came back: HT/VHT is not working
                baLog("lvl %u: no unicast rx in %us (tx=%u rxd=%u eth=%u)", lvl, up, stTx, stRxData, stRxEth);
                evLog("lvl %u: no unicast frame received in %us tx=%u fail=%u fwfail=%u free=%u rxd=%u eapol=%u eth=%u", lvl, up,
                      stTx, stTxFail, stTxFwFail, stTxFree, stRxData, stRxEapol, stRxEth);
                m3stepDown = true;
            }
        }
        if (fastAmsdu && ((stAmsduPkt >= 8 && !stAmsduSub) || (stAmsduBad >= 32 && stAmsduBad * 4 > stAmsduSub))) {
            baLog("lvl %u: A-MSDUs cannot be unpacked (pkt=%u sub=%u bad=%u)", lvl, stAmsduPkt, stAmsduSub, stAmsduBad);
            evLog("lvl %u: A-MSDUs cannot be unpacked", lvl);
            m3stepDown = true;
        }
        if (txPool && now - lastTxf >= 5000) {                           // nearly every transmission reported failed by the firmware, twice in a row
            lastTxf = now;
            uint32_t df = stTxFree - txfWinFree, dx = stTxFwFail - txfWinFail;
            txfWinFree = stTxFree;
            txfWinFail = stTxFwFail;
            if (df >= 30 && dx * 10 >= df * 8) {
                if (++txfBad >= 2) {
                    baLog("lvl %u: tx failing (freed=%u failed=%u in 5 s)", lvl, df, dx);
                    evLog("lvl %u: tx failing, %u of %u frames in 5 s, rssi %d", lvl, dx, df, tgtRssi);
                    m3stepDown = m3stepRadio = true;
                }
            } else {
                txfBad = 0;
            }
        }
        if (up >= lastStat + (statSecs ? statSecs : (up < 300 ? 5 : 60))) {
            lastStat = up;
            uint32_t ri = 0, rd_ = 0, rb = 0, rdup = 0, rold = 0, rfl = 0, rto = 0, rlate = 0;
            for (int t = 0; t < 8; t++) {
                ri += rxBa[t].r.nIn; rd_ += rxBa[t].r.nDirect; rb += rxBa[t].r.nBuffered; rdup += rxBa[t].r.nDup;
                rold += rxBa[t].r.nOld; rfl += rxBa[t].r.nFlushed; rto += rxBa[t].r.nTimeout; rlate += rxBa[t].r.nLate;
            }
            char st[400];
            snprintf(st, sizeof(st),
                     "up=%us L%u rxd=%u eapol=%u eth=%u drop=%u tx=%u fail=%u rxpk=%u uni=%u/%u ba=%u/%u reo=%u/%u/%u/%u/%u/%u/%u/%u txf=%u/%u/%u/%u/%u am=%u/%u/%u/%u rty=%u/%u bw=%u",
                     up, lvl, stRxData, stRxEapol, stRxEth, stRxDrop, stTx, stTxFail, rxPkts, stRxUni, stTxUni, stBaRxOn, stBaTxOn, ri, rd_,
                     rb, rdup, rold, rfl, rto, rlate, stTxFree, stTxReclaim, stTxNoSlot, stTxFwFail, stTxFwRetry, stAmsduPkt, stAmsduSub, stAmsduBad,
                     stRxBigDrop, stRxRetry, stRxPkt, fastActive ? fastBw : 0);
            nvramStat(st);
            nvramChunks(m3buf);
            wifiLink();
        }
        if (m3stepDown) return;
        if (!fastActive) { IOSleep(1); continue; }           // legacy path: exactly as verified (1 ms poll)
        if (act) { lastActive = now; IODelay(20); }
        else if (now - lastActive < 50) IODelay(200);       // traffic is bursty (A-MPDUs): keep polling closely for a while
        else IOSleep(1);
    }
}
