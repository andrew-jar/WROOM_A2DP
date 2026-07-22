
// Copyright (c) 2026 A. Jaroszuk. All rights reserved.
// Licensed for private home use only under LICENSE_HOME_USE.md.
// No redistribution, no commercial use, no derivative distribution without prior written permission.



/*
  WROOM-BT-TX v1.6.6 (ESP32-WROOM-32D) — AUTO SRC 44.1/48k + RESAMPLE + BETTER SCAN + UART EVENTS
  Autor: A. Jaroszuk
  -----------------------------------------------------------------------------------
  Co robi:
  - Wykrywa częstotliwość WS/LRCLK (~44100 lub ~48000) na PIN_I2S_WS
  - Gdy źródło = 44.1k: passthrough
  - Gdy źródło = 48k: resampling 48k -> 44.1k (linear interpolation)
  - Ring buffer PCM (żeby uniknąć blokowania i underrunów)
  - GAP scan + EIR (lepsze nazwy urządzeń)
  - Eventy po UART/USB: A2DP_CONN / A2DP_AUDIO / SRC_FS
  - VOL 0..100 -> 0..127
  - BOOST 100..400 (%), domyślnie 100


  Poprawki: (22.07.2026) v1.6.6
  - GAP DISCOVERY_STARTED: scanClear() dla czystej listy po każdym SCAN
  - GAP DISC_RES: ignorowanie wyników gdy już po CONNECT oraz poza aktywnym SCAN
  - GAP DISC_RES: nie aktualizuj nazwy pustym EIR; aktualizuj tylko RSSI
  - Nowa komenda: SCAN STOP (cancel discovery przed CONNECT)

  Poprawki: (20.07.2026) v1.6.5
  - RB_DROP: zamiast skip co drugiej ramki, flush do najnowszych danych (mniej chipmunk/fast-forward)
  - DISCONNECTED: czyszczenie RB i fazy resamplera
  - RB_FRAMES: 12288 -> 16384 (większy margines na skoki WiFi/S3)
  - I2S DMA: 8x256 -> 8x512
  - i2s_read(br==0): nie licz jako twardy ZR, tylko poczekaj 1 ms

  Poprawki: (19.07.2026) v1.6.4
  - CONNECT retry: CONNECT_RETRY_MAX = 1 (jedna automatyczna ponowna próba po szybkim DISCONNECTED)
  - Resampler: dodany przełącznik RESAMPLER_QUALITY
      0 = fast linear
      1 = linear + 2-stage low-pass (mniej aliasingu na wysokich tonach)
  - Skanowanie urządzeń: filtr COD RENDERING przeniesiony do wersji MOD AJ
    w bibliotece ESP32-A2DP (BluetoothA2DPSource.cpp)

  Komendy:
    HELP, PING, GET/STATUS?
    BT ON, BT OFF
    MODE OFF|TX (MODE AUTO tylko legacy/compat)
    VOL 0..100
    BOOST 100..400
    SCAN
    CONNECT <idx> lub CONNECT AA:BB:CC:DD:EE:FF
    DISCONNECT
    PAIRED?
    DELPAIRED ALL
    SAVE
    DBG 0|1
    HARDRESET
*/

#include <Arduino.h>
#include "BluetoothA2DPSource.h"

extern "C" {
  #include "nvs_flash.h"
  #include "nvs.h"
  #include "driver/i2s.h"
  #include "esp_bt.h"
  #include "esp_gap_bt_api.h"
  #include "esp_bt_main.h"
  #include "esp_bt_device.h"
  #include "esp_a2dp_api.h"
  #include "esp_system.h"
  #include "esp_heap_caps.h"
}

#include <stdarg.h>

// Arduino preprocessor may generate function prototypes before enum definitions.
// Forward-declare the enum type to keep generated prototypes valid.
enum SrcRate : uint8_t;

// ESP-IDF/Arduino core compatibility: IDF 4.4 (core 2.x) uses REMOTE_SUSPEND,
// IDF 5.x uses SUSPENDED or SUSPEND depending on minor version.
#ifndef ESP_A2D_AUDIO_STATE_SUSPENDED
  #ifdef ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND
    #define ESP_A2D_AUDIO_STATE_SUSPENDED ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND
  #elif defined(ESP_A2D_AUDIO_STATE_SUSPEND)
    #define ESP_A2D_AUDIO_STATE_SUSPENDED ESP_A2D_AUDIO_STATE_SUSPEND
  #else
    #define ESP_A2D_AUDIO_STATE_SUSPENDED ((esp_a2d_audio_state_t)3)
  #endif
#endif

// ====== KONFIG PINÓW (DOPASUJ) ======
static const int PIN_UART_RX = 16;
static const int PIN_UART_TX = 17;
static const uint32_t UART_BAUD = 115200;

// I2S (podsłuch z S3) - mapping z działającej wersji legacy wroom_BT
static const int PIN_I2S_BCLK = 14;
static const int PIN_I2S_WS   = 15;  // LRCLK/WS - tu mierzymy Fs
static const int PIN_I2S_DIN  = 32;

// audio format
static const i2s_bits_per_sample_t AUDIO_BITS = I2S_BITS_PER_SAMPLE_32BIT;

// A2DP output (stałe) — w praktyce ESP32 A2DP source lubi 44.1k
static const int OUT_SR = 44100;
// =====================================

HardwareSerial CTRL(2);
BluetoothA2DPSource a2dp;
static const i2s_port_t I2S_PORT = I2S_NUM_0;

// ====== DEBUG / LOG ======
static bool USB_DEBUG = true;

static void logLn(const char* s){
  CTRL.println(s);
  if (USB_DEBUG) Serial.println(s);
}

static void logF(const char* fmt, ...){
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  CTRL.print(buf);
  if (USB_DEBUG) Serial.print(buf);
}
// =========================

enum Mode : uint8_t { MODE_OFF=0, MODE_TX=1, MODE_AUTO=2 };
static volatile Mode g_mode = MODE_TX;

static bool g_btReady = false;
static bool g_scanning = false;
static bool g_a2dpConnected = false;
static bool g_connectInProgress = false;
static bool g_ignoreLocalDisconnectOnce = false;
static uint32_t g_connectRetryAtMs = 0;
static uint8_t g_connectRetryCount = 0;
static uint32_t g_lastAudioDataMs = 0;

