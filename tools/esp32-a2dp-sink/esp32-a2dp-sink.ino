// A2DP SINK INSTRUMENT for the M.2 Bluetooth work, on a SparkFun ESP32 Thing
// Plus (original, ESP32-WROOM-32, CP2104 USB-serial).
//
// WHY THIS EXISTS
// ---------------
// BT-3 turns the RT1176 into an A2DP SOURCE.  A commercial headset is a black
// box for that job: hand it a SET_CONFIGURATION it dislikes or an SBC frame it
// cannot parse and it simply stays silent -- indistinguishable from a link that
// never carried a byte.  This sink is the ORACLE instead.  It runs Bluedroid
// (ESP-IDF's stack, Apache-2.0) as a black box and reports, over USB serial,
// exactly what a sink sees:
//
//   * every connection / audio-state transition, with the peer's address;
//   * the codec configuration the negotiation actually settled on (sample
//     rate, channel mode, block length, subbands, allocation, bitpool);
//   * a running count of decoded PCM packets and bytes, and the PCM rate;
//   * and it PLAYS the audio on the chip's own DACs, so the capstone's
//     "audible tone" assertion needs no extra parts.
//
// It is always connectable + discoverable with a fixed name and Just-Works
// pairing (IO capability NONE), so there is no pairing-mode button and no
// window to race -- the two things that made headset runs expensive.
//
// CLEAN-ROOM NOTE.  This is bench equipment, not firmware for the RT1176, and
// it is never linked into that tree.  We use Bluedroid ONLY through its public
// API, as an instrument; our own AVDTP/SBC stays built from the specification.
//
// CALIBRATE BEFORE TRUSTING.  Pair a phone or the Mac to "EVKB-SINK", play
// something, and confirm (a) audio_cfg prints, (b) the packet counter runs at
// the expected rate, (c) sound comes out of A0/A1.  An instrument that has
// never been checked against a known-good source cannot grade our stack.
//
// AUDIO OUT.  ESP32 DAC1 = GPIO25 (Thing Plus "A1", left), DAC2 = GPIO26
// ("A0", right): 8-bit, 0..3.3 V, unbuffered.  Straight into a powered
// speaker / amp input (a series 1-10 uF cap to lose the DC offset is polite).
// Not headphone-drive; not hi-fi; it is an audibility oracle.
//
// BUILD + FLASH  (run from the repo root; the port is the CP2104 "usbserial")
//   arduino-cli compile -b esp32:esp32:esp32thing_plus tools/esp32-a2dp-sink
//   arduino-cli upload  -b esp32:esp32:esp32thing_plus -p /dev/cu.usbserial-XXXX \
//                       tools/esp32-a2dp-sink
// Opening the serial port toggles DTR, which RESETS the board -- so a reader
// always sees a fresh boot banner.  115200 8N1.
//
// MIT.

#include <Arduino.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "driver/dac_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

static const char *kName = "EVKB-SINK";

// ★ Arduino-ESP32 releases ALL Bluetooth controller RAM to the heap at startup
// (esp_bt_controller_mem_release(BTDM) in initArduino) unless a sketch claims
// the radio through this weak hook -- only the bundled BluetoothSerial/BLE
// libraries do.  Without it esp_bt_controller_init() fails with no memory to
// run in, which reads like a dead radio.  Measured, not assumed.
extern "C" bool btInUse() { return true; }

// ── counters the heartbeat and the audio-state events report ──────────────
static volatile uint32_t s_pkts = 0, s_bytes = 0, s_dropped = 0;
static volatile uint32_t s_pcmRate = 0;             // negotiated sample rate, from audio_cfg
static volatile uint8_t  s_connected = 0, s_playing = 0;
static uint32_t          s_lastRateMs = 0, s_lastRateBytes = 0;
static char              s_peer[18] = "--";

