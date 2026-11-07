// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
#ifndef ITLMT7925_HPP
#define ITLMT7925_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>

#include "itl_caps.h"
#include "itl_ba.h"
#include "itl_native_bridge.h"

class itlmt7925Eth;
class IOUserClient;

// Per-frame options for m3Tx (everything defaults to the verified legacy behaviour: fixed 6 Mb/s, management queue)
struct M3TxOpt {
    bool protect = false;     // encrypt (Protected Frame bit; the hardware adds the CCMP header)
    bool rateAdapt = false;   // no FIXED_RATE: the firmware's rate control chooses the MCS
    uint8_t tid = 0;          // TXD1.TID: 0 normal, 2 = MT_TX_ADDBA for an ADDBA request
    uint8_t qidx = 0x10;      // TXD0.Q_IDX: 0x10 = MT_LMAC_ALTX0 (management / non-QoS), 0..3 = access categories
    int sn = -1;              // >= 0: sequence number chosen by the host (TXD3.SN_VALID)
    bool aggr = false;        // leave BA_DISABLE clear: the frame may be sent in an A-MPDU
    bool *full = nullptr;     // set: no staging slot / descriptor free, the caller keeps the frame (Ethernet output queue)
    uint8_t pid = 3;          // TXD5.PID echoed in the TX status (TXS) record: 3 host frames, 4/5 AWDL unicast/group data
    uint8_t omac = 0;         // TXD1.OWN_MAC: 0 the station address, 1 the AWDL address (awdlOwnMacApply)
};

struct itl_dma_buf {
    IOBufferMemoryDescriptor *md = nullptr;
    uint8_t *va = nullptr;
    uint64_t pa = 0;
    size_t size = 0;
};

struct itl_ring {
    itl_dma_buf desc;      // n * 16 bytes
    itl_dma_buf buf;       // tx: one 4 KiB staging buffer, rx: n * 2048
    uint32_t regs = 0;     // BAR0 offset of this queue's register block
    int n = 0;
    int head = 0;          // tx: next slot to write; rx: next slot to refill
    int tail = 0;          // rx: next slot to read
    uint16_t slotBuf[1536]; // rx: which buffer sits in which slot (MT7925_RX_RING_SIZE at most)
};

class itlmt7925 : public IOService {
    OSDeclareDefaultStructors(itlmt7925)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) override;
    // Ethernet interface glue (itlmt7925_eth.cpp): frames from the network stack / frames received from the AP
    // 0 sent, 1 no TX buffer free (the output queue keeps the frame until ethTxKick()), -1 dropped
    int  ethTx(const uint8_t *frame, uint32_t len);
    bool ethCreate();
    void ethRx(const uint8_t *frame, uint32_t len);
    void ethTxKick();                        // restart a stalled output queue
    void ethFlush();                         // deliver the received batch to the stack
    // Speed shown by macOS for the interface: the estimated best PHY rate when HT/VHT is in use, else the 54 Mb/s legacy value
    static void baDeliverCb(void *ctx, const uint8_t *frame, uint32_t len);
    uint64_t linkSpeedBps() const { return fastActive ? (uint64_t)fastRate * 1000000ull : 54000000ull; }
    // Runtime control from user space (itlmt7925_uc.cpp, ABI in itl_uc_abi.h): the worker thread picks the commands up.
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;
    IOReturn ucScan();
    IOReturn ucJoin(const char *ssid, const char *psk);
    IOReturn ucDisconnect();