static String g_connMac = "";
static String g_connName = "";

// VOL: 0..100 -> 0..127
static int g_vol_ui = 100;
static uint8_t g_vol_127 = 127;

// BOOST: 100..400 (%), domyślnie 100
static int g_boost_pct = 100;

// ===== Scan list =====
struct Dev {
  esp_bd_addr_t bda{};
  int rssi = 0;
  String name;
  bool valid = false;
};
static Dev g_scan[25];
static int g_scanCount = 0;
static uint32_t g_scanStartedMs = 0;
static String g_targetMac = "";
static const uint32_t SCAN_WINDOW_MS = 12000;
static const uint8_t CONNECT_RETRY_MAX = 1;
static const uint32_t CONNECT_RETRY_DELAY_MS = 1400;
static const uint32_t CONNECT_POST_SCAN_DELAY_MS = 350;

static void clear_connect_retry_timer(){
  g_connectRetryAtMs = 0;
}

static void reset_connect_retry_state(){
  g_connectRetryAtMs = 0;
  g_connectRetryCount = 0;
  g_ignoreLocalDisconnectOnce = false;
}

static inline int16_t sampleFromHigh16(int32_t sample32){
  int32_t v = sample32 >> 16;
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return (int16_t)v;
}

static void setBtTxPowerMax(){
  esp_err_t err = esp_bredr_tx_power_set(ESP_PWR_LVL_P9, ESP_PWR_LVL_P9);
  if (err == ESP_OK){
    logLn("OK BT TX POWER P9");
  } else {
    logF("ERR BT TX POWER %d\n", (int)err);
  }
}

static bool start_gap_scan(){
  // Inquiry length unit is 1.28s. 10 -> about 12.8s.
  esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  if (err != ESP_OK){
    logF("ERR SCAN START GAP=%d\n", (int)err);
    g_scanning = false;
    return false;
  }
  return true;
}

// ===== Source sample rate detect =====
enum SrcRate : uint8_t { SRC_UNKNOWN=0, SRC_44100=1, SRC_48000=2 };
static volatile SrcRate g_srcRate = SRC_UNKNOWN;
static volatile int g_srcHz = 0;

// ISR counter
static volatile uint32_t g_wsEdges = 0;

// ===== Runtime diagnostics =====
static volatile uint32_t g_i2sReadsOk = 0;
static volatile uint32_t g_i2sReadsErr = 0;
static volatile uint32_t g_i2sZeroReads = 0;
static volatile uint32_t g_i2sBytes = 0;
static volatile uint32_t g_cbCalls = 0;
static volatile uint32_t g_cbSilent = 0;
static volatile uint32_t g_cbUnderrun = 0;

// ===== Ring buffer PCM (3-state machine) =====
// State machine for adaptive buffer management:
// - PREFETCH (0): RB filling, return silence to A2DP (avoid underruns)
// - PROCESS (1): RB at target level, normal output
// - DROP (2): RB overfilled, skip frames (prevent overflow artifacts)
enum RBState : uint8_t { RB_PREFETCH=0, RB_PROCESS=1, RB_DROP=2 };
static volatile RBState g_rbState = RB_PREFETCH;
static const uint32_t RB_PREFETCH_THRESHOLD = 2048;   // transition to PROCESS when > this
static const uint32_t RB_PROCESS_MIN_THRESHOLD = 1536; // drop below this -> back to PREFETCH
static const uint32_t RB_DROP_THRESHOLD = 7000;        // transition to DROP when > this
static const uint32_t RB_DROP_MIN_THRESHOLD = 5000;    // drop below this -> back to PROCESS

// przechowujemy stereo frames: L,R (int16,int16)
// rozmiar w frames (nie w samplach)
static const int RB_FRAMES = 8192; // 8192 frames ~ 8192/48k = 171 ms
static int16_t rb[RB_FRAMES * 2];  // [L,R,L,R...]
static volatile uint32_t rb_w = 0; // write index in frames
static volatile uint32_t rb_r = 0; // read index in frames
static portMUX_TYPE rb_mux = portMUX_INITIALIZER_UNLOCKED;

static inline uint32_t rb_count_frames(){
  uint32_t w = rb_w, r = rb_r;
  return (w >= r) ? (w - r) : (RB_FRAMES - (r - w));
}

static inline uint32_t rb_free_frames(){
  // zostawiamy 1 frame wolny żeby odróżnić full/empty
  return (RB_FRAMES - 1) - rb_count_frames();
}

static void rb_clear(){
  portENTER_CRITICAL(&rb_mux);
  rb_w = rb_r = 0;
  portEXIT_CRITICAL(&rb_mux);
}

static bool rb_push_frames(const int16_t* framesLR, uint32_t frames){
  bool ok = true;
  portENTER_CRITICAL(&rb_mux);
  uint32_t freeF = rb_free_frames();
  if (frames > freeF){
    // overflow: utnij nadmiar (lepsze niż blokowanie)
    frames = freeF;
    ok = false;
  }
  for(uint32_t i=0;i<frames;i++){
    uint32_t wi = rb_w;
    rb[wi*2 + 0] = framesLR[i*2 + 0];
    rb[wi*2 + 1] = framesLR[i*2 + 1];
    rb_w = (wi + 1) % RB_FRAMES;
  }
  portEXIT_CRITICAL(&rb_mux);
  return ok;
}

static uint32_t rb_pop_frames(int16_t* outLR, uint32_t frames){
  uint32_t got = 0;
  portENTER_CRITICAL(&rb_mux);
  uint32_t avail = rb_count_frames();
  if (frames > avail) frames = avail;
  for(uint32_t i=0;i<frames;i++){
    uint32_t ri = rb_r;
    outLR[i*2 + 0] = rb[ri*2 + 0];
    outLR[i*2 + 1] = rb[ri*2 + 1];
    rb_r = (ri + 1) % RB_FRAMES;
    got++;
  }
  portEXIT_CRITICAL(&rb_mux);
  return got;
}

// ===== Resampler state (48 -> 44.1) =====
// phase w Q16 oznacza pozycję w wejściu (w frames)
// step = input_sr / output_sr w Q16
static uint32_t g_phase_q16 = 0;
static uint32_t g_step_q16  = 0;