static void fmtBda(const uint8_t *b, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

// ── DAC output: ring buffer fed from the A2DP data callback, drained by a task ─
// The data callback runs on the Bluetooth task and must never block, so it
// only copies into the ring (drop + count on overflow).  The DAC task does the
// blocking write.  16-bit stereo PCM -> 8-bit unsigned interleaved L,R.
static RingbufHandle_t          s_ring = nullptr;
static dac_continuous_handle_t  s_dac  = nullptr;
static uint32_t                 s_dacRate = 0;

static void dacStart(uint32_t rate) {
    if (s_dac && s_dacRate == rate) return;
    if (s_dac) { dac_continuous_disable(s_dac); dac_continuous_del_channels(s_dac); s_dac = nullptr; }
    dac_continuous_config_t cfg = {};
    cfg.chan_mask = DAC_CHANNEL_MASK_ALL;           // DAC1 (GPIO25) + DAC2 (GPIO26)
    cfg.desc_num  = 8;
    cfg.buf_size  = 2048;
    cfg.freq_hz   = rate;
    cfg.offset    = 0;
    cfg.clk_src   = DAC_DIGI_CLK_SRC_APLL;          // APLL: exact 44.1 k / 48 k
    cfg.chan_mode = DAC_CHANNEL_MODE_ALTER;         // interleaved L,R
    if (dac_continuous_new_channels(&cfg, &s_dac) != ESP_OK) { Serial.println("dac=new_fail"); s_dac = nullptr; return; }
    if (dac_continuous_enable(s_dac) != ESP_OK)              { Serial.println("dac=enable_fail"); return; }
    s_dacRate = rate;
    Serial.printf("dac=on rate=%lu pins=GPIO25(A1,L)/GPIO26(A0,R)\n", (unsigned long)rate);
}

static void dacTask(void *) {
    static uint8_t out[1024];
    for (;;) {
        size_t n = 0;
        int16_t *pcm = (int16_t *)xRingbufferReceiveUpTo(s_ring, &n, pdMS_TO_TICKS(100), sizeof(out) * 2);
        if (!pcm) continue;
        size_t samples = n / 2;                                        // int16 samples, L R L R ...
        for (size_t i = 0; i < samples; i++) out[i] = (uint8_t)((pcm[i] >> 8) + 128);
        vRingbufferReturnItem(s_ring, pcm);
        if (s_dac) { size_t loaded = 0; dac_continuous_write(s_dac, out, samples, &loaded, 200); }
    }
}

// ── A2DP ──────────────────────────────────────────────────────────────────
static void a2dpData(const uint8_t *data, uint32_t len) {
    s_pkts++; s_bytes += len;
    if (s_ring && xRingbufferSend(s_ring, data, len, 0) != pdTRUE) s_dropped++;
}

static const char *sbcRate(uint8_t b) {
    switch (b & 0xF0) { case 0x80: return "16000"; case 0x40: return "32000"; case 0x20: return "44100"; case 0x10: return "48000"; default: return "?"; }
}
static uint32_t sbcRateHz(uint8_t b) {
    switch (b & 0xF0) { case 0x80: return 16000; case 0x40: return 32000; case 0x20: return 44100; case 0x10: return 48000; default: return 44100; }
}
static const char *sbcChan(uint8_t b) {
    switch (b & 0x0F) { case 0x08: return "mono"; case 0x04: return "dual"; case 0x02: return "stereo"; case 0x01: return "joint"; default: return "?"; }
}

static void a2dpEvent(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t *p) {
    switch (ev) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        static const char *st[] = { "disconnected", "connecting", "connected", "disconnecting" };
        fmtBda(p->conn_stat.remote_bda, s_peer);
        s_connected = (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED);
        Serial.printf("a2dp_conn: state=%s peer=%s", st[p->conn_stat.state], s_peer);
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
            Serial.printf(" reason=%s", p->conn_stat.disc_rsn == ESP_A2D_DISC_RSN_NORMAL ? "normal" : "abnormal");
        Serial.println();
        break; }
    case ESP_A2D_AUDIO_STATE_EVT: {
        static const char *st[] = { "suspended", "stopped", "started" };
        s_playing = (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
        Serial.printf("a2dp_audio: state=%s pkts=%lu bytes=%lu dropped=%lu\n", st[p->audio_stat.state],
                      (unsigned long)s_pkts, (unsigned long)s_bytes, (unsigned long)s_dropped);
        if (s_playing) { s_pkts = 0; s_bytes = 0; s_dropped = 0; s_lastRateMs = millis(); s_lastRateBytes = 0; }
        break; }
    case ESP_A2D_AUDIO_CFG_EVT: {                       // the negotiated codec -- BT-3's SET_CONFIGURATION oracle
        const uint8_t *c = p->audio_cfg.mcc.cie.sbc;
        Serial.printf("a2dp_audio_cfg: codec=%s", p->audio_cfg.mcc.type == ESP_A2D_MCT_SBC ? "SBC" : "other");
        if (p->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            Serial.printf(" rate=%s chan=%s blocks=%d subbands=%d alloc=%s bitpool=%u..%u cie=%02X %02X %02X %02X",
                          sbcRate(c[0]), sbcChan(c[0]),
                          (c[1] & 0x80) ? 4 : (c[1] & 0x40) ? 8 : (c[1] & 0x20) ? 12 : 16,
                          (c[1] & 0x08) ? 4 : 8, (c[1] & 0x02) ? "SNR" : "loudness",
                          c[2], c[3], c[0], c[1], c[2], c[3]);
            s_pcmRate = sbcRateHz(c[0]);
            dacStart(s_pcmRate);
        }
        Serial.println();
        break; }
    default:
        Serial.printf("a2dp_event: %d\n", (int)ev);
    }
}

