// AWDL "park" mode (itl_awdlpark=<channel>, docs/ROADMAP-AWDL.md §10.68): AWDL with the station not associated.
//
// On the MT7927 no way of running AWDL next to an associated station on the same channel kept the station alive here
// (2026-11-07: the sniffer, and even the other-BSS RX filter alone, stopped the station's own unicast), and Owly lists the
// MT7927 as failing in its shared mode on Linux too (t4t5/owly#44). The one MT7927 setup known to transfer files
// (Guizzan/mt7927-airdrop, Linux) runs AWDL exclusively: the station is down, the radio sits on channel 44 and the sniffer is
// left on there, configured for that channel. This file does the same: while no network is joined and the AWDL interface
// is handing over windows, the radio is held on the park channel with a long remain-on-channel (renewed before it runs out),
// the sniffer and the other-BSS filter stay on, and every AWDL window happens there (awdlSyncTx treats it as "on channel").
// The infrastructure network is not used meanwhile; the Mac's internet goes over Ethernet.

#include "itlmt7925.hpp"

static inline void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint64_t parkUptimeUs()
{
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000;
}

// One remain-on-channel on the park channel, waited for up to 300 ms (as an AWDL window waits for its grant).
bool itlmt7925::awdlParkRoc(uint8_t ch)
{
    if (m3Roc(true, ch, 1, kAwdlParkRocMs)) return false;
    for (int i = 0; i < 300 && rocWant && !rocGrant; i++) m3Pump(1);
    if (!rocGrant || rocStatus) {
        m3Roc(false, ch);
        return false;
    }
    awdlParkRocEndUs = parkUptimeUs() + (uint64_t)kAwdlParkRocMs * 1000;
    return true;
}

// Our last PSF (or MIF) again, stamped for now, on the park channel (the afall resend of awdlSyncTx, outside a window).
void itlmt7925::awdlParkAf(uint8_t ch, bool mif)
{
    uint32_t len = mif ? awdlAfKeepLen2 : awdlAfKeepLen;
    if (!len || len > sizeof(awdlTxBuf) || len < 40) return;
    uint8_t *f = awdlTxBuf;                                  // the link thread's buffer; no window is running now
    memcpy(f, mif ? awdlAfKeep2 : awdlAfKeep, len);
    if (cmdLock) IOLockLock(cmdLock);
    AwdlTimeline tl = { awdlAwBaseUs, awdlAwBaseSeq, awdlAwFlags, awdlAwPeriodUs };
    if (cmdLock) IOLockUnlock(cmdLock);
    awdlAfSetCh(f, len, ch);
    uint32_t s = (awdlAfSeq++ & 0xfff) << 4;                 // as stamp() in awdlSyncTx
    f[22] = (uint8_t)(s & 0xff);
    f[23] = (uint8_t)((s >> 8) & 0xff);
    uint32_t us = (uint32_t)parkUptimeUs();
    put32(f + 32, us);
    put32(f + 36, us);
    awdlElFix(f, len);
    awdlSeqOneCh(f, len);
    if (awdlStampSync(f, len, tl)) awdlStamped++;
    M3TxOpt o;
    o.qidx = 0x10;                                           // ALTX0, OFDM 6 Mb/s, as the window's PSF/MIF
    o.rateAdapt = false;
    o.protect = false;
    awdlParkAfTx++;
    if (m3Tx(f, len, 0, 13, o)) awdlParkAfOk++;
}

bool itlmt7925::awdlParkStart(uint8_t ch)
{
    awdlCurBand = chBandIdx(ch);                             // sniffer / filter header and TX band for the whole park
    m3RxFilter(0x80000040u);                                 // other-BSS frames through (AWDL's BSSID)
    if (!awdlParkRoc(ch)) {
        m3RxFilter(0x80000000u);
        awdlParkFail++;
        m3log("awdl park: no grant on ch %u (status %u)", ch, rocStatus);
        return false;
    }
    m3SnifferEnable(true);
    m3SnifferConfig(ch);                                     // the sniffer is told its channel (Guizzan's MT7927 fix)
    awdlSnifPersist = 3;                                     // awdlSyncTx: filter and sniffer are already on, leave them
    awdlParked = 1;
    awdlRocCh = ch;
    awdlRocActive = 1;                                       // AWDL RX (data, peer heard) between windows too
    awdlParkN++;
    m3log("awdl park: on ch %u band %u", ch, awdlCurBand);
    return true;
}

void itlmt7925::awdlParkStop()
{
    if (!awdlParked) return;
    uint8_t ch = awdlRocCh;
    awdlParked = 0;
    awdlRocActive = 0;
    awdlSnifPersistOff();                                    // sniffer off, station RX filter back
    m3Roc(false, ch);
    m3log("awdl park: off (windows %u renew %u/%u)", awdlParkWin, awdlParkRenew, awdlParkRenewFail);
}

// Runs while the station is idle and the AWDL interface hands over windows. Returns when a scan / join / disconnect is
// asked for, the switch is cleared, the firmware stops answering, or AWDL has handed over nothing for kAwdlParkIdleMs.
void itlmt7925::awdlParkLoop()
{
    uint8_t ch = awdlParkOn;
    if (!awdlParkStart(ch)) {
        uint32_t t = nowMs() + 5000;                         // let the firmware settle before the next try
        awdlParkRetryMs = t ? t : 1;
        return;
    }
    uint32_t lastSeq = __atomic_load_n(&awdlFrameSeq, __ATOMIC_ACQUIRE), lastSeqMs = nowMs();
    for (;;) {
        m3PumpOnce();
        if (cmdPending(CMD_AWDL_SYNC)) {
            cmdTake(CMD_AWDL_SYNC, nullptr);
            awdlSyncTx();                                    // a window on the park channel (no ROC of its own)
            awdlParkWin++;
            awdlRocActive = 1;                               // the window ended; the radio is still parked
            awdlRocCh = ch;
        }
        uint32_t seq = __atomic_load_n(&awdlFrameSeq, __ATOMIC_ACQUIRE), now = nowMs();
        if (now - awdlParkPsfMs >= kAwdlParkPsfMs) { awdlParkPsfMs = now; awdlParkAf(ch, false); }
        if (now - awdlParkMifMs >= kAwdlParkMifMs) { awdlParkMifMs = now; awdlParkAf(ch, true); }
        awdlDataFlush();                                     // unicast / group data between windows too (gated: bit 1)
        if (seq != lastSeq) { lastSeq = seq; lastSeqMs = now; }
        if (cmdPending(CMD_SCAN | CMD_JOIN | CMD_DISCONNECT) || awdlParkOn != ch || recoverWanted ||
            now - lastSeqMs > kAwdlParkIdleMs)
            break;
        if (parkUptimeUs() + 300000 >= awdlParkRocEndUs) {   // renew the remain-on-channel before it runs out
            m3Roc(false, ch);
            if (awdlParkRoc(ch)) awdlParkRenew++;
            else { awdlParkRenewFail++; m3log("awdl park: renew failed (status %u)", rocStatus); break; }
        }
        IOSleep(1);
    }
    awdlParkStop();
}