// Resampler quality:
// 0 = fast linear interpolation
// 1 = linear interpolation + 2-stage low-pass smoothing (lower aliasing on highs)
static const uint8_t RESAMPLER_QUALITY = 1;
static int32_t g_rsLpL1 = 0;
static int32_t g_rsLpL2 = 0;
static int32_t g_rsLpR1 = 0;
static int32_t g_rsLpR2 = 0;

static inline int16_t clamp16_from_i32(int32_t v){
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static inline int16_t resampler_hq_filter_i32(int32_t x, int32_t& s1, int32_t& s2){
  // 2x one-pole LP in Q15. Chosen as a lightweight anti-alias stage for 48k->44.1k.
  static const int32_t ALPHA_Q15 = 18000;
  s1 += (int32_t)(((int64_t)(x - s1) * ALPHA_Q15) >> 15);
  s2 += (int32_t)(((int64_t)(s1 - s2) * ALPHA_Q15) >> 15);
  return clamp16_from_i32(s2);
}

// do interpolacji potrzebujemy dwóch kolejnych frames
static bool rb_peek_two(int16_t &l0,int16_t &r0,int16_t &l1,int16_t &r1){
  bool ok = false;
  portENTER_CRITICAL(&rb_mux);
  uint32_t avail = rb_count_frames();
  if (avail >= 2){
    uint32_t ri0 = rb_r;
    uint32_t ri1 = (ri0 + 1) % RB_FRAMES;
    l0 = rb[ri0*2 + 0]; r0 = rb[ri0*2 + 1];
    l1 = rb[ri1*2 + 0]; r1 = rb[ri1*2 + 1];
    ok = true;
  }
  portEXIT_CRITICAL(&rb_mux);
  return ok;
}

static void rb_drop_frames(uint32_t frames){
  portENTER_CRITICAL(&rb_mux);
  uint32_t avail = rb_count_frames();
  if (frames > avail) frames = avail;
  rb_r = (rb_r + frames) % RB_FRAMES;
  portEXIT_CRITICAL(&rb_mux);
}

static void rb_keep_recent_frames(uint32_t minFrames){
  portENTER_CRITICAL(&rb_mux);
  uint32_t avail = rb_count_frames();
  if (avail > minFrames) {
    rb_r = (rb_w + RB_FRAMES - minFrames) % RB_FRAMES;
  }
  portEXIT_CRITICAL(&rb_mux);
}

// ====== Utils ======
static String bdaToStr(const esp_bd_addr_t bda){
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
           bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
  return String(s);
}

static bool parseMac(const String& mac, esp_bd_addr_t out){
  int b[6];
  if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  for (int i = 0; i < 6; i++){
    if (b[i] < 0 || b[i] > 255) return false;
    out[i] = (uint8_t)b[i];
  }
  return true;
}

static void scanClear(){
  // Keep String objects valid while force-resetting cached scan data.
  for (auto &d: g_scan){
    memset(d.bda, 0, sizeof(d.bda));
    d.rssi = 0;
    d.name = "";
    d.valid = false;
  }
  g_scanCount = 0;
}

static int scanFindByMac(const String& mac){
  for(int i=0;i<g_scanCount;i++){
    if (!g_scan[i].valid) continue;
    if (bdaToStr(g_scan[i].bda).equalsIgnoreCase(mac)) return i;
  }
  return -1;
}

static void scanStore(const esp_bd_addr_t bda, int rssi, const String& name){
  for(int i=0;i<g_scanCount;i++){
    if (g_scan[i].valid && memcmp(g_scan[i].bda, bda, 6) == 0){
      g_scan[i].rssi = rssi;
      if (name.length() && name != "Android TV" && g_scan[i].name.startsWith("JBL")) {
        // Do not overwrite known JBL label with generic Android TV advert name.
      } else if (name.length()) {
        g_scan[i].name = name;
      }
      return;
    }
  }
  if (g_scanCount >= (int)(sizeof(g_scan)/sizeof(g_scan[0]))) return;
  memcpy(g_scan[g_scanCount].bda, bda, 6);
  g_scan[g_scanCount].rssi = rssi;
  g_scan[g_scanCount].name = name;
  g_scan[g_scanCount].valid = true;
  g_scanCount++;
}

static bool on_ssid_found(const char* ssid, esp_bd_addr_t address, int rssi){
  String name = ssid ? String(ssid) : String("");
  String mac = bdaToStr(address);

  if (g_scanning || g_targetMac.length()){
    int prevCount = g_scanCount;
    scanStore(address, rssi, name);
    int idx = scanFindByMac(mac);
    if (idx >= 0 && idx >= prevCount){
      logF("DEV %d %s RSSI=%d NAME=\"%s\"\n", idx, mac.c_str(), rssi, g_scan[idx].name.c_str());
    }
  }

  if (g_targetMac.length() && mac.equalsIgnoreCase(g_targetMac)){
    int idx = scanFindByMac(mac);
    if (idx >= 0 && name.length()) {
      g_scan[idx].name = name;
    }
    g_connMac = mac;
    if (name.length()) g_connName = name;
    logF("EVT SCAN MATCH MAC=%s NAME=\"%s\"\n", mac.c_str(), g_connName.c_str());

    return true;
  }

  return false;
}

// ===== I2S init (slave RX) =====
static void i2s_init_slave_rx(){
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX);
  cfg.sample_rate = OUT_SR; // w SLAVE i tak liczy się zewnętrzny WS, ale zostawiamy sensowną wartość
  cfg.bits_per_sample = AUDIO_BITS;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_I2S;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 512;
  cfg.use_apll = false;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = -1;
  pins.data_in_num = PIN_I2S_DIN;
#if ESP_IDF_VERSION_MAJOR >= 5
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
#endif

  i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

// ===== WS ISR =====
static void IRAM_ATTR ws_isr(){
  g_wsEdges++;
}

// ===== Source rate monitor task =====
static TaskHandle_t g_rateTask = nullptr;

static void reset_resampler_for(SrcRate r){
  // wyczyść bufor żeby nie mieszać starych próbek w nowym trybie
  rb_clear();
  g_phase_q16 = 0;
  g_rsLpL1 = g_rsLpL2 = 0;
  g_rsLpR1 = g_rsLpR2 = 0;

  if (r == SRC_48000){
    // step = input_sr / output_sr w Q16
    // (48000/44100) * 65536
    g_step_q16 = (uint32_t)(((uint64_t)48000 << 16) / (uint32_t)OUT_SR);
  } else {
    g_step_q16 = 0;
  }
}

static void rate_task(void *){
  // prosta stabilizacja: wymagamy 2 takich samych odczytów pod rząd
  SrcRate lastDec = SRC_UNKNOWN;
  int stable = 0;

  uint32_t lastEdges = 0;
  uint32_t lastMs = millis();

  for(;;){
    vTaskDelay(pdMS_TO_TICKS(250));

    uint32_t nowMs = millis();
    uint32_t e = g_wsEdges;
    uint32_t de = e - lastEdges;
    uint32_t dt = nowMs - lastMs;
    lastEdges = e;
    lastMs = nowMs;

    if (dt < 50){
      continue;
    }

    // rising edges na WS: 1 na frame => ~ sample rate
    float hz = (float)de * 1000.0f / (float)dt;
    int ihz = (int)(hz + 0.5f);
    g_srcHz = ihz;

    SrcRate dec = SRC_UNKNOWN;
    if (ihz > 43000 && ihz < 45500) dec = SRC_44100;
    else if (ihz > 46500 && ihz < 49500) dec = SRC_48000;
    else dec = SRC_UNKNOWN;

    if (dec == lastDec && dec != SRC_UNKNOWN){
      stable++;
    } else {
      stable = 0;
      lastDec = dec;
    }

    // po 2 stabilnych oknach (czyli ok. 0.5s) przełącz tryb
    if (stable >= 2 && dec != g_srcRate){
      g_srcRate = dec;
      reset_resampler_for(dec);
      logF("EVT SRC_FS %dHz MODE=%s\n",
           g_srcHz,
           (dec==SRC_44100) ? "44100" : (dec==SRC_48000 ? "48000" : "UNKNOWN"));
    }

    // jeśli nie wykrywa nic sensownego przez dłużej, oznacz UNKNOWN
    if (dec == SRC_UNKNOWN){
      // nie czyścimy od razu żeby nie skakało, ale jeśli już jest UNKNOWN, to ok
      // (zostawiamy ostatni tryb, bo niektóre źródła potrafią mieć krótkie dziury)
    }
  }
}

// ===== PCM producer task (I2S -> ring buffer) =====
static TaskHandle_t g_pcmTask = nullptr;

static void pcm_task(void *){
  static uint8_t raw[2048]; // 32-bit L + 32-bit R (8 bajtów na frame)
  for(;;){
    if (!g_btReady || g_mode == MODE_OFF){
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    size_t br = 0;
    // krótki timeout żeby nie przywieszać
    esp_err_t r = i2s_read(I2S_PORT, raw, sizeof(raw), &br, pdMS_TO_TICKS(20));
    if (r != ESP_OK){
      g_i2sReadsErr++;
      continue;
    }
    if (br == 0){
      // Zero-length reads are expected occasionally when producer timing drifts.
      // Do not count them as hard ZR errors in diagnostics.
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    g_i2sReadsOk++;
    g_i2sBytes += (uint32_t)br;

    // br bajtów => frames = br / 8 (L32+R32)
    uint32_t frames = (uint32_t)(br / 8);
    if (frames > 256) frames = 256;

    int32_t *src = (int32_t*)raw;

    int16_t pcm16[256 * 2];
    for (uint32_t i = 0; i < frames; i++){
      pcm16[i * 2 + 0] = sampleFromHigh16(src[i * 2 + 0]);
      pcm16[i * 2 + 1] = sampleFromHigh16(src[i * 2 + 1]);
    }

    // BOOST (opcjonalny) na surowych próbkach zanim trafią do bufora
    if (g_boost_pct != 100){
      int16_t *s = pcm16;
      uint32_t count = frames * 2;
      int32_t gain_q10 = (g_boost_pct * 1024) / 100;
      for(uint32_t i=0;i<count;i++){
        int32_t v = (int32_t)s[i] * gain_q10;
        v >>= 10;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
      }
    }

    if (!g_a2dpConnected || (g_rbState == RB_PREFETCH && rb_count_frames() > RB_PREFETCH_THRESHOLD * 2)) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    rb_push_frames(pcm16, frames);
  }
}

// ===== EIR name helper =====
static String eirToName(uint8_t *eir){
  if (!eir) return "";
  uint8_t len = 0;
  uint8_t *p = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
  if (!p) p = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
  if (p && len){
    char name[64];
    int n = (len < (sizeof(name)-1)) ? len : (sizeof(name)-1);
    memcpy(name, p, n);
    name[n] = 0;
    return String(name);
  }
  return "";
}

// ===== GAP callback =====
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param){
  switch(event){
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED){
        scanClear();
        g_scanning = true;
        logLn("SCAN START");
      } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED){
        g_scanning = false;
        logF("SCAN DONE COUNT=%d\n", g_scanCount);
      }
    } break;

    case ESP_BT_GAP_DISC_RES_EVT: {
      if (g_a2dpConnected) break;
      if (!g_scanning) break;

      int rssi = 0;
      String name = "";

      for (int i=0; i<param->disc_res.num_prop; i++){
        auto &p = param->disc_res.prop[i];
        if (p.type == ESP_BT_GAP_DEV_PROP_RSSI){
          rssi = *(int8_t*)p.val;
        } else if (p.type == ESP_BT_GAP_DEV_PROP_EIR){
          String n = eirToName((uint8_t*)p.val);
          if (n.length()) name = n;
        }
      }

      if (name.length() == 0) {
        String mac = bdaToStr(param->disc_res.bda);
        int idx = scanFindByMac(mac);
        if (idx >= 0) g_scan[idx].rssi = rssi;
        break;
      }

      scanStore(param->disc_res.bda, rssi, name);

      String mac = bdaToStr(param->disc_res.bda);
      int idx = scanFindByMac(mac);

      logF("DEV %d %s RSSI=%d NAME=\"%s\"\n",
           (idx >= 0 ? idx : 0),
           mac.c_str(),
           rssi,
           name.c_str());
    } break;

    case ESP_BT_GAP_AUTH_CMPL_EVT: {
      String mac = bdaToStr(param->auth_cmpl.bda);
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS){
        logF("EVT GAP AUTH OK MAC=%s NAME=\"%s\"\n", mac.c_str(), (char*)param->auth_cmpl.device_name);
      } else {
        logF("EVT GAP AUTH FAIL MAC=%s STAT=%d\n", mac.c_str(), (int)param->auth_cmpl.stat);
      }
    } break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
      esp_bt_pin_code_t pin_code;
      pin_code[0] = '0';
      pin_code[1] = '0';
      pin_code[2] = '0';
      pin_code[3] = '0';
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
      logF("EVT GAP PIN_REQ MIN16=%d -> REPLY 0000\n", (int)param->pin_req.min_16_digit);
    } break;

    case ESP_BT_GAP_CFM_REQ_EVT: {
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      logF("EVT GAP CFM_REQ NUM=%lu -> ACCEPT\n", (unsigned long)param->cfm_req.num_val);
    } break;

    case ESP_BT_GAP_KEY_NOTIF_EVT: {
      logF("EVT GAP KEY_NOTIF PASSKEY=%lu\n", (unsigned long)param->key_notif.passkey);
    } break;

    case ESP_BT_GAP_KEY_REQ_EVT: {
      logLn("EVT GAP KEY_REQ");
    } break;

    default:
      break;
  }
}