// ── GAP: Just-Works pairing, log everything ───────────────────────────────
static void gapEvent(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *p) {
    char bda[18];
    switch (ev) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        fmtBda(p->auth_cmpl.bda, bda);
        Serial.printf("gap_auth: status=%d peer=%s name=\"%s\"\n", (int)p->auth_cmpl.stat, bda, (const char *)p->auth_cmpl.device_name);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:                       // numeric comparison: accept (IO cap NONE => Just Works anyway)
        fmtBda(p->cfm_req.bda, bda);
        Serial.printf("gap_confirm_req: peer=%s num=%lu -> accept\n", bda, (unsigned long)p->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        Serial.printf("gap_key_notif: passkey=%lu\n", (unsigned long)p->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        Serial.println("gap_key_req (passkey entry requested -- unexpected with IO cap NONE)");
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {                     // legacy (pre-SSP) pairing fallback
        esp_bt_pin_code_t pin = { '1', '2', '3', '4' };
        Serial.println("gap_pin_req -> 1234");
        esp_bt_gap_pin_reply(p->pin_req.bda, true, 4, pin);
        break; }
    case ESP_BT_GAP_MODE_CHG_EVT:
        Serial.printf("gap_mode: %d\n", (int)p->mode_chg.mode);
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        fmtBda(p->acl_conn_cmpl_stat.bda, bda);
        Serial.printf("gap_acl_conn: status=%d peer=%s\n", (int)p->acl_conn_cmpl_stat.stat, bda);
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        fmtBda(p->acl_disconn_cmpl_stat.bda, bda);
        Serial.printf("gap_acl_disconn: reason=%d peer=%s\n", (int)p->acl_disconn_cmpl_stat.reason, bda);
        break;
    default:
        Serial.printf("gap_event: %d\n", (int)ev);
    }
}

static bool btUp() {
    esp_err_t e;
    // The prebuilt Arduino controller is dual-mode (BTDM); enable() must be
    // asked for the mode it was BUILT for (cc.mode), and BLE's RAM must not be
    // released first -- ESP_BT_MODE_CLASSIC_BT here returns ESP_ERR_INVALID_ARG.
    esp_bt_controller_config_t cc = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((e = esp_bt_controller_init(&cc)) != ESP_OK)                      { Serial.printf("bt=controller_init_fail %s\n", esp_err_to_name(e)); return false; }
    if ((e = esp_bt_controller_enable((esp_bt_mode_t)cc.mode)) != ESP_OK) { Serial.printf("bt=controller_enable_fail %s mode=%d\n", esp_err_to_name(e), (int)cc.mode); return false; }
    esp_bluedroid_config_t bc = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if ((e = esp_bluedroid_init_with_cfg(&bc)) != ESP_OK)                 { Serial.printf("bt=bluedroid_init_fail %s\n", esp_err_to_name(e)); return false; }
    if ((e = esp_bluedroid_enable()) != ESP_OK)                           { Serial.printf("bt=bluedroid_enable_fail %s\n", esp_err_to_name(e)); return false; }

    esp_bt_gap_register_callback(gapEvent);
    esp_bt_gap_set_device_name(kName);

    // IO capability DisplayYesNo, confirmed in gapEvent() -- the path the
    // reference sinks use.  With IO cap NONE Bluedroid auto-replies inside the
    // stack instead, and against the RT1176 probe that path failed every time
    // ("E(26, 0) in user_cfm_req_reply", then LMP response timeout) while the
    // iPhone paired fine.  Peer NoInputNoOutput + our YesNo is still Just Works.
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));

    // Class of Device: Audio/Video major, Headphones minor, Rendering+Audio
    // service bits -- the RT1176 probe targets inquiry hits with major class 4.
    esp_bt_cod_t cod = {};
    cod.major   = ESP_BT_COD_MAJOR_DEV_AV;
    cod.minor   = 0x06;                                               // headphones
    cod.service = ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_AUDIO;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);

    esp_a2d_register_callback(a2dpEvent);
    esp_a2d_sink_register_data_callback(a2dpData);
    if (esp_a2d_sink_init() != ESP_OK)                                { Serial.println("bt=a2dp_sink_init_fail"); return false; }

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("esp32-a2dp-sink: A2DP SINK instrument (Bluedroid as a black box)");
    s_ring = xRingbufferCreate(32 * 1024, RINGBUF_TYPE_BYTEBUF);
    xTaskCreatePinnedToCore(dacTask, "dac", 4096, nullptr, 5, nullptr, 1);
    if (!btUp()) { Serial.println("bt=FAILED -- instrument is not usable"); return; }
    const uint8_t *a = esp_bt_dev_get_address();
    char me[18]; fmtBda(a, me);
    Serial.printf("bt=up name=\"%s\" bd_addr=%s cod=AV/headphones iocap=DisplayYesNo(auto-confirm) legacy_pin=1234 discoverable=yes\n", kName, me);
    Serial.println("ready: pair from a phone/Mac first to calibrate, then point m2_hci_probe at it (M2_BT_TARGET_NAME=EVKB-SINK)");
}

void loop() {
    static uint32_t n = 0;
    delay(5000);
    uint32_t now = millis();
    uint32_t bytes = s_bytes;
    uint32_t rateBps = (now > s_lastRateMs) ? (uint32_t)((uint64_t)(bytes - s_lastRateBytes) * 1000 / (now - s_lastRateMs)) : 0;
    s_lastRateMs = now; s_lastRateBytes = bytes;
    Serial.printf("hb n=%lu conn=%u play=%u peer=%s pkts=%lu bytes=%lu pcm_bytes_per_s=%lu (expect %lu at %lu Hz) dropped=%lu\n",
                  (unsigned long)n++, s_connected, s_playing, s_peer, (unsigned long)s_pkts, (unsigned long)bytes,
                  (unsigned long)rateBps, (unsigned long)(s_pcmRate * 4), (unsigned long)s_pcmRate, (unsigned long)s_dropped);
}