private:
    bool nativeMode = false;
    IOLock *nativeLock = nullptr;
    ItlNativeSink nativeSink = {};
    IOPCIDevice *pci = nullptr;
    IOMemoryMap *bar0 = nullptr;
    volatile uint8_t *regs = nullptr;
    uint32_t barSize = 0;

    uint32_t rd(uint32_t off);
    void wr(uint32_t off, uint32_t val);
    uint32_t rdMapped(uint32_t addr);       // translates a few CONN addresses
    // M1 (gated by boot-arg itlmt_m1=<delay seconds>)
    uint32_t mapAddr(uint32_t addr, uint32_t *restoreL1);
    uint32_t rdA(uint32_t addr);
    void wrA(uint32_t addr, uint32_t val);
    void rmwA(uint32_t addr, uint32_t clr, uint32_t set);
    bool pollA(uint32_t addr, uint32_t mask, uint32_t val, uint32_t timeoutMs);
    void m1log(const char *fmt, ...);
    bool dmaAlloc(itl_dma_buf &b, size_t size, bool cached = false);
    void dmaFree(itl_dma_buf &b);
    bool ringInit(itl_ring &r, int qid, uint32_t base, int n, bool rx, bool cached = false);
    void ringFree(itl_ring &r);
    void dmaDisable();
    void dmaEnable();
    bool txKick(itl_ring &r, size_t len);
    int  mcuSend(uint8_t cid, const void *payload, size_t plen, bool wantResp, int *ret);
    int  fwScatter(const uint8_t *data, size_t len);
    int  rxWait(uint8_t seq, int *ret, uint32_t timeoutMs);
    int  initDownload(uint32_t addr, uint32_t len, uint32_t mode);
    int  loadPatch();
    int  loadRam();
    int  m1Run();
    void m1Main();
    static void m1Thread(void *arg);
    itl_ring txWm, txFwdl, rxEvt, rxData;
    // MT7927 (itlmt7927.cpp): same firmware interface as the MT7925, different host-side bring-up
    bool is7927 = false;                     // decided from the PCI device id in start()
    uint16_t pciDid = 0;
    itl_ring rxEvt2;                         // MT7927 only: RX ring 7 (second MCU ring, carries TXFREE / events)
    const uint8_t *fwPatchP = nullptr, *fwPatchE = nullptr, *fwRamP = nullptr, *fwRamE = nullptr;
    uint8_t vifBand = 0xff;                  // band_idx of our interface in DEV_INFO / BSS_INFO / scan (MT7925: 0xff, auto)
    uint8_t rocBand = 0xff;                  // dbdcband the firmware granted in the last ROC event
    uint8_t chBandIdx(uint8_t ch) const { return is7927 ? (ch && ch <= 14 ? 0 : 1) : 0xff; }
    void aspmDisable7927();
    void sema7927(uint32_t addr);
    int  wfsysReset7927();
    void dmaPrefetch7927();
    void wfdmaSetup7927();
    void wake7927();                         // re-apply GLO_CFG bits that a power-ownership round trip can clear
    int  devInfo(bool active);               // DEV_INFO_UPDATE (ACTIVE tlv) with the current vifBand
    int  bssBasicMin(bool active);           // BSS_INFO_UPDATE, BASIC tlv only (the interface-creation shape of M2)
    int  setDbdc(bool enable);               // MT7927: UNI SET_DBDC_PARMS, as mt7925_mcu_set_dbdc()
    int  reconfigBand(uint8_t ch);           // MT7927: move the interface to the band of channel ch (mt7927_reconfig_band)
    void rxEvt2Poll();
    // M2 (boot-arg itlmt_m2=1): UNI commands, interface setup, scan, beacon parsing
    bool m2On = false;
    int  uniSend(uint8_t cid, const void *payload, size_t plen, bool query, uint8_t *evOut, size_t evMax,
                 uint32_t *evLen, uint32_t timeoutMs, bool noWait = false);
    bool ringPeek(itl_ring &r, const uint8_t **data, uint32_t *len, int *bi);
    void ringRelease(itl_ring &r, int bi);
    void rxDataPoll();
    void rxParseFrame(const uint8_t *p, uint32_t len);
    void rxFrameDump(const uint8_t *p, uint32_t len);
    bool awdlRxData(const uint8_t *p, uint32_t len);    // AWDL data frame heard in an AWDL window (§10.33)
    void m2log(const char *fmt, ...);
    void m2State(const char *tag);
    uint32_t fwLogCnt = 0;
    void m2Publish();
    int  m2Run();
    int  m2Scan(const char *ssid);
    uint8_t scanSeq = 0;
    uint32_t scanMs = 0, evLogged = 0;
    uint8_t chHist[200] = {0};
    char m2buf[2048] = {0};
    size_t m2len = 0;
    char m2res[400] = {0};
    uint8_t mac[6] = {0};
    bool scanDone = false;
    uint32_t rxPkts = 0, rxBeacons = 0, rxOther = 0, rxDump = 0;
    uint32_t evCount = 0;
    // sec: ITL_SEC_* (itlmt7925_uc.cpp); rssi in dBm, 0 = not measured; seenMs = nowMs() of the last beacon / probe response
    // cap / bcnInt / ie / probe: the frame as heard (native Wi-Fi hands them to the Family as the scan result's beacon message)
    struct itl_ap { uint8_t bssid[6]; uint8_t ch; char ssid[33]; int8_t rssi; uint8_t sec; uint32_t seenMs; uint32_t seenGen;
                    uint8_t probe; uint16_t cap, bcnInt, ieLen; uint8_t ie[ITL_NATIVE_IE_MAX]; } aps[ITL_NATIVE_AP_MAX];
    int nAps = 0;
    uint32_t apGen = 0;                      // bumped per scan: an entry with seenGen != apGen was not heard in the current scan
    uint8_t mcuSeq = 0;
    // M3 (boot-arg itlmt_m3=1, implies M2): associate to the configured network, WPA2-PSK, Ethernet interface
    bool m3On = false;
    itl_ring txData;                         // band0 data TX ring (mgmt + data frames)
    void m3log(const char *fmt, ...);
    void m3Publish(const char *stage);
    int  m3Run();
    void m3Capture(const uint8_t *frame, uint32_t len, const char *ssid, uint8_t ch);
    int  m3Cmd(uint8_t cid, const void *payload, size_t plen, const char *name, bool noWait = false);
    int  m3HwPrep();
    void ctlPoll();                          // link loop: take run-time control requests (setProperties)
    enum M3BssOp { M3_BSS_ADD, M3_BSS_ASSOC, M3_BSS_REMOVE };
    int  m3Bss(M3BssOp op);
    uint8_t awdlOmac1 = 1;                   // itl_awdlomac: 1 = AWDL data goes out with TXD1.OWN_MAC = 1 (the AWDL address, once programmed), 0 = OMAC 0
    // TX status records (PKT_TYPE_TXS, docs/ROADMAP-AWDL.md §10.53), by PID: [0] host frames (3), [1] AWDL unicast data (4),
    // [2] AWDL group data (5), [3] other. ok = TXS0 ACK_ERROR_MASK clear; err[] by which of its bits was set (queue / RTS / ACK).
    // Indexed by PID & 7: 3 host frames, 4 AWDL unicast to a peer not heard in this window, 5 AWDL group, 6 AWDL unicast to a
    // peer heard in this window. late = a TXS for AWDL data that arrived after its window had ended (sent on the wrong channel).
    uint32_t txsOk[8] = {}, txsFail[8] = {}, txsErr[3] = {}, txsN = 0, txsLogged = 0, txsPkts = 0, txsLate = 0;
    uint32_t txsUcBand[2][2] = {};
    // AWDL data results from TXFREE (data frames get no TXS record on this chip; mt7925_mac_tx_free reads the header word's
    // STAT (29:28, non-zero = failed) and COUNT (27:24, attempts) for the MSDUs that follow it). By PID 4..6 (index - 4).
    uint32_t tfOk[3] = {}, tfFail[3] = {}, tfTries = 0, tfLate = 0, tfBand[2][2] = {}, tfLogged = 0;               // AWDL unicast TXS by band (0 = 2.4, 1 = 5 GHz) and result (0 ok, 1 fail)
    uint32_t awdlDataInFlight = 0;               // AWDL data frames handed to the hardware whose TXS has not come back
    uint32_t awdlWinSeq = 0;                     // granted AWDL windows so far (a peer "heard in this window" carries this value)
    uint32_t awdlTxGuardHeld = 0, awdlDrainWaitMs = 0, awdlDrainTimeout = 0;
    struct AwdlPeerSeen { uint8_t mac[6]; uint8_t ch; uint32_t win; };
    AwdlPeerSeen awdlPeers[4] = {};
    void awdlPeerHeard(const uint8_t *sa);
    bool awdlPeerInWindow(const uint8_t *da) const;
    uint32_t awdlDataRxUc = 0, awdlDataRxUcMine = 0;
    // Any received buffer with addr1 = the AWDL address, by 802.11 type (mgmt / ctrl / data) and by whether an AWDL window was
    // open, before any parser looks at it: does the chip hand unicast to awdl0 to the host at all (MT7927 test 2: rxuc=0)
    uint32_t awdlToOwn[2][3] = {};
    // Inside ROC windows, with the 802.11 header at rxHdrOff() + the sniffer delta learned from AWDL data frames (the AWDL
    // BSSID gives the true offset): frames whose addr2 is a heard AWDL peer, and unicast data from anyone else (the neighbours'
    // traffic, as a control for "does the sniffer hand up unicast data at all"). [0] peer uc data, [1] peer uc mgmt,
    // [2] peer group data, [3] other uc data, [4] candidates rejected
    int16_t awdlSnifDelta = -1;
    uint32_t awdlUcSeen[5] = {};
    uint8_t awdlUcLogged = 0;
    uint8_t awdlToOwnLogged = 0;   // AWDL data heard with a unicast DA / with DA = our AWDL address
    void m3Txs(const uint8_t *p, uint32_t len);
    uint8_t grpWcid19 = 0;                   // itl_grp19: 0 = group TX on the AP wcid (the proven path), 1 = the mt76 BSS wcid 19 + its STA_REC records
    int  m3Clc(const char *cc, uint8_t env);   // CLC power tables from the RAM firmware for country cc
    int  clcSend(const uint8_t *clc, const char *cc, uint8_t env, uint8_t idx);
    int  m3TxPower();                        // per-channel rate power limits (SAR from ACPI MTDS / MTGS)
    int  efuseRead(uint32_t off, uint8_t *val);
    void sarInit();
    int  powerCmd(const void *payload, size_t plen, const char *name);
    int  m3Edca();                           // per-AC contention parameters from the AP's WMM element (after every BSS_INFO)
    int  m3Sta(uint8_t state, bool newly, uint16_t aid);
    int  m3StaBss();                         // STA_REC_UPDATE for the BSS wcid (19): what mt76 sends on BSS_CHANGED_ASSOC
    // reqtype: 0 JOIN (association, the default), 1 ROC (a timed remain-on-channel, what an AWDL window is); durMs = maxinterval
    int  m3Roc(bool acquire, int channel = -1, uint8_t reqtype = 0, uint32_t durMs = 5000);
    // AWDL PSF sync frame handoff (docs/ROADMAP-AWDL.md phase 3): awdlSubmit() copies one frame from the native interface's
    // setAWDL_SYNC_FRAME_TEMPLATE into this mailbox and awdlSyncTx() runs on the link thread, pushing it out during a ROC
    // window on the AWDL channel (the firmware has no AWDL channel sequencing of its own).
    int  awdlSubmit(const uint8_t *frame, uint32_t len, uint8_t channel, uint32_t dwellMs, uint64_t awBaseUs = 0,
                    uint16_t awBaseSeq = 0, uint32_t awPeriodUs = 0, uint16_t awFlags = 0, const uint8_t *frame2 = nullptr,
                    uint32_t len2 = 0, uint64_t startUs = 0);
    struct AwdlTimeline { uint64_t baseUs; uint16_t baseSeq; uint16_t flags; uint32_t periodUs; };
    bool awdlStampSync(uint8_t *frame, uint32_t len, const AwdlTimeline &tl);   // the sync TLV at the real TX time (§10.28)
    void awdlSyncTx();
    int  awdlRxFetch(uint8_t *out, uint32_t cap, uint32_t *outLen, uint32_t *outCh, int8_t *outRssi, uint32_t *outUs);
    void m3RxFilter(uint32_t fif);
    void m3Sniffer(bool on, uint8_t ch);
    void m3SnifferEnable(bool on);
    void m3SnifferConfig(uint8_t ch);
    uint32_t awdlRocFrames = 0, awdlBcn = 0;      // frames / beacons heard while the radio was parked on the social channel
    uint32_t awdlRocOnCh = 0;                     // of those, the ones whose RXD channel really is the parked channel
    uint32_t awdlRxFc[5] = {};                    // heard during a ROC window: beacons / probe / action / other mgmt / data
    uint32_t awdlRxDumped = 0;                    // trace budget for the RX descriptor dump (verifies the group layout)
    // Forensics counters (docs/ROADMAP-AWDL.md §10.22): what the firmware actually hands the host while the radio is parked on
    // the social channel. awdlOuiHits counts buffers that carry Apple's OUI anywhere, awdlSigHits the ones that carry the AWDL
    // action header (7f 00 17 f2 08) and awdlBssidHits the ones that carry AWDL's BSSID (00:25:00:ff:94:73). Together they
    // answer "does the firmware deliver AWDL at all, and where in the buffer does it sit" without guessing an offset.
    uint32_t awdlOuiHits = 0, awdlSigHits = 0, awdlBssidHits = 0;
    uint32_t awdlNearMiss = 0;                    // fc == 0xd0 frames whose action header is not AWDL's
    uint32_t awdlForensics = 0;                   // trace budget for the buffer forensics dump
    uint8_t awdlQuiet = 0;                        // suppress the per-command ROC/sniffer logging (it evicts the diagnostics)
    volatile uint8_t awdlRocActive = 0;           // set while a ROC window is open (see awdlSyncTx / rxParseFrame)
    volatile uint8_t awdlRocCh = 0;               // channel that window is parked on (attribution in the AF log)
    uint8_t awdlRxFilterDone = 0;                 // the "pass other BSS" RX filter is programmed once per link
    // 1600 bytes: the PSF with the driver-built sync TLV and the master indication frame with its service TLVs do not fit in
    // 256 (docs/ROADMAP-AWDL.md §10.29). awdlFrame2 is the MIF sent right after the PSF in the same window (0 = none).
    uint8_t awdlFrame[1600] = {};
    volatile uint32_t awdlFrameLen = 0;
    uint8_t awdlFrame2[1600] = {};
    volatile uint32_t awdlFrame2Len = 0;
    uint8_t awdlTxBuf[1600] = {};                 // link-thread snapshots of the two frames (too big for the kernel stack)
    uint8_t awdlTxBuf2[1600] = {};
    uint32_t awdlTx2Try = 0, awdlTx2Ok = 0;       // second frame (MIF) transmissions
    uint32_t awdlAfSeq = 0;                                  // seq_ctrl counter of the AWDL action frames this driver sends
    volatile uint32_t awdlFrameSeq = 0, awdlFrameDone = 0;
    uint8_t awdlChannel = 6;
    uint64_t awdlAwBaseUs = 0;                    // AWDL timeline from the interface (ItlNativeAwdlSync v2): window awdlAwBaseSeq
    uint16_t awdlAwBaseSeq = 0;                   // starts at host uptime awdlAwBaseUs and windows are awdlAwPeriodUs long
    uint32_t awdlAwPeriodUs = 0;                  // 0 = no timeline (v1 submission): the frame goes out as handed over
    uint16_t awdlAwFlags = 0;                     // ItlNativeAwdlSync.flags
    uint32_t awdlStamped = 0, awdlStampMiss = 0;  // frames whose sync TLV was stamped / frames without one
    uint32_t awdlDwellMs = 60;
    // v5 window start (docs/ROADMAP-AWDL.md §10.56): host uptime (us) at which the window's AW begins, 0 = "now". The core
    // begins the setup (AP null, filter, sniffer, ROC) awdlLeadUs before it - the measured setup-to-grant time - so the radio
    // is on the channel when the AW opens instead of 10-40 ms into it.
    uint64_t awdlStartUs = 0;
    uint32_t awdlLeadUs = 30000, awdlLeadEarly = 0, awdlLeadLateN = 0, awdlLeadStale = 0, awdlLeadWaitMs = 0;
    uint64_t awdlLeadLateUs = 0;
    uint32_t awdlOnChWin = 0;
    uint64_t awdlPhUs[9] = {};                    // per-phase time of ROC windows (§10.59)
    uint32_t awdlPhN = 0;                     // windows run on the AP's own channel: no ROC, no AP cost (§10.58)
    // §10.61: one ROC held across consecutive windows on the same channel (one firmware channel switch for several AWs)
    uint32_t awdlChainMs = 0;                     // itl_awdlchain: extra off-channel time a held ROC may add (0 = off; §10.63: off by default)
    uint32_t awdlHoldLog = 0;
    uint8_t awdlHeldCh = 0;                       // channel of the ROC kept open for the next window (0 = none)
    uint32_t awdlHeldOffT0 = 0;                   // nowMs() when the held absence began (duty bookkeeping)
    uint64_t awdlHeldUntilUs = 0, awdlHeldRocEndUs = 0;
    uint32_t awdlChainN = 0, awdlChainBreak = 0, awdlChainTimeout = 0;
    void awdlHeldClose();
    uint8_t awdlLeadOn = 1;                       // itl_awdllead: 0 = start the setup when the window is due (before §10.56)
    uint32_t awdlTxTry = 0, awdlTxOk = 0, awdlTxRocFail = 0;
    uint32_t awdlListenWin = 0;                   // v4 listen-only windows (docs/ROADMAP-AWDL.md §10.32)
    // Phase 3 RX probe: AWDL action frames heard from other devices during the ROC window (see rxParseFrame). The bytes give
    // the exact 802.11 wrapper Apple's driver builds around the TLV blob the Family hands over, which is what the transmit
    // path still has to reproduce.
    uint32_t awdlAfSeen = 0, awdlAfDumped = 0;
    // AWDL action frames heard on the social channel, waiting for the AWDL interface to hand them to the peer manager
    // (phase 3 receive half). Single producer (the RX path) and single consumer (the interface's work loop), so the two
    // indices are the only shared state.
    // 1600 bytes: a master indication frame (MIF) carries the service TLVs and is 600-1000+ bytes on the air; 512 cut every
    // MIF short and the peer manager dropped it as a truncated TLV (docs/ROADMAP-AWDL.md §10.27).
    struct AwdlRxSlot { uint32_t len; uint32_t ch; int8_t rssi; uint32_t tUs; uint8_t data[1600]; };   // rssi 0 = unknown
    // 16 slots (§10.33): AWDL data frames share the ring with the action frames, and a TCP exchange arrives in bursts.
    static const uint32_t kAwdlRxSlots = 16;
    AwdlRxSlot awdlRxRing[kAwdlRxSlots] = {};
    uint32_t awdlRxRingFull = 0, awdlDataRx = 0;
    // AWDL data TX (docs/ROADMAP-AWDL.md §10.33): complete 802.11 AWDL data frames from the AWDL interface, sent by the link
    // thread inside the next AWDL radio window (listen or PSF), when the radio is on a peer's channel.
    struct AwdlTxSlot { uint32_t len; uint32_t ms; uint8_t data[1600]; };   // ms: when queued (itl_awdlonech bit 1)
    static const uint32_t kAwdlTxSlots = 16;
    AwdlTxSlot awdlDataTxRing[kAwdlTxSlots] = {};
    volatile uint32_t awdlDataTxHead = 0, awdlDataTxTail = 0;
    uint32_t awdlDataTxQueued = 0, awdlDataTxSent = 0, awdlDataTxFail = 0, awdlDataTxFull = 0;
    // Uptime (ms) of the last AWDL radio window; 0 = none yet. A full ring with no window for a second is not backpressure
    // but "AWDL is not on the air": the frame is refused as stale so the stack drops it instead of retrying (§10.36).
    uint32_t awdlLastWinMs = 0, awdlDataTxStale = 0;
    uint32_t awdlDataRxHdr[3] = {};              // where the AWDL data header sat: 0 rxHdrOff(), 1 elsewhere (sniffer RXV), 2 not 802.11 data
    int  awdlDataQueue(const uint8_t *frame, uint32_t len);
    void awdlDataFlush();
    void awdlDwell(uint32_t dwellMs);
    volatile uint32_t awdlRxHead = 0, awdlRxTail = 0;
    void m3Pump(uint32_t ms);
    void m3Event(const uint8_t *ev, uint32_t len);
    void m3ParseAp();
    volatile bool rocGrant = false;
    volatile bool rocWant = false;                  // a ROC grant is expected (set by m3Roc); the event is parsed after link up too
    uint8_t rocTok = 0, rocStatus = 0xff;
    uint32_t rocEvIgnored = 0;                   // ROC events that were not the grant of the request in flight (§10.52)
    uint8_t m3cipher = 0;                    // 0 = open, 4 = CCMP (BSS_INFO sec TLV)
    uint8_t apSupp = 0, apBasic = 0;         // OFDM rate bitmaps (6,9,12,18,24,36,48,54 -> bit 0..7)
    uint8_t apCck = 0, apBasicCck = 0;       // 2.4 GHz CCK rate bitmaps (1,2,5.5,11 -> bit 0..3)
    uint8_t apRsn[64] = {0};
    uint8_t apRsnLen = 0;
    uint8_t apGroupCipher = 0;               // RSN suite type: 1 WEP40, 2 TKIP, 4 CCMP, 5 WEP104 (0 unknown)
    uint8_t apPairCipher = 0;                // what the 4-way handshake will use: CCMP when offered, else TKIP
    char m3buf[8000] = {0};
    size_t m3len = 0;
    char m3res[400] = {0};
    char m3ssid[33] = {0};
    char m3psk[72] = {0};
    // step 3: authentication / association over the data TX ring
    uint8_t m3pmk[32] = {0};
    uint8_t m3wantFc = 0;                    // fc byte 0 of the management frame we are waiting for (0 = none)
    uint8_t m3rx[600] = {0};
    uint32_t m3rxLen = 0;
    int m3txSlot = 0;
    uint16_t m3tok = 0, m3aid = 0;
    uint32_t m3rxOther = 0, m3fwLogged = 0;
    bool m3hw = false;
    int8_t sarPwr[11];                       // ACPI dynamic SAR limit per mt76 SAR range (0.5 dBm), 127 none; read in m3HwPrep
    uint8_t hwEncap = 0;                     // efuse 0xa71 bits 1:0: selects the CLC tables meant for this module
    char clcCc[3] = {0};                     // country the CLC tables were last sent for
    // step 4: association complete -> 4-way handshake, keys, Ethernet interface, data path
    bool m3dataOn = false;                   // data frames from the AP are handled (set after association)
    bool m3wantEapol = false;
    bool m3hsApLeave = false;
    uint16_t m3hsApReason = 0;
    uint8_t m3eapol[420] = {0};
    uint32_t m3eapolLen = 0;
    // RX polling may deliver several EAPOL packets before the handshake gets CPU time.
    // Only the core worker produces/consumes this queue; no network-stack callback accesses it.
    static const uint32_t kEapolQueue = 8;
    struct EapolRx { uint32_t len; uint8_t data[420]; };
    EapolRx m3eapolQueue[kEapolQueue] = {};
    uint32_t m3eapolHead = 0, m3eapolTail = 0, m3eapolDrops = 0;
    bool m3EapolNext();
    uint8_t m3replay[8] = {0};               // the last EAPOL-Key replay counter accepted from the AP (M3, then group rekeys)
    uint32_t m3gtkRekey = 0, m3gtkRekeyBad = 0, m3M4Again = 0;
    void m3EapolOnLink();
    uint8_t m3ptk[48] = {0};                 // KCK(16) KEK(16) TK(16)
    bool m3sleepy = false;                   // m3Pump sleeps instead of spinning once the link is up
    volatile bool m3linkUp = false;
    itlmt7925Eth *eth = nullptr;
    IOLock *txLock = nullptr;
    uint32_t stRxData = 0, stRxEapol = 0, stRxEth = 0, stRxDrop = 0, stTx = 0, stTxFail = 0, stNote = 0, stGrpTx = 0, stEcho = 0;
    int  m3Handshake();
    bool m3Eapol(const uint8_t *eapol, uint32_t len);
    int  m3Key(uint16_t wcid, bool pairwise, uint8_t keyId, const uint8_t *key, uint8_t keyLen, uint8_t cipher);
    bool m3RxData(const uint8_t *p, uint32_t len);
    bool m3TxData(const uint8_t *da, uint16_t ethertype, const uint8_t *payload, uint32_t plen, bool protect,
                  bool *full = nullptr);
    void m3Note(const char *dir, const uint8_t *eth, uint32_t len);
    // Post-mortem: the last Ethernet frames in either direction and one line of chip / ring / PCIe state, published as the
    // properties "wifi-frames" and "wifi-postmortem" when the firmware stops answering. chipRecover() redoes M1 + M2.
    struct FrameRec { uint32_t ms; uint16_t len, et, a, b; uint8_t dir, proto; };
    static const int kFrRing = 24;
    FrameRec frRing[kFrRing] = {};
    uint32_t frHead = 0;
    bool recoverWanted = false;
    // The AWDL interface's own address as a second OMAC (DEV_INFO_UPDATE omac 1), so the MAC hardware ACKs unicast frames
    // to awdl0: without it every unicast from a peer is retried 7 times and still counted lost (docs/ROADMAP-AWDL.md §10.38).
    uint8_t awdlOwnAddr[6] = {};
    bool awdlOwnSent = false;
    int awdlOwnRc = 1;                            // 1 = not sent yet
    void awdlOwnMacApply();
    // MT7927 (DBDC: 2.4 GHz = band 0, 5 GHz = band 1): the AWDL window's band. The MT7925 has one band and keeps band 0 / auto.
    // itl_awdlband=0 restores the single-band behaviour at run time.
    uint8_t awdlBandFix = 1;                      // itl_awdlband
    // itl_awdlown=0 (MT7927, A/B for test 3: no unicast to the AWDL address ever reached the host): OMAC 1 is taken off every
    // band, so frames to awdl0 are no longer "to me" for the RMAC and come in (unACKed) through the sniffer like other-BSS
    // frames; AWDL data then goes out with OWN_MAC 0. 1 (default) = OMAC 1 registered on the window's band.
    uint8_t awdlOwnOn = 1;
    uint8_t awdlCurBand = 0;                      // band_idx of the current AWDL window's channel (MT7927)
    uint8_t awdlOwnBand = 0xff;                   // band OMAC 1 is registered on (0xff = auto, as on the MT7925; 0xfe = nowhere)
    uint32_t awdlOwnMoves = 0, awdlOwnMoveFail = 0;
    uint32_t awdlGrantBand[2][3] = {};            // [window band][band the firmware granted: 0, 1, other]
    uint8_t awdlHdrBand() const { return (is7927 && awdlBandFix) ? awdlCurBand : 0; }
    // itl_awdlel (itlmt7925_awdl_el.cpp, §10.65): election TLVs on the air as Owly writes them - the master's own metric and
    // counter, our metric itl_awdlelm (60). 0 (default) = the Family's TLVs unchanged.
    struct AwdlElPeer { uint8_t addr[6]; uint32_t metric, counter; uint64_t ms; };
    static constexpr uint32_t kAwdlElPeers = 4;
    AwdlElPeer awdlElPeer[kAwdlElPeers] = {};
    uint8_t awdlElOn = 0;
    uint32_t awdlElMetric = 60;
    uint32_t awdlElFixed = 0, awdlElSelf = 0, awdlElStale = 0, awdlElNo24 = 0, awdlElLearned = 0;
    uint8_t awdlElTxMaster[6] = {};
    uint32_t awdlElTxMetric = 0, awdlElTxCounter = 0, awdlElTxOwn = 0;
    void awdlElLearn(const uint8_t *f, uint32_t len);
    bool awdlElFix(uint8_t *f, uint32_t len);
    void awdlElStat();
    void awdlElRxFix(uint8_t *f, uint32_t len);
    static constexpr uint32_t kAwdlSnapKinds = 8;     // our psf/mif, peer psf/mif, our uc/grp data, peer grp/uc data
    struct AwdlSnapBuf { volatile uint8_t busy, dirty; uint32_t len; uint64_t ms; uint8_t data[1100]; };
    AwdlSnapBuf awdlSnapBuf[kAwdlSnapKinds] = {};
    void awdlSnap(uint32_t kind, const uint8_t *f, uint32_t len);
    void awdlSnapPublish();
    uint8_t awdlTlvOn = 1;                            // itl_awdltlv: add TLVs 5 and 18 when the Family's frame lacks them
    uint32_t awdlTlvAdded = 0, awdlTlvNo4 = 0, awdlTlvNoRoom = 0;
    uint32_t awdlAddTlvs(uint8_t *f, uint32_t len, uint32_t cap);
    // itl_awdlafall (§10.65): our last PSF/MIF are also sent at the start of every listen window (the windows that follow the
    // iPhone, where our data frames do reach it), not only in the sync windows. 0 (default) = as before.
    uint8_t awdlAfAll = 0;
    uint8_t awdlAfKeep[1600] = {}, awdlAfKeep2[1600] = {};
    uint32_t awdlAfKeepLen = 0, awdlAfKeepLen2 = 0, awdlAfAllTx = 0, awdlAfAllOk = 0;
    static void awdlAfSetCh(uint8_t *f, uint32_t len, uint8_t ch);
    uint8_t awdlApCh = 0;                             // itl_awdlapch: AWDL windows on the AP's channel even when it is DFS (§10.66)
    uint32_t awdlApChWin = 0;
    void awdlApChPublish();
    uint32_t awdlElRxFixed = 0;
    // itl_awdlonech (§10.67, Owly's "shared mode"): AWDL never leaves the AP's channel. Bit 0: every window is moved to the AP's
    // channel (no ROC, the AP keeps being served) and our channel sequence names only that channel, in the slots we serve.
    // Bit 1: unicast to a peer waits for a window in which that peer has been heard (at most kAwdlUcHoldMs).
    static const uint32_t kAwdlUcHoldMs = 400;
    uint8_t awdlOneCh = 0;
    uint32_t awdlOneChRemap = 0, awdlOneChSeq = 0, awdlOneChSeqFail = 0, awdlUcAged = 0, awdlUcGated = 0;
    void awdlSeqOneCh(uint8_t *f, uint32_t len);
    int awdlOwnDevInfo(bool active, uint8_t band);
    // The last MCU commands (name, rc, uptime ms), dumped when the firmware stops answering (docs/ROADMAP-AWDL.md §10.42).
    struct McuHist { const char *name; int rc; uint32_t ms; };
    McuHist mcuHist[24] = {};
    uint32_t mcuHistN = 0;
    void mcuHistDump();
    // AWDL ROC grant latency: <10 / <40 / <80 ms, late (80-300 ms: after the window budget), never granted.
    uint32_t awdlRocLat[5] = {};
    // Per band (0 = 2.4 GHz, 1 = 5 GHz): AWDL action frames heard, windows granted, windows never granted. And windows not
    // opened because their channel is a DFS one (docs/ROADMAP-AWDL.md §10.49).
    uint32_t awdlAfBand[2] = {}, awdlWinBand[2] = {}, awdlNeverBand[2] = {}, awdlDfsSkip = 0;
    uint8_t awdlDfsLastCh = 0;
    uint32_t recoverFailMs = 0;                  // when the last chip recovery failed (0 = none pending): retried while idle
    uint32_t awdlBackoffUntilMs = 0;             // no AWDL window before this uptime (after a ROC that was never granted)
    uint32_t awdlNextWinMs = 0;                  // no AWDL window before this uptime: the AP channel gets at least as long as the last absence
    uint32_t awdlOffMs = 0, awdlOffN = 0;        // measured time off the AP channel (ROC request to the abort returning), windows
    uint32_t awdlDutySkip = 0;                   // windows not taken because the AP channel had not had its share yet
    uint32_t awdlPsTx[2] = {};                   // null-data frames to the AP: PM=1 before a window, PM=0 after it
    void awdlApPs(bool doze);
    uint32_t m3CmdTimeouts = 0;                   // consecutive MCU command timeouts (2 = the firmware is gone)
    uint32_t recoverCnt = 0;
    void frameNote(char dir, const uint8_t *eth, uint32_t len);
    void m3Postmortem(const char *tag);
    bool chipRecover();
    void m3Link();
    void nvramStat(const char *msg);
    bool m3Tx(const uint8_t *frame, uint32_t len, uint8_t type, uint8_t subtype, bool protect = false)
    {
        M3TxOpt o;
        o.protect = protect;
        return m3Tx(frame, len, type, subtype, o);
    }
    bool m3Tx(const uint8_t *frame, uint32_t len, uint8_t type, uint8_t subtype, const M3TxOpt &o);
    int  m3Run1();                           // one connection attempt (scan .. handshake .. link); m3Run() adds the fallback ladder
    int  m3PumpOnce();                       // service the rx rings once; non-zero if anything arrived
    void m3Teardown();                       // leave the AP (deauthentication, STA / BSS records) before connecting again
    bool m3RxCtl(const uint8_t *p, uint32_t len);   // Block Ack action frames and BlockAckReq from the AP
    bool m3RxMgmt(const uint8_t *p, uint32_t len);
    bool m3Xchg(const uint8_t *frame, uint32_t len, uint8_t subtype, uint8_t wantFc, uint32_t waitMs);
    int  m3Auth();
    int  m3Assoc();
    // HT/VHT (boot-arg itlmt_fast=1): advertise 11n/11ac in the association, give the firmware the AP's capabilities and let
    // its rate control choose MCS for unicast data instead of the fixed 6 Mb/s. Only used when the AP is on 5 GHz and has HT.
    bool m3fast = false;                     // requested by the boot-arg
    bool fastActive = false;                 // requested and the target AP supports it
    itl_ap_caps apCaps = {};
    uint8_t fastBw = 0, fastCenter = 0, fastNss = 1;   // operating width (CMD_CBW_*), center channel, usable streams
    uint32_t fastRate = 54;                  // estimated best PHY rate, Mb/s
    bool fastHe = false;                     // level 6 and the AP has HE: advertise and use 802.11ax
    bool fastAmsdu = false;                  // level 4: accept A-MSDU in the Block Ack agreement and unpack it
    bool fastAllow160 = false;               // level 5 and the BSS is 160 MHz wide: advertise 160 MHz
    void m3FastPlan();
    uint8_t fastSig(uint8_t level);          // features at `level` for the current AP, to skip levels that change nothing
    void m3Hex(char *out, size_t outLen, const uint8_t *p, uint32_t n);

    // ---- Block Ack, TX buffer tracking and the fallback ladder (itlmt7925_ba.cpp) ----
    // itlmt_fast=<level>: 1 HT/VHT rates, 2 + receive Block Ack (reorder buffer), 3 + transmit Block Ack, QoS access-category
    // queue, host-assigned sequence numbers and TXFREE-tracked buffers, 4 + A-MSDU receive, 5 + 160 MHz channels, 6 + HE (802.11ax).
    // If a level does not work (association or handshake fails, or no unicast frame comes back from the AP after the link is
    // up, or A-MSDUs cannot be unpacked, or the firmware reports nearly every transmission failed) the driver connects again
    // one level lower, skipping levels that would change nothing for this AP.
    uint8_t fastLevel = 0;                   // requested by the boot-arg
    uint8_t lvl = 0;                         // level of the current attempt, 0 = legacy 6 Mb/s
    uint8_t lvlCap = 255;                    // highest level not given up on the current network (reset when the network changes)
    char capSsid[33] = {0};                  // the network lvlCap belongs to: kept across disconnects and failed joins, so that a
    uint32_t capMs = 0;                      // retry from the user starts at the level that worked, not at itlmt_fast again (10 min)
    uint8_t capBand = 0;                     // band the cap was learned on (1 = 2.4 GHz, 2 = 5 GHz): another band starts over
    bool m3stepDown = false;                 // set when the attempt at `lvl` failed; m3Run() then retries one level lower
    bool m3stepRadio = false;                // ... because of radio conditions (tx failing), not because the AP rejects the level
    bool m3reachedAssoc = false;
    struct RxBaSess { itl_reorder r; itl_ba_slot *mem; uint32_t memBytes; };
    RxBaSess rxBa[8] = {};                      // receive sessions by TID (the AP is the originator)
    itl_ba_action baQ[6];                    // Block Ack action frames received, handled by baService() outside the rx path
    uint8_t baQn = 0;
    enum { TXBA_IDLE, TXBA_REQ, TXBA_ON, TXBA_GAVEUP };
    uint8_t txBaState = TXBA_IDLE, txBaToken = 0, txBaTries = 0;
    uint16_t txBaWin = 0;
    uint32_t txBaSentMs = 0, txBaNextMs = 0;
    volatile uint16_t txSn = 0;              // next sequence number of TID 0 (level 3)
    bool txPool = false;                     // level 3: TX staging buffers are released by TXFREE, not reused blindly
    bool txFreeSeen = false;
    // Staging slots of txData. One slot per ring descriptor, and well above the 64-frame Block Ack window: with 64 slots
    // every aggregate in flight held the whole pool and any retransmission produced "no slot" drops (13k of 316k frames
    // measured). The token is generation(5) | slot(8) = 13 bits, the same range the Linux driver uses for msdu ids.
    static const uint32_t kTxSlots = 256;
    uint64_t txBusy[kTxSlots / 64] = {0};    // staging slots in flight
    uint32_t txSlotT[kTxSlots] = {0};
    uint8_t txGen[kTxSlots] = {0};
    uint8_t txPid[kTxSlots] = {0};           // TXD5 PID of the frame in each staging slot (AWDL data: 4..6, read back by TXFREE)
    uint8_t txNext = 0;
    bool txIsBusy(uint32_t s) const { return (txBusy[s >> 6] >> (s & 63)) & 1; }
    void txSetBusy(uint32_t s) { txBusy[s >> 6] |= 1ull << (s & 63); }
    void txClrBusy(uint32_t s) { txBusy[s >> 6] &= ~(1ull << (s & 63)); }
    void txClrAll() { for (uint32_t i = 0; i < kTxSlots / 64; i++) txBusy[i] = 0; }
    uint32_t stTxUni = 0, stRxUni = 0, stRxAmsdu = 0, stTxFree = 0, stTxReclaim = 0, stTxNoSlot = 0, stTxFwFail = 0, stTxFwRetry = 0;
    uint32_t stBaReqRx = 0, stBaDecl = 0, stBaRxOn = 0, stBaTxOn = 0, stBaDel = 0, stBaBar = 0;
    uint32_t linkUpMs = 0, lastReorderMs = 0;
    // How long a frame waits in a Block Ack reorder buffer for a missing earlier one (boot-arg itlmt_reo=<ms>). mac80211 uses
    // 100 ms; measured here: 1258 timeouts in 536 s at 100 ms = 23 % of the time with the receive path held, while a hole is
    // either filled by the AP's retry within a few ms or never. 30 ms keeps the retries and cuts the stalls.
    uint32_t reoMs = 30;
    // Boot-arg itlmt_no160=1: never run a 160 MHz channel (level 5/6 stay 80 MHz; HE keeps working). A/B for 160 MHz links where
    // the AP re-sends ~9 % of the frames already received (its Block Acks from us do not arrive) and holes never fill.
    bool no160 = false;
    // Boot-arg itlmt_nopwr=1: no CLC / SAR power tables at join (the firmware's own defaults, as the benchmarks before a45e8c1
    // ran); the tables can then be sent at run time (ctlPwr) for a before/after comparison in one boot.
    bool noPwr = false;
    // A-MSDU receive stays off by default (levels 4..6 run without it): the round 26 matrix gave 5.6 MB/s at level 3 and 25-60 KB/s
    // at every level with A-MSDU in the Block Ack agreement, same AP / radio / tables. Boot-arg itlmt_amsdu=1 or itl_amsdu=1
    // (run time, with the next reconnect) turn it back on for the diagnosis.
    bool noAmsdu = true;
    uint32_t stAmsduAi[4] = {}, stAmsduN[5] = {}, stBundleOvf = 0;   // RXD4 payload format seen, subframes per packet (0..3, 4+), bundle full
    uint8_t clcOwnFw = 1;                   // itl_clcown: CLC power rules from the running RAM image (1) or the MT7925 one (0, old)
    uint8_t pwrCeil = 40;                   // per-rate ceiling of the SKU tables, 0.5 dBm (m3TxPower)
    uint32_t statSecs = 0;                   // boot-arg itlmt_stat=<s>: stat-log period (default 5 s for 5 min, then 60 s)
    // Run-time control (IORegistryEntrySetCFProperties, administrator only; tools/itlctl.c). Taken by the link loop, so an A/B
    // does not need a reboot: itl_lvl = reconnect at that level, itl_no160, itl_reo = reorder timeout ms, itl_pwr = send the
    // power tables now with that per-rate ceiling (0.5 dBm).
    volatile uint32_t ctlLvl = 0, ctlPwr = 0;
    uint8_t lvlForce = 0;                    // level the next reconnect uses (set from ctlLvl), 0 = the ladder's own choice
    int  m3PowerSetup();                     // efuse hw type + CLC + SKU tables; best effort, 0 when the link may go on
    IOReturn setProperties(OSObject *props) override;
    // Where do the large frames go: classes of the Ethernet frame length (<128, <512, <1024, <1280, <1400, >=1400) at the RX ring,
    // after A-MSDU / header parsing, and when handed to the stack; big ICMP echoes (>= 1000 B) at the same stages and on TX.
    uint32_t szRing[6] = {}, szParsed[6] = {}, szDeliv[6] = {}, szOther[6] = {};
    uint32_t icmpRepParsed = 0, icmpRepDeliv = 0, icmpReqTx = 0;
    static uint8_t szClass(uint32_t n) { return n < 128 ? 0 : n < 512 ? 1 : n < 1024 ? 2 : n < 1280 ? 3 : n < 1400 ? 4 : 5; }
    static bool isBigEcho(const uint8_t *e, uint32_t n, uint8_t type) {
        return n >= 1000 && n >= 34 && e[12] == 0x08 && e[13] == 0x00 && e[23] == 1 && e[14 + ((e[14] & 15) * 4)] == type;
    }
    // one sample per second: [eth delivered, ring frames, tx, fw retries, tx no-slot stalls, reorder old, reorder timeouts]
    static const uint32_t kTsN = 90;
    uint32_t tsRing[kTsN][7] = {};
    uint32_t tsHead = 0, tsLastMs = 0, tsPrev[7] = {};
    void tsSample(uint32_t now);
    char rssiProbe[700] = {0};               // RXD layout samples of the first beacons with RX vector, for the signal decoder
    uint8_t rssiProbeN = 0;
    // A-MSDU receive (level 4): the hardware splits an A-MSDU into one RX packet per subframe (first / middle / last in RXD4),
    // each with the 802.11 header. The subframes of one MPDU are collected in a bundle and handed to the reorder buffer together.
    struct AmsduAsm { bool active; uint8_t tid; bool reorder; uint16_t sn; uint32_t used, t; uint8_t buf[ITL_BA_SLOT_BYTES]; };
    AmsduAsm amsduAsm = {};
    uint32_t stAmsduPkt = 0, stAmsduSub = 0, stAmsduBad = 0, stRxBigDrop = 0;
    uint32_t amsduLogged = 0, amsduBadLogged = 0, rxdLogged = 0;
    // 802.11 duplicate detection for unicast data outside a Block Ack reorder window: the AP resends an MPDU whose ACK it did not
    // hear (Retry bit set, same sequence number), and nothing below this driver drops it. Index = TID, 8 = non-QoS.
    uint16_t rxLastSn[9] = {};
    uint16_t rxLastValid = 0;
    uint32_t stRxDupL = 0;
    uint32_t stRxRetry = 0, stRxPkt = 0;     // unicast QoS data MPDUs with the Retry bit / all unicast QoS data MPDUs (AP resends)
    uint32_t txfWinFree = 0, txfWinFail = 0, txfBad = 0;   // TX failure watchdog (level >= 3), 5 s windows
    volatile bool ethStalled = false;        // the Ethernet output queue is stalled on a full TX pool (under txLock)
    uint32_t ethStallMs = 0;
    uint32_t m3RxAmsdu(const uint8_t *body, uint32_t blen, uint8_t ai, bool reorder, uint8_t tid, uint16_t sn);
    void amsduFlush();                       // hand the collected subframes to the reorder buffer
    void rxStore(const uint8_t *eb, uint32_t n, bool reorder, uint8_t tid, uint16_t sn, bool last);   // Ethernet frame -> stack / reorder buffer
    static void ethDeliverCb(void *ctx, const uint8_t *frame, uint32_t len);
    static void amsduFrameCb(void *ctx, const uint8_t *da, const uint8_t *sa, uint16_t et, const uint8_t *pl, uint32_t pn);
    char balog[1100] = {0};
    size_t balen = 0;
    void baLog(const char *fmt, ...);
    void nvramBa(const char *buf);           // itlb0..itlb2
    uint32_t nowMs();
    void baReset();                          // forget every session (state is lost on re-association)
    void baService();                        // called from the link loop
    void baRxRequest(const itl_ba_action &a);
    void baRxStop(uint8_t tid, bool sendDelba);
    void baTxStart();
    void baTxResponse(const itl_ba_action &a);
    void baTxStop(bool sendDelba);
    int  baStaCmd(uint8_t tid, bool recipient, bool enable, uint16_t ssn, uint16_t win, bool amsdu = false);
    bool baSendAction(const uint8_t *body, size_t blen, uint8_t tid);
    bool txSlotAlloc(uint32_t *slot, uint16_t *tok);
    void m3TxFree(const uint8_t *p, uint32_t len);
    void ethDeliver(const uint8_t *frame, uint32_t len);
    // target AP, captured from a beacon / probe response during the scan
    bool tgtFound = false;
    uint8_t tgtBssid[6] = {0};
    uint8_t tgtCh = 0;
    uint16_t tgtCap = 0, tgtBcnInt = 0;
    uint8_t tgtIe[1024] = {0};
    uint32_t tgtIeLen = 0;
    char m1buf[3072] = {0};
    size_t m1len = 0;
    bool m1Started = false;
    bool regPending = false;              // native mode: registerService() waits until the firmware MAC is known (publishService)
    void publishService();
    bool driverOwn();
    void dumpState();
    void holdLog();
    void nvramLog(const char *msg);       // survives reboot, readable from Windows
    void nvramChunks(const char *buf);    // whole m3 log -> itlk0..itlk7
    char logbuf[240] = {0};
    size_t loglen = 0;
    void note(const char *fmt, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);

    // ---- runtime control: command mailbox, supervisor loop, status for the menu bar app (itlmt7925_uc.cpp) ----
    enum { CMD_SCAN = 1, CMD_JOIN = 2, CMD_DISCONNECT = 4, CMD_AWDL_SYNC = 8 };
    // hasPmk: `pmk` is the ready pairwise master key (native Wi-Fi: userland's supplicant derived it), psk is empty then
    struct Net { char ssid[33]; char psk[72]; bool hasPmk; bool native; uint8_t pmk[32]; };
    IOLock *cmdLock = nullptr;
    volatile uint32_t cmdBits = 0;
    Net reqNet = {};                         // the network of the last join command (under cmdLock)
    uint32_t lastScanReqMs = 0;
    bool cmdPending(uint32_t mask) const { return (__atomic_load_n(&cmdBits, __ATOMIC_ACQUIRE) & mask) != 0; }
    uint32_t cmdTake(uint32_t mask, Net *join);
    Net candNets[4] = {};                    // candidates for the next m3Run1(), in priority order
    int candN = 0;
    int loadPlistNets(Net *out, int max);
    bool apInRange(const char *ssid, uint32_t maxAgeMs);
    void m3Supervise();                      // replaces a single m3Run(): connect, reconnect, obey commands
    void wifiState(const char *st, const char *err);
    void wifiLink();                         // ssid / channel / rssi / rate of the current link
    void wifiNets();                         // wifi-networks from aps[]
    void wifiScan();                         // wildcard scan, then wifiNets()
    void apSeen(const uint8_t *p, uint32_t len, uint32_t off, const char *ssid, uint8_t ch);   // beacon / probe response
    int8_t rxRssi(const uint8_t *p, uint32_t len, uint32_t hdrOff);
    char wstate[16] = {0};
    uint32_t scanGen = 0;
    volatile uint32_t scanBusy = 0;          // a scan is queued (CMD_SCAN) or running (native Wi-Fi reads it through ITL_NATIVE_NETS)
    // native Wi-Fi join bookkeeping, read by ITL_NATIVE_STATUS: njState is ITL_NATIVE_*, njSeq counts joins, njFail a stage code
    volatile uint32_t njState = 0, njSeq = 0, njFail = 0;
    IOReturn nativeScanReq(uint32_t *started);
    IOReturn nativeJoin(const struct ItlNativeJoin *j);
    void nativeStatus(struct ItlNativeStatus *s);
    void nativeNets(struct ItlNativeNets *n);
    volatile bool m3lost = false;            // the AP sent deauthentication / disassociation, or went silent
    char m3lostWhy[48] = {0};
    char evbuf[6000] = {0};                  // wifi-events: the last link events of this boot, one per line, oldest first
    size_t evlen = 0;
    void evLog(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    uint32_t tgtLastBcnMs = 0, lastRxDataMs = 0;
    uint32_t tgtBcnCount = 0;                // beacons heard from tgtBssid while linked; reported in ItlNativeStatus.beaconCount
    int8_t tgtRssi = 0;
    void ethLink(bool up);
};

#endif