// ===== A2DP eventy =====
static void on_conn_state(esp_a2d_connection_state_t state, void *){
  if (g_mode == MODE_OFF){
    return;
  }

  const char* s =
    (state==ESP_A2D_CONNECTION_STATE_DISCONNECTED) ? "DISCONNECTED" :
    (state==ESP_A2D_CONNECTION_STATE_CONNECTING)   ? "CONNECTING" :
    (state==ESP_A2D_CONNECTION_STATE_CONNECTED)    ? "CONNECTED" :
    (state==ESP_A2D_CONNECTION_STATE_DISCONNECTING)? "DISCONNECTING" : "UNKNOWN";

  logF("EVT A2DP_CONN %s MAC=%s NAME=\"%s\"\n",
       s,
       g_connMac.length()?g_connMac.c_str():"None",
       g_connName.c_str());

  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED){
    g_a2dpConnected = true;
    g_connectInProgress = false;
    g_lastAudioDataMs = millis();
    reset_connect_retry_state();
    g_cbCalls = 0;
    rb_clear();
    g_rbState = RB_PROCESS;
    g_phase_q16 = 0;
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    delay(100);
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    if (!g_connMac.length() && g_targetMac.length()) {
      g_connMac = g_targetMac;
    }
  }

  if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED){
    g_a2dpConnected = false;
    g_lastAudioDataMs = 0;
    g_cbCalls = 0;     // Reset callback counter on disconnect
    g_cbSilent = 0;    // Reset silence counter
    g_cbUnderrun = 0;  // Reset underrun counter
    g_rbState = RB_PREFETCH;  // Reset ringbuffer state machine
    rb_clear();
    g_phase_q16 = 0;

    // Ignore one locally-triggered disconnect used to restart the raw stream before connect.
    if (g_ignoreLocalDisconnectOnce){
      g_ignoreLocalDisconnectOnce = false;
      clear_connect_retry_timer();
      logLn("EVT A2DP_CONN LOCAL_RESET_IGNORED");
      return;
    }

    if (g_connectInProgress && g_targetMac.length() && g_connectRetryCount < CONNECT_RETRY_MAX){
      g_connectRetryCount++;
      g_connectRetryAtMs = millis() + CONNECT_RETRY_DELAY_MS;
      g_connectInProgress = false;
      logF("EVT A2DP_CONN RETRY %u IN %lums MAC=%s\n",
           (unsigned)g_connectRetryCount,
           (unsigned long)CONNECT_RETRY_DELAY_MS,
           g_targetMac.c_str());
      return;
    }

    clear_connect_retry_timer();

    if (g_connectInProgress || g_targetMac.length()) {
      if (!g_connMac.length() && g_targetMac.length()) {
        g_connMac = g_targetMac;
      }
    } else {
      g_connMac = "";
      g_connName = "";
    }
  }

  if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTING){
    g_a2dpConnected = false;
  }
}

static void on_audio_state(esp_a2d_audio_state_t state, void *){
  const char* s =
    (state==ESP_A2D_AUDIO_STATE_STOPPED)   ? "STOPPED" :
    (state==ESP_A2D_AUDIO_STATE_STARTED)   ? "STARTED" :
    (state==ESP_A2D_AUDIO_STATE_SUSPENDED) ? "SUSPENDED" : "UNKNOWN";

  logF("EVT A2DP_AUDIO %s RB=%lu\n", s, (unsigned long)rb_count_frames());
}

// ===== A2DP data callback =====
// A2DP prosi o len bajtów PCM (stereo 16-bit)
static int32_t get_data(uint8_t *data, int32_t len){
  g_lastAudioDataMs = millis();
  g_cbCalls++;

  // jeśli nie nadajemy, to cisza
  if (!g_btReady || g_mode == MODE_OFF){
    g_cbSilent++;
    memset(data, 0, len);
    return len;
  }

  // ile frames wyjściowych potrzeba
  uint32_t outFrames = (uint32_t)(len / 4);
  int16_t *out = (int16_t*)data;

  SrcRate sr = g_srcRate;

  // jeśli jeszcze nie wykryło, to próbuj zachować się bezpiecznie: cisza
  if (sr == SRC_UNKNOWN){
    g_cbSilent++;
    memset(data, 0, len);
    return len;
  }

  // ===== 3-state ringbuffer machine =====
  uint32_t rbLevel = rb_count_frames();
  
  // State transitions based on ringbuffer water level
  if (g_rbState == RB_PREFETCH){
    if (rbLevel > RB_PREFETCH_THRESHOLD){
      g_rbState = RB_PROCESS;  // enough data, start streaming
    }
  } else if (g_rbState == RB_PROCESS){
    if (rbLevel < RB_PROCESS_MIN_THRESHOLD){
      g_rbState = RB_PREFETCH;  // buffer depleted, back to prefetch
    } else if (rbLevel > RB_DROP_THRESHOLD){
      g_rbState = RB_DROP;  // buffer overfilled, start dropping
    }
  } else if (g_rbState == RB_DROP){
    if (rbLevel < RB_DROP_MIN_THRESHOLD){
      g_rbState = RB_PROCESS;  // buffer cleared, back to normal
    }
  }
  
  // State-specific handling
  if (g_rbState == RB_PREFETCH){
    // Prefetch: return silence, don't consume ringbuffer
    memset(data, 0, len);
    g_cbSilent++;
    return len;
  }
  
  if (g_rbState == RB_DROP){
    // Overflow recovery: drop stale history, keep only the freshest audio.
    portENTER_CRITICAL(&rb_mux);
    uint32_t avail = rb_count_frames();
    if (avail > RB_DROP_MIN_THRESHOLD) {
      rb_r = (rb_w + RB_FRAMES - RB_DROP_MIN_THRESHOLD) % RB_FRAMES;
    }
    portEXIT_CRITICAL(&rb_mux);
    g_rbState = RB_PROCESS;
  }

  if (sr == SRC_44100){
    // passthrough: pop tyle frames ile się da, resztę uzupełnij zerami
    uint32_t got = rb_pop_frames(out, outFrames);
    if (got < outFrames){
      g_cbUnderrun++;
      uint32_t missing = outFrames - got;
      memset(out + got*2, 0, missing * 4);
    }
    return len;
  }

  // sr == SRC_48000: resampling 48 -> 44.1
  // potrzebujemy dwóch kolejnych frames do interpolacji
  for(uint32_t i=0;i<outFrames;i++){
    // phase_q16 mówi ile wejściowych frames "przeszliśmy"
    uint32_t idxInt = (g_phase_q16 >> 16);
    uint32_t frac   = (g_phase_q16 & 0xFFFF);

    // upewnij się, że w buforze jest idxInt+1
    // najprościej: dropnij idxInt frames, potem peek 2
    if (idxInt > 0){
      rb_drop_frames(idxInt);
      g_phase_q16 &= 0xFFFF; // zostaw tylko frac
    }

    int16_t l0,r0,l1,r1;
    if (!rb_peek_two(l0,r0,l1,r1)){
      // brak danych => cisza
      g_cbUnderrun++;
      out[i*2 + 0] = 0;
      out[i*2 + 1] = 0;
      // nie przesuwaj fazy za agresywnie, ale też nie stój w miejscu
      g_phase_q16 += g_step_q16;
      continue;
    }

    // linear interpolation
    int32_t dl = (int32_t)l1 - (int32_t)l0;
    int32_t dr = (int32_t)r1 - (int32_t)r0;

    int32_t l = (int32_t)l0 + ((dl * (int32_t)frac) >> 16);
    int32_t r = (int32_t)r0 + ((dr * (int32_t)frac) >> 16);

    if (RESAMPLER_QUALITY == 0){
      out[i*2 + 0] = clamp16_from_i32(l);
      out[i*2 + 1] = clamp16_from_i32(r);
    } else {
      out[i*2 + 0] = resampler_hq_filter_i32(l, g_rsLpL1, g_rsLpL2);
      out[i*2 + 1] = resampler_hq_filter_i32(r, g_rsLpR1, g_rsLpR2);
    }

    g_phase_q16 += g_step_q16;
  }

  return len;
}

// ===== NVS save/load =====
static void cfg_save(){
  nvs_handle_t h;
  if (nvs_open("btcfg", NVS_READWRITE, &h) != ESP_OK){ logLn("ERR SAVE"); return; }
  nvs_set_i32(h, "mode", (int)g_mode);
  nvs_set_i32(h, "vol", g_vol_ui);
  nvs_set_i32(h, "boost", g_boost_pct);
  nvs_set_str(h, "mac", g_connMac.c_str());
  nvs_commit(h);
  nvs_close(h);
  logLn("OK SAVE");
}

static void cfg_load(){
  nvs_handle_t h;
  if (nvs_open("btcfg", NVS_READONLY, &h) != ESP_OK) return;

  int32_t m=0, v=100, b=100;
  size_t len=0;

  if (nvs_get_i32(h, "mode", &m) == ESP_OK) g_mode = (Mode)m;
  if (nvs_get_i32(h, "vol", &v) == ESP_OK) g_vol_ui = (int)v;
  if (nvs_get_i32(h, "boost", &b) == ESP_OK) g_boost_pct = (int)b;

  nvs_get_str(h, "mac", nullptr, &len);
  if (len > 1 && len < 32){
    char buf[32];
    if (nvs_get_str(h, "mac", buf, &len) == ESP_OK) g_connMac = String(buf);
  }
  nvs_close(h);

  if (g_vol_ui < 0) g_vol_ui = 0;
  if (g_vol_ui > 100) g_vol_ui = 100;
  g_vol_127 = (uint8_t)lround((double)g_vol_ui * 127.0 / 100.0);

  if (g_boost_pct < 100) g_boost_pct = 100;
  if (g_boost_pct > 400) g_boost_pct = 400;
}

// ===== BT start (raz) =====
static void ensureBtStarted(){
  if (g_btReady) return;

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
    nvs_flash_erase();
    nvs_flash_init();
  }

  // WS interrupt (frequency detect)
  pinMode(PIN_I2S_WS, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_I2S_WS), ws_isr, RISING);

  i2s_init_slave_rx();

  // A2DP
  a2dp.set_local_name("WROOM-BT-TX");
  a2dp.set_ssp_enabled(true);
  a2dp.set_ssid_callback(on_ssid_found);
  a2dp.set_auto_reconnect(false);
  a2dp.set_on_connection_state_changed(on_conn_state);
  a2dp.set_on_audio_state_changed(on_audio_state);

  a2dp.start_raw(get_data);
  a2dp.set_volume(g_vol_127);
  setBtTxPowerMax();

  rb_clear();
  g_srcRate = SRC_UNKNOWN;
  g_srcHz = 0;
  g_i2sReadsOk = 0;
  g_i2sReadsErr = 0;
  g_i2sZeroReads = 0;
  g_i2sBytes = 0;
  g_cbCalls = 0;
  g_cbSilent = 0;
  g_cbUnderrun = 0;

  // tasks
  xTaskCreatePinnedToCore(rate_task, "rate_task", 4096, nullptr, 2, &g_rateTask, 1);
  xTaskCreatePinnedToCore(pcm_task,  "pcm_task",  4096, nullptr, 2, &g_pcmTask,  1);

  g_btReady = true;
}

// ===== actions =====
static void status_send(){
  const char* sr =
    (g_srcRate==SRC_44100) ? "44100" :
    (g_srcRate==SRC_48000) ? "48000" : "UNKNOWN";
  
  const char* rbState =
    (g_rbState==RB_PREFETCH) ? "PREFETCH" :
    (g_rbState==RB_DROP) ? "DROP" : "PROCESS";

  logF("STATE BT=%s MODE=%s VOL=%d(127=%d) BOOST=%d SRC=%s(%dHz) RB=%lu[%s] SCAN=%d CONN=%d MAC=%s NAME=\"%s\" I2S_OK=%lu I2S_ERR=%lu I2S_ZR=%lu I2S_B=%lu CB=%lu CBS=%lu CBU=%lu\n",
    g_btReady ? "ON":"OFF",
    g_mode==MODE_OFF?"OFF":(g_mode==MODE_TX?"TX":"AUTO"),
    g_vol_ui,
    (int)g_vol_127,
    g_boost_pct,
    sr,
    g_srcHz,
    (unsigned long)rb_count_frames(),
    rbState,
    g_scanning ? 1:0,
    g_a2dpConnected ? 1:0,
    g_a2dpConnected && g_connMac.length()?g_connMac.c_str():"None",
    g_a2dpConnected?g_connName.c_str():"",
    (unsigned long)g_i2sReadsOk,
    (unsigned long)g_i2sReadsErr,
    (unsigned long)g_i2sZeroReads,
    (unsigned long)g_i2sBytes,
    (unsigned long)g_cbCalls,
    (unsigned long)g_cbSilent,
    (unsigned long)g_cbUnderrun
  );
}

static void soft_off(){
  reset_connect_retry_state();
  if (g_scanning){
    esp_bt_gap_cancel_discovery();
    g_scanning = false;
  }
  a2dp.disconnect();
  g_a2dpConnected = false;
  g_connectInProgress = false;
  g_targetMac = "";
  g_connMac = "";
  g_connName = "";
  g_mode = MODE_OFF;
  logLn("OK MODE OFF");
}

static void scan_start(){
  ensureBtStarted();

  reset_connect_retry_state();
  
  if (g_a2dpConnected || g_connectInProgress){
    g_ignoreLocalDisconnectOnce = false;
    g_a2dpConnected = false;
    g_connectInProgress = false;
    g_targetMac = "";
    g_connMac = "";
    g_connName = "";
    a2dp.disconnect();
  }

  if (g_scanning){
    esp_bt_gap_cancel_discovery();
    g_scanning = false;
    vTaskDelay(pdMS_TO_TICKS(CONNECT_POST_SCAN_DELAY_MS));
  }

  scanClear();
  g_targetMac = "";
  g_scanning = true;
  g_scanStartedMs = millis();
  logLn("OK SCAN START");
  logLn("SCAN START");
  start_gap_scan();
}

static void scan_stop(){
  if (!g_scanning) {
    logLn("OK SCAN STOP");
    return;
  }

  esp_bt_gap_cancel_discovery();
  g_scanning = false;
  logLn("OK SCAN STOP");
}

static void connect_mac(const String& mac){

  if (g_mode == MODE_OFF){
    g_mode = MODE_TX;
    logLn("OK MODE TX (AUTO)");
  }

  esp_bd_addr_t bda{};
  if (!parseMac(mac, bda)){
    logF("ERR CONNECT MAC (invalid format): %s\n", mac.c_str());
    return;
  }

  int idx = scanFindByMac(mac);
  if (idx >= 0) g_connName = g_scan[idx].name; else g_connName = "";
  g_connMac = mac;
  g_targetMac = mac;
  g_connectInProgress = true;
  g_scanning = false;

  // If already connected to another device, drop the old link once before switching.
  if (g_a2dpConnected){
    g_ignoreLocalDisconnectOnce = true;
    a2dp.disconnect();
    vTaskDelay(pdMS_TO_TICKS(120));
  }

  esp_err_t err = esp_a2d_source_connect(bda);
  if (err != ESP_OK){
    g_connectInProgress = false;
    logF("ERR CONNECT %s A2DP=%d\n", mac.c_str(), (int)err);
    return;
  }

  logF("OK CONNECT %s NAME=\"%s\"\n", mac.c_str(), g_connName.c_str());
}

static void connect_idx(int idx){
  if (idx < 0 || idx >= g_scanCount || !g_scan[idx].valid){
    logLn("ERR CONNECT IDX");
    return;
  }
  String mac = bdaToStr(g_scan[idx].bda);
  g_connName = g_scan[idx].name;
  reset_connect_retry_state();
  connect_mac(mac);
}

static void disconnect_bt(){
  reset_connect_retry_state();
  a2dp.disconnect();
  g_a2dpConnected = false;
  g_connectInProgress = false;
  g_targetMac = "";
  g_connMac = "";
  g_connName = "";
  logLn("OK DISCONNECT");
}

static void paired_list(){
  ensureBtStarted();
  int n = esp_bt_gap_get_bond_device_num();
  if (n <= 0){
    logLn("PAIRED DONE COUNT=0");
    return;
  }
  esp_bd_addr_t *list = (esp_bd_addr_t*)malloc(n * sizeof(esp_bd_addr_t));
  if (!list){ logLn("ERR MEM"); return; }
  esp_bt_gap_get_bond_device_list(&n, list);
  for (int i=0;i<n;i++){
    logF("PAIRED %d %s NAME=\"\"\n", i, bdaToStr(list[i]).c_str());
  }
  free(list);
  logF("PAIRED DONE COUNT=%d\n", n);
}

// ===== DELPAIRED =====
static TaskHandle_t g_delTask = nullptr;

static void delpaired_task(void *){
  ensureBtStarted();

  reset_connect_retry_state();
  a2dp.disconnect();
  if (g_scanning){
    esp_bt_gap_cancel_discovery();
    g_scanning = false;
  }

  int n = esp_bt_gap_get_bond_device_num();
  if (n <= 0){
    logLn("OK DELPAIRED 0");
    g_delTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  esp_bd_addr_t *list = (esp_bd_addr_t*)malloc(n * sizeof(esp_bd_addr_t));
  if (!list){
    logLn("ERR MEM");
    g_delTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  esp_bt_gap_get_bond_device_list(&n, list);
  int removed = 0;
  for (int i=0;i<n;i++){
    if (esp_bt_gap_remove_bond_device(list[i]) == ESP_OK) removed++;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  free(list);

  nvs_handle_t h;
  if (nvs_open("btcfg", NVS_READWRITE, &h) == ESP_OK){
    nvs_erase_key(h, "mac");
    nvs_commit(h);
    nvs_close(h);
  }

  g_a2dpConnected = false;
  g_connectInProgress = false;
  g_targetMac = "";
  g_connMac = "";
  g_connName = "";
  logF("OK DELPAIRED %d\n", removed);

  g_delTask = nullptr;
  vTaskDelete(nullptr);
}

static void delpaired_all_async(){
  if (g_delTask){
    logLn("ERR DELPAIRED BUSY");
    return;
  }
  logLn("OK DELPAIRED START");
  xTaskCreatePinnedToCore(delpaired_task, "delpaired", 4096, nullptr, 1, &g_delTask, 1);
}

// ===== cmd parser =====
static void help_print(){
  logLn("CMDS: HELP, PING, GET, STATUS?, BT ON, BT OFF, MODE OFF|TX (AUTO=legacy), VOL 0..100, BOOST 100..400, SCAN, SCAN STOP, CONNECT <idx|MAC>, DISCONNECT, PAIRED?, DELPAIRED ALL, SAVE, DBG 0|1, HARDRESET");
}

static void handle_cmd(String s, const char* src){
  s.trim();
  if (!s.length()) return;

  logF("CMD[%s] \"%s\"\n", src, s.c_str());

  String u = s;
  u.toUpperCase();

  if (u == "PING"){ logLn("PONG"); return; }
  if (u == "HELP"){ help_print(); return; }

  if (u.startsWith("DBG ")){
    int v = s.substring(4).toInt();
    USB_DEBUG = (v != 0);
    logF("OK DBG %d\n", USB_DEBUG ? 1 : 0);
    return;
  }

  if (u=="GET" || u=="STATUS?"){ status_send(); return; }

  if (u=="BT ON"){ ensureBtStarted(); logLn("OK BT ON"); return; }
  if (u=="BT OFF"){ soft_off(); return; }

  if (u.startsWith("MODE ")){
    String m = s.substring(5); m.trim();
    m.toUpperCase();
    if (m=="OFF"){ soft_off(); return; }
    if (m=="TX"){ ensureBtStarted(); g_mode=MODE_TX; logLn("OK MODE TX"); return; }
    if (m=="AUTO"){ ensureBtStarted(); g_mode=MODE_AUTO; logLn("OK MODE AUTO"); return; }
    logLn("ERR MODE"); return;
  }

  if (u.startsWith("VOL ")){
    int v = s.substring(4).toInt();
    if (v<0) v=0;
    if (v>100) v=100;
    g_vol_ui = v;
    g_vol_127 = (uint8_t)lround((double)g_vol_ui * 127.0 / 100.0);
    if (g_btReady) a2dp.set_volume(g_vol_127);
    logF("OK VOL %d\n", g_vol_ui);
    return;
  }

  if (u.startsWith("BOOST ")){
    int b = s.substring(6).toInt();
    if (b < 100) b = 100;
    if (b > 400) b = 400;
    g_boost_pct = b;
    logF("OK BOOST %d\n", g_boost_pct);
    return;
  }

  if (u=="SCAN"){ scan_start(); return; }
  if (u=="SCAN STOP"){ scan_stop(); return; }

  if (u.startsWith("CONNECT ")){
    String a = s.substring(8); a.trim();
    String aUp = a;
    aUp.toUpperCase();
    if (aUp.startsWith("CONNECT ")){
      a = a.substring(8);
      a.trim();
    }

    if (a.indexOf(':') >= 0){ reset_connect_retry_state(); connect_mac(a); return; }
    int idx = a.toInt();
    connect_idx(idx);
    return;
  }

  if (u=="DISCONNECT"){ disconnect_bt(); return; }
  if (u=="PAIRED?"){ paired_list(); return; }
  if (u=="DELPAIRED ALL"){ delpaired_all_async(); return; }
  if (u=="SAVE"){ cfg_save(); return; }

  if (u=="HARDRESET"){ logLn("OK HARDRESET"); delay(50); ESP.restart(); return; }

  logLn("ERR UNKNOWN");
}

// ===== read line =====
static bool readLineFrom(Stream& st, String& buf, String& outLine){
  while (st.available()){
    char c = (char)st.read();
    if (c == '\n'){
      outLine = buf;
      buf = "";
      return outLine.length() > 0;
    } else if (c != '\r'){
      buf += c;
      if (buf.length() > 300) buf = "";
    }
  }
  return false;
}

void setup(){
  Serial.begin(115200);
  CTRL.begin(UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

  logLn("READY WROOM-BT-TX v1.6.6 (CB-reset enabled)");

  cfg_load();

  // BEZPIECZNIK STARTOWY (Dla synchronizacji z S3):
  // Czekamy 10 sekund (10000 ms). Dajemy czas na pełny rozruch ESP32-S3,
  // uruchomienie ekranu TFT oraz stabilizację linii zegarowych I2S.
  vTaskDelay(pdMS_TO_TICKS(10000));
  ensureBtStarted();
}

void loop(){
  static String bufUart, bufUsb, line;

  if (g_connectRetryAtMs && (int32_t)(millis() - g_connectRetryAtMs) >= 0){
    String retryMac = g_targetMac;
    g_connectRetryAtMs = 0;
    if (retryMac.length()){
      logF("EVT A2DP_CONN RETRY CONNECT MAC=%s\n", retryMac.c_str());
      connect_mac(retryMac);
      return;
    }
  }

  if (g_scanning){
    const uint32_t scanAgeMs = millis() - g_scanStartedMs;

    if (scanAgeMs >= SCAN_WINDOW_MS){
      esp_bt_gap_cancel_discovery();
      g_scanning = false;
      logF("SCAN DONE COUNT=%d\n", g_scanCount);
    }
  }

  // v1.6.5c: disable media restart loop here; CONNECTED must not be retriggered by watchdog.

  if (readLineFrom(CTRL, bufUart, line)){
    handle_cmd(line, "UART");
  }
  if (readLineFrom(Serial, bufUsb, line)){
    handle_cmd(line, "USB");
  }

  delay(1);
}
