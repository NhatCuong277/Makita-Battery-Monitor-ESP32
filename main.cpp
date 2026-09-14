// ============================================================
//  Makita Battery Monitor / Unlock / Omega Lock Utility
//  ESP32-C3 Super Mini edition — Wi-Fi AP + mobile web UI
//
//  Scan mode      : full battery read-out (optional auto-unlock)
//  Omega lock mode: sets charger lock nybble (nybble 34) non-zero
//
//  Mode select :  bridge GPIO5 -> GPIO7  =>  OMEGA LOCK  (red pulse)
//                 both pins open         =>  SCAN/UNLOCK (white pulse)
//
//  Wiring (ESP32-C3 Super Mini):
//     GPIO2  -> Makita DATA   (1-Wire, 4.7k pull-up to 3V3, 120R series)
//     GPIO1  -> Makita ENABLE (4.7k pull-up to 3V3, 120R series)
//     GPIO10 -> NeoPixel DIN
//     GPIO5 / GPIO7 -> mode select bridge
//
//  Wi-Fi : AP "Makita Battery Monitor", open, http://192.168.4.1
//
//  Build : PlatformIO + OneWire2 (vendored fork)
//
//  License: Provided as-is, no warranty. Interacting with battery
//           pack firmware can permanently brick a pack. Use at your
//           own risk.
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include "OneWire2.h"  // vendored fork of OneWire with tweaks for Makita timing

// ─── Firmware identity ────────────────────────────────────────
#define FIRMWARE_VERSION "2.0.1-c3web"

// ─── Pin definitions (ESP32-C3 Super Mini) ───────────────────
#ifndef PIN_ONEWIRE_CFG
  #define PIN_ONEWIRE_CFG 2
#endif
#ifndef PIN_ENABLE_CFG
  #define PIN_ENABLE_CFG 1
#endif
#ifndef PIN_NEOPIXEL_CFG
  #define PIN_NEOPIXEL_CFG 10
#endif
#ifndef PIN_MODE_OUT_CFG
  #define PIN_MODE_OUT_CFG 5
#endif
#ifndef PIN_MODE_IN_CFG
  #define PIN_MODE_IN_CFG 7
#endif

static constexpr uint8_t PIN_ONEWIRE      = PIN_ONEWIRE_CFG;
static constexpr uint8_t PIN_ENABLE       = PIN_ENABLE_CFG;
static constexpr uint8_t NEOPIXEL_OUT_PIN = PIN_NEOPIXEL_CFG;
static constexpr uint8_t NUM_PIXELS       = 1;
static constexpr uint8_t PIN_MODE_OUT     = PIN_MODE_OUT_CFG;
static constexpr uint8_t PIN_MODE_IN1     = PIN_MODE_IN_CFG;  // bridge GPIO5-GPIO7 = omega lock

// PIN_MODE_OUT drives HIGH, the input uses INPUT_PULLDOWN, so a bridge pulls it HIGH.
static constexpr bool MODE_PIN_ACTIVE_HIGH = true;

// ─── Wi-Fi access point ──────────────────────────────────────
static const char *AP_SSID     = "Makita Battery Monitor";
static const char *AP_PASSWORD = nullptr;      // open network
#ifndef AP_TX_POWER
  #define AP_TX_POWER WIFI_POWER_11dBm
#endif
// Diagnostic builds: each task can be left out to isolate a fault.
#ifndef WIFI_ONLY_TEST
  #define WIFI_ONLY_TEST 0
#endif
#ifndef ENABLE_LED_TASK
  #define ENABLE_LED_TASK (!WIFI_ONLY_TEST)
#endif
#ifndef ENABLE_BATTERY_TASK
  #define ENABLE_BATTERY_TASK (!WIFI_ONLY_TEST)
#endif
static constexpr uint8_t AP_CHANNEL     = 6;
static constexpr uint8_t AP_MAX_CLIENTS = 4;
static constexpr uint16_t HTTP_PORT     = 80;
static constexpr uint16_t DNS_PORT      = 53;

// ─── Log ring buffer (mirrors every serial line to the web UI) ─
static constexpr uint16_t LOG_MAX_LINES = 160;
static constexpr uint16_t LOG_MAX_RESPONSE_LINES = 60;   // per /api/state reply
static constexpr uint8_t  LOG_LINE_LEN  = 100;

class LogStream : public Print {
public:
    void begin(unsigned long baud) { Serial.begin(baud); }
    int  available()               { return Serial.available(); }
    int  read()                    { return Serial.read(); }

    size_t write(uint8_t c) override {
        Serial.write(c);
        if (c == '\r') return 1;
        if (c == '\n') { push_line(); return 1; }
        if (m_cur_len < LOG_LINE_LEN - 1) m_cur[m_cur_len++] = (char)c;
        return 1;
    }
    size_t write(const uint8_t *b, size_t n) override {
        for (size_t i = 0; i < n; i++) write(b[i]);
        return n;
    }

    void clear() {
        lock(); m_count = 0; m_head = 0; m_cur_len = 0; unlock();
    }
    uint32_t seq() { return m_seq; }

    // Appends every buffered line newer than `since` to `out` as JSON strings.
    // Returns the sequence number the caller has now consumed.
    uint32_t collect(uint32_t since, String &out);

    void attach_mutex(SemaphoreHandle_t m) { m_mutex = m; }

private:
    void lock()   { if (m_mutex) xSemaphoreTakeRecursive(m_mutex, portMAX_DELAY); }
    void unlock() { if (m_mutex) xSemaphoreGiveRecursive(m_mutex); }
    void push_line() {
        lock();
        m_cur[m_cur_len] = '\0';
        memcpy(m_lines[m_head], m_cur, m_cur_len + 1);
        m_head = (uint16_t)((m_head + 1) % LOG_MAX_LINES);
        if (m_count < LOG_MAX_LINES) m_count++;
        m_seq++;
        m_cur_len = 0;
        unlock();
    }

    char              m_lines[LOG_MAX_LINES][LOG_LINE_LEN];
    char              m_cur[LOG_LINE_LEN] = {0};
    uint8_t           m_cur_len = 0;
    uint16_t          m_head    = 0;
    uint16_t          m_count   = 0;
    uint32_t          m_seq     = 0;
    SemaphoreHandle_t m_mutex   = nullptr;
};

static LogStream LOG;

static void json_escape(const char *s, String &out) {
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)*p < 0x20) { char b[7]; snprintf(b, sizeof(b), "\\u%04X", *p); out += b; }
                else out += *p;
        }
    }
}

uint32_t LogStream::collect(uint32_t since, String &out) {
    lock();
    uint32_t first = (m_seq > m_count) ? (m_seq - m_count) : 0;   // seq of oldest kept line
    if (since < first) since = first;
    bool comma = false;
    for (uint32_t s = since; s < m_seq; s++) {
        uint16_t idx = (uint16_t)((m_head + LOG_MAX_LINES - (m_seq - s)) % LOG_MAX_LINES);
        if (comma) out += ',';
        out += '"';
        json_escape(m_lines[idx], out);
        out += '"';
        comma = true;
    }
    uint32_t now_seq = m_seq;
    unlock();
    return now_seq;
}

// ─── 1-Wire bus timing ───────────────────────────────────────
static constexpr uint16_t BUS_BYTE_GAP_US    = 90;   // inter-byte gap on 1-Wire bus
static constexpr uint16_t BUS_SETTLE_US      = 100;  // settle before reading response bytes
static constexpr uint16_t BUS_SETTLE_ON_MS   = 150;  // bus power-up settle after ENABLE=HIGH
//                                                       (covers: enable, presence poll, powercycle-on)
static constexpr uint16_t POWERCYCLE_OFF_MS  = 100;  // bus off time during power cycle

// ─── Inter-command delays ─────────────────────────────────────
// All inter-step delays use 30ms — consistent across frame-write,
// testmode, and Type 5 session sequences.
static constexpr uint16_t INTER_CMD_MS       = 30;

// ─── Type 5 (F0513) constants ─────────────────────────────────
static constexpr uint8_t  TYPE5_CMD_SESSION  = 0x99; // enters voltage read session
static constexpr uint8_t  TYPE5_CMD_MODEL    = 0x31; // reads model bytes
static constexpr uint8_t  TYPE5_CMD_TEMP     = 0x52; // reads cell temperature
// Some Type 5 variants require multiple read attempts before all
// cells report valid data — this is a hardware ADC property.
static constexpr uint8_t  TYPE5_MAX_ATTEMPTS = 10;    // max read attempts per scan

// ─── Type 6 constants ─────────────────────────────────────────
static constexpr uint8_t  TYPE6_VOLT_READ    = 0xD4; // voltage read prefix
static constexpr float    TEMP6_A            = 9323.0f;
static constexpr float    TEMP6_B            = -40.0f;

// ─── Protocol commands ────────────────────────────────────────
static constexpr uint8_t CMD_BASIC_INFO[]     = { 0xAA, 0x00 };
static constexpr uint8_t CMD_MODEL[]          = { 0xDC, 0x0C };
static constexpr uint8_t CMD_TESTMODE_ENTER[] = { 0xD9, 0x96, 0xA5 };
static constexpr uint8_t CMD_TESTMODE_EXIT[]  = { 0xD9, 0xFF, 0xFF };
static constexpr uint8_t CMD_RESET_ERRORS[]   = { 0xDA, 0x04 };
static constexpr uint8_t CMD_CC_F0[]          = { 0xF0, 0x00 }; // ADC trigger / frame-write arm
static constexpr uint8_t CMD_STORE[]          = { 0x55, 0xA5 };
static constexpr uint8_t FRAME_WRITE_OPCODE   = 0x0F;
static constexpr uint8_t FRAME_WRITE_PAD      = 0x00;
static constexpr uint8_t BASIC_INFO_LEN       = 32;
static constexpr uint8_t TYPE_PROBE_MAGIC     = 0x06;
static constexpr float    TEMP_INVALID         = -999.0f;
static constexpr uint8_t  MAX_CELLS            = 10;

// ─── Health / battery data ────────────────────────────────────
static constexpr uint8_t  HEALTH_SCALE_EXTENDED_CODES[] = { 26, 28, 40, 50 };
static constexpr size_t   HEALTH_SCALE_EXTENDED_COUNT   =
    sizeof(HEALTH_SCALE_EXTENDED_CODES) / sizeof(HEALTH_SCALE_EXTENDED_CODES[0]);
static constexpr uint16_t HEALTH_SCALE_STANDARD = 600;
static constexpr uint16_t HEALTH_SCALE_EXTENDED = 1000;
static constexpr float    HEALTH_MAX            = 4.0f;
static constexpr float    SOC_DIVISOR           = 2880.0f;

// ─── Scan / mode timing ───────────────────────────────────────
static constexpr uint16_t POLL_INTERVAL_MS      = 200;
static constexpr uint16_t MODE_DEBOUNCE_MS      = 50;
static constexpr uint8_t  MODE_DEBOUNCE_COUNT   = 4;

// ─── No-response polling ─────────────────────────────────────
// Chip goes bus-silent for ~3s during frame commit. 10s failsafe = 3x headroom.
static constexpr uint32_t NO_RESPONSE_TIMEOUT_MS = 10000;
static constexpr uint16_t NO_RESPONSE_POLL_MS    = 25;

// ─── Unlock / lock attempt limits ────────────────────────────
static constexpr uint8_t  UNLOCK_MAX_CYCLES     = 6;
static constexpr uint8_t  LOCK_MAX_ATTEMPTS     = 6;

// ─── LED parameters ──────────────────────────────────────────
static constexpr uint8_t  LED_BRIGHTNESS_MAX         = 80;
static constexpr uint8_t  LED_FLASH_COUNT            = 3;
static constexpr uint16_t LED_FLASH_ON_MS            = 200;
static constexpr uint16_t LED_FLASH_OFF_MS           = 100;
static constexpr uint16_t LED_PULSE_PERIOD_MS        = 2000;
static constexpr uint8_t  LED_PULSE_INTERVAL_MS      = 40;
static constexpr float    IMBALANCE_THRESHOLD_V      = 0.300f;
static constexpr uint16_t IMBALANCE_BLINK_INTERVAL_MS = 500;
static constexpr uint16_t IMBALANCE_BLINK_ON_MS      = 80;

// ─── Watchdog ────────────────────────────────────────────────
// Must exceed NO_RESPONSE_TIMEOUT_MS (10s). Set to 12s.
static constexpr uint32_t WATCHDOG_TIMEOUT_MS = 12000;

// ─── Lock cause flags (bitfield) ─────────────────────────────
// Confirmed by systematic charger validation testing (193 fields tested).
static constexpr uint16_t LF_CS0  = 0x0002;  // CS0 mismatch (nybbles 0-15)
static constexpr uint16_t LF_CS2  = 0x0008;  // CS2 mismatch (nybbles 32-40)
static constexpr uint16_t LF_N34  = 0x0040;  // nybble 34 != 0 (charger lock nybble)


// ─── Return codes / types ─────────────────────────────────────
enum BasicInfoResult { BASIC_INFO_NO_RESPONSE = 0, BASIC_INFO_OK = 1, BASIC_INFO_PRE_TYPE0 = -1 };
enum BusResult       { BUS_OK = 0, BUS_NO_PRESENCE = 1 };

// Failure codes stored in nybble 40 of the basic-info frame.
enum FailureCode : uint8_t {
    FC_OK         = 0,
    FC_OVERLOADED = 1,
    FC_WARNING    = 5,
    FC_DEAD       = 15,
};

enum class BatteryType : int8_t {
    UNKNOWN = -1,
    T0      = 0,
    T2      = 2,
    T3      = 3,
    T5      = 5,
    T6      = 6,
};

static inline int  batt_type_to_int(BatteryType t) { return static_cast<int>(t); }
static inline bool is_type_023(BatteryType t) {
    return t == BatteryType::T0 || t == BatteryType::T2 || t == BatteryType::T3;
}
static inline bool is_type_56(BatteryType t) {
    return t == BatteryType::T5 || t == BatteryType::T6;
}

// ─── Data structures ─────────────────────────────────────────
struct RawBasicInfo {
    uint8_t  batt_type, capacity, failure_code, overdischarge, overload;
    uint16_t cycles;
    bool     cell_failure;
    uint8_t  checksum_n[3];
    uint8_t  aux_checksum_n[2];
};

struct HealthData {
    float    rating;
    uint16_t overload_count;
    uint8_t  od_count;
    uint32_t charge_level;
};

struct CellVoltages {
    float   v[MAX_CELLS];
    uint8_t n;
    bool    valid;
};

struct VoltageReadResult {
    float        vpack;
    CellVoltages cells;
    float        t_cell;
    float        t_mosfet;
    bool         ok;
};

static inline VoltageReadResult voltage_result_init() {
    VoltageReadResult vr{};
    vr.t_cell   = TEMP_INVALID;
    vr.t_mosfet = TEMP_INVALID;
    return vr;
}

struct BatteryInfo {
    char         model[8];
    uint8_t      rom_id[8];
    bool         rom_id_valid;
    BatteryType  type;
    RawBasicInfo raw;
    HealthData   health;
    float        temperature_c;
    float        temperature_mosfet_c;
    uint8_t      cell_count;
    float        capacity_ah;
    bool         locked;
    bool         checksums_ok[3];
    bool         aux_checksums_ok[2];
    uint16_t     lock_causes;     // bitfield of LF_* flags
};

// ─── Device / scan state ─────────────────────────────────────
enum ScanState { WAIT_BATTERY, SCAN_PENDING, IDLE, UNSUPPORTED };
enum DeviceMode { MODE_SCAN = 0, MODE_LOCK = 1 };

// Actions requested from the web UI (single-slot queue, battery task drains it).
enum PendingAction : uint8_t { ACT_NONE = 0, ACT_SCAN, ACT_UNLOCK, ACT_OMEGA_LOCK };

// Snapshot of the last completed scan — rendered by the web UI.
struct ScanSnapshot {
    bool              valid;          // a scan produced data
    bool              pre_type0;      // unsupported HC08 pack
    uint32_t          timestamp_ms;
    BatteryInfo       info;
    VoltageReadResult vr;
    uint8_t           frame[BASIC_INFO_LEN];
    float             imbalance_v;
    bool              imbalanced;
    char              last_error[64];
};

// ─── Globals ──────────────────────────────────────────────────
static OneWire           g_ow(PIN_ONEWIRE);
// WS2812 driver on a persistent RMT channel.
//
// Adafruit_NeoPixel's ESP32 backend runs rmt_driver_install()/uninstall() —
// which allocates memory and hooks an ISR — on *every* show(). At the pulse
// refresh rate, next to an active Wi-Fi AP, that churn destabilises the radio
// and reboots the board. Here the channel is claimed once and only the 24
// symbols are pushed per update.
static rmt_obj_t *g_rmt = nullptr;

static bool neopixel_begin() {
    g_rmt = rmtInit(NEOPIXEL_OUT_PIN, true /* tx */, RMT_MEM_64);
    if (!g_rmt) return false;
    rmtSetTick(g_rmt, 100.0f);   // 100 ns per tick
    return true;
}

// WS2812B bit cells at 100 ns ticks: '0' = 400 ns high / 900 ns low,
// '1' = 800 ns high / 500 ns low.
static void neopixel_write(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_rmt) return;
    rmt_data_t items[24];
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    for (uint8_t i = 0; i < 24; i++) {
        bool one = grb & (1UL << (23 - i));
        items[i].level0    = 1;
        items[i].duration0 = one ? 8 : 4;
        items[i].level1    = 0;
        items[i].duration1 = one ? 5 : 9;
    }
    rmtWriteBlocking(g_rmt, items, 24);
}
static bool              g_charger_arm_issued = false;
static bool              g_in_testmode        = false;
static volatile bool     g_pulse_active       = false;
static volatile bool     g_pulse_scan_mode    = true;
static volatile bool     g_imbalance          = false;
static volatile uint8_t  g_result_r           = 0;
static volatile uint8_t  g_result_g           = 0;
static volatile uint8_t  g_result_b           = 0;
static uint32_t          g_last_poll          = 0;
static uint8_t           g_mode_debounce      = 0;
static uint32_t          g_last_mode_poll     = 0;

static ScanSnapshot            g_snap{};
static SemaphoreHandle_t       g_snap_mutex   = nullptr;
static SemaphoreHandle_t       g_log_mutex    = nullptr;
static SemaphoreHandle_t       g_led_mutex    = nullptr;
static volatile PendingAction  g_action       = ACT_NONE;
static volatile bool           g_auto_unlock  = false;   // off by default: scan is read-only
static volatile bool           g_busy         = false;
static volatile bool           g_present      = false;
static volatile ScanState      g_state        = WAIT_BATTERY;
static volatile DeviceMode     g_mode         = MODE_SCAN;
static char                    g_activity[40] = "Khoi dong";

static void set_activity(const char *s) {
    strncpy(g_activity, s, sizeof(g_activity) - 1);
    g_activity[sizeof(g_activity) - 1] = '\0';
}

// ─── Watchdog helpers ────────────────────────────────────────
// The battery task is registered with the ESP-IDF task watchdog; every long
// operation kicks it. WiFi/HTTP live on the Arduino loop task, unaffected.
static bool g_wdt_armed = false;

static inline void wdt_kick() {
    if (g_wdt_armed) esp_task_wdt_reset();
}

static inline void wdt_begin() {
    // Arduino's default task watchdog is 5 s with panic enabled, which a slow
    // BMS handshake can legitimately exceed. Widen it before subscribing.
    esp_task_wdt_init(WATCHDOG_TIMEOUT_MS / 1000, true);
    if (esp_task_wdt_add(nullptr) == ESP_OK) g_wdt_armed = true;
}

static void safe_delay(uint32_t ms) {
    const uint32_t chunk = 500;
    while (ms > chunk) { delay(chunk); wdt_kick(); ms -= chunk; }
    delay(ms);
    wdt_kick();
}

// ─── NeoPixel colours ─────────────────────────────────────────
struct Colour { uint8_t r, g, b; };
static constexpr Colour COL_OFF    = {  0,                  0,                  0 };
static constexpr Colour COL_GREEN  = {  0, LED_BRIGHTNESS_MAX,                  0 };
static constexpr Colour COL_YELLOW = { LED_BRIGHTNESS_MAX, 60,                  0 };
static constexpr Colour COL_ORANGE = { LED_BRIGHTNESS_MAX, 40,                  0 };
static constexpr Colour COL_BLUE   = {  0,                  0, LED_BRIGHTNESS_MAX };
static constexpr Colour COL_RED    = { LED_BRIGHTNESS_MAX,  0,                  0 };
static constexpr Colour COL_PURPLE = { LED_BRIGHTNESS_MAX,  0, LED_BRIGHTNESS_MAX };
static constexpr Colour COL_WHITE  = { LED_BRIGHTNESS_MAX, LED_BRIGHTNESS_MAX, LED_BRIGHTNESS_MAX };

static void led_set(Colour c) {
    if (g_led_mutex) xSemaphoreTake(g_led_mutex, portMAX_DELAY);
    neopixel_write(c.r, c.g, c.b);
    if (g_led_mutex) xSemaphoreGive(g_led_mutex);
}
static void led_off()    { led_set(COL_OFF);    }
static void led_yellow() { led_set(COL_YELLOW); }
static void led_blue()   { led_set(COL_BLUE);   }
static void led_purple() { led_set(COL_PURPLE); }

static void led_flash(Colour c) {
    g_pulse_active = false;          // pause core1 during flash
    for (uint8_t i = 0; i < LED_FLASH_COUNT; i++) {
        led_set(c); safe_delay(LED_FLASH_ON_MS);
        led_off();  safe_delay(LED_FLASH_OFF_MS);
    }
    led_set(c);  // stays on after final flash
}

static void led_pulse(Colour c) {
    constexpr uint32_t HALF = LED_PULSE_PERIOD_MS / 2;
    uint32_t t = millis() % LED_PULSE_PERIOD_MS;
    uint8_t  v = (t < HALF)
        ? (uint8_t)(t * LED_BRIGHTNESS_MAX / HALF)
        : (uint8_t)((LED_PULSE_PERIOD_MS - t) * LED_BRIGHTNESS_MAX / HALF);
    Colour sc = {
        (uint8_t)(v * c.r / LED_BRIGHTNESS_MAX),
        (uint8_t)(v * c.g / LED_BRIGHTNESS_MAX),
        (uint8_t)(v * c.b / LED_BRIGHTNESS_MAX),
    };
    led_set(sc);
}

static inline void led_result(bool ok) { led_flash(ok ? COL_GREEN : COL_RED); }

// ─── Mode detection ───────────────────────────────────────────
// Mode 0 = SCAN/UNLOCK  (both pins open)
// Mode 1 = OMEGA LOCK   (GPIO0-GPIO1 bridged)
static DeviceMode g_last_mode = MODE_SCAN;

static inline bool mode_pin_active(uint8_t pin) {
    return digitalRead(pin) == (MODE_PIN_ACTIVE_HIGH ? HIGH : LOW);
}

static DeviceMode mode_read() {
    if (mode_pin_active(PIN_MODE_IN1)) return MODE_LOCK;
    return MODE_SCAN;
}

// ─── Bus control ─────────────────────────────────────────────
static void bus_enable(uint16_t ms = BUS_SETTLE_ON_MS) { digitalWrite(PIN_ENABLE, HIGH); safe_delay(ms); }
static void bus_disable()                            { digitalWrite(PIN_ENABLE, LOW);  }

static void power_cycle_bus(uint16_t off_ms = POWERCYCLE_OFF_MS, uint16_t on_ms = BUS_SETTLE_ON_MS) {
    bus_disable();
    g_charger_arm_issued = false;
    safe_delay(off_ms);
    bus_enable(on_ms);
}

static bool battery_present() {
    digitalWrite(PIN_ENABLE, HIGH);
    safe_delay(BUS_SETTLE_ON_MS);
    bool p = (g_ow.reset() == 1);
    digitalWrite(PIN_ENABLE, LOW);
    return p;
}

// ─── Low-level 1-Wire transactions ───────────────────────────
static BusResult ow_transaction(bool managed, uint8_t rom_prefix,
                                const uint8_t *cmd, uint8_t cmd_len,
                                uint8_t *rsp,       uint8_t rsp_len) {
    if (managed) bus_enable();
    if (!g_ow.reset()) { if (managed) bus_disable(); return BUS_NO_PRESENCE; }
    g_ow.write(rom_prefix, 0);
    if (rom_prefix == 0x33) {
        for (uint8_t i = 0; i < 8; i++) { delayMicroseconds(BUS_BYTE_GAP_US); rsp[i] = g_ow.read(); }
        rsp += 8;
    }
    for (uint8_t i = 0; i < cmd_len; i++) { delayMicroseconds(BUS_BYTE_GAP_US); g_ow.write(cmd[i], 0); }
    delayMicroseconds(BUS_SETTLE_US);
    for (uint8_t i = 0; i < rsp_len; i++) { delayMicroseconds(BUS_BYTE_GAP_US); rsp[i] = g_ow.read(); }
    if (managed) bus_disable();
    return BUS_OK;
}

static BusResult cmd_cc    (const uint8_t *c, uint8_t cl, uint8_t *r, uint8_t rl) { return ow_transaction(true,  0xCC, c, cl, r, rl); }
static BusResult cmd_cc_raw(const uint8_t *c, uint8_t cl, uint8_t *r, uint8_t rl) { return ow_transaction(false, 0xCC, c, cl, r, rl); }

// Two safe wrappers for the 0x33 (read-ROM) command.
// _with_rom : caller wants the 8 ROM bytes + rsp_len response bytes.
// _no_rom   : ROM bytes are discarded internally; rsp_len may be 0.
static BusResult cmd_33_with_rom(const uint8_t *c, uint8_t cl,
                                  uint8_t rom_out[8],
                                  uint8_t *rsp, uint8_t rsp_len) {
    uint8_t buf[8 + 32] = {0};
    BusResult res = ow_transaction(true, 0x33, c, cl, buf, rsp_len);
    if (res == BUS_OK) {
        memcpy(rom_out, buf, 8);
        if (rsp && rsp_len) memcpy(rsp, buf + 8, rsp_len);
    }
    return res;
}

static BusResult cmd_33_no_rom(const uint8_t *c, uint8_t cl) {
    uint8_t throwaway[8] = {0};
    return ow_transaction(true, 0x33, c, cl, throwaway, 0);
}

// ─── Nybble helpers ───────────────────────────────────────────
static uint8_t nybble_get(const uint8_t *d, uint8_t n) {
    return (n % 2 == 0) ? (d[n/2] & 0x0F) : ((d[n/2] >> 4) & 0x0F);
}
static void nybble_set(uint8_t *d, uint8_t n, uint8_t v) {
    v &= 0x0F;
    if (n % 2 == 0) d[n/2] = (d[n/2] & 0xF0) | v;
    else            d[n/2] = (d[n/2] & 0x0F) | (v << 4);
}
static uint8_t nybble_pair(const uint8_t *d, uint8_t hi, uint8_t lo) {
    return (uint8_t)((nybble_get(d, hi) << 4) | nybble_get(d, lo));
}
static uint8_t checksum_calc(const uint8_t *d, uint8_t s, uint8_t e) {
    uint8_t sum = 0;
    for (uint8_t i = s; i <= e; i++) sum += nybble_get(d, i);
    return sum & 0x0F;
}
static inline uint16_t le16(const uint8_t *b) { return (uint16_t)b[0] | ((uint16_t)b[1] << 8); }

// ─── Utility ─────────────────────────────────────────────────
static void print_sep() { LOG.println(F("-------------------------------------------------")); }


static void print_failure_code(uint8_t fc) {
    switch (fc) {
        case FC_OK:         LOG.println(F("OK (0)")); break;
        case FC_OVERLOADED: LOG.println(F("BAD - Overloaded (1)")); break;
        case FC_WARNING:    LOG.println(F("BAD - Warning (5)")); break;
        case FC_DEAD:       LOG.println(F("BAD - Dead (15)")); break;
        default: LOG.print(F("BAD - Unknown (")); LOG.print(fc); LOG.println(')'); break;
    }
}

// Prints each active lock cause with aligned brackets.
// Only prints if there are causes — nothing printed if clean.
static void print_lock_causes(const BatteryInfo &info, const uint8_t d[BASIC_INFO_LEN]) {
    if (info.lock_causes == 0) return;
    LOG.println(F("Lock causes    :"));
    char buf[48];
    if (info.lock_causes & LF_CS0) LOG.println(F("  CS0 mismatch     (nybbles 0-15)"));
    if (info.lock_causes & LF_CS2) LOG.println(F("  CS2 mismatch     (nybbles 32-40)"));
    if (info.lock_causes & LF_N34) {
        snprintf(buf, sizeof(buf), "  Nybble 34 = 0x%X  (must be 0)", nybble_get(d, 34));
        LOG.println(buf);
    }
}

static void print_frame(const uint8_t d[BASIC_INFO_LEN],
                        const __FlashStringHelper *label = nullptr) {
    if (!label) label = F("Frame          : ");
    LOG.print(label);
    for (uint8_t i = 0; i < BASIC_INFO_LEN; i++) {
        if (i > 0 && i % 11 == 0) { LOG.println(); LOG.print(F("                 ")); }
        if (d[i] < 0x10) LOG.print('0');
        LOG.print(d[i], HEX);
        if (i < BASIC_INFO_LEN - 1) LOG.print(' ');
    }
    LOG.println();
}

// ─── Test mode ───────────────────────────────────────────────
static BusResult enter_testmode() {
    uint8_t r[1] = {0};
    BusResult res = cmd_cc(CMD_TESTMODE_ENTER, sizeof(CMD_TESTMODE_ENTER), r, 1);
    if (res == BUS_OK) { g_in_testmode = true; safe_delay(INTER_CMD_MS); }
    return res;
}
static void exit_testmode() {
    if (!g_in_testmode) return;
    uint8_t r[1] = {0};
    cmd_cc(CMD_TESTMODE_EXIT, sizeof(CMD_TESTMODE_EXIT), r, 1);
    g_in_testmode = false;
    safe_delay(INTER_CMD_MS);
}

// ─── Basic info read ─────────────────────────────────────────
static BasicInfoResult read_basic_info(uint8_t d[BASIC_INFO_LEN]) {
    if (cmd_cc(CMD_BASIC_INFO, sizeof(CMD_BASIC_INFO), d, BASIC_INFO_LEN) != BUS_OK)
        return BASIC_INFO_NO_RESPONSE;
    uint8_t orv = 0, andv = 0xFF;
    for (uint8_t i = 0; i < BASIC_INFO_LEN; i++) { orv |= d[i]; andv &= d[i]; }
    if (orv  == 0x00) return BASIC_INFO_NO_RESPONSE;
    if (andv == 0xFF) return BASIC_INFO_PRE_TYPE0;
    return BASIC_INFO_OK;
}

// ─── ROM ID read ─────────────────────────────────────────────
// Returns true on bus success. On failure rom_out is zero-filled; callers
// MUST treat it as invalid (a zero ROM looks like a valid type-5 ROM).
static bool read_rom_id(uint8_t rom_out[8]) {
    uint8_t rsp[BASIC_INFO_LEN] = {0};
    BusResult res = cmd_33_with_rom(CMD_BASIC_INFO, sizeof(CMD_BASIC_INFO),
                                     rom_out, rsp, BASIC_INFO_LEN);
    if (res != BUS_OK) { memset(rom_out, 0, 8); return false; }
    return true;
}

// ─── Raw bus session helper ──────────────────────────────────
static bool raw_cc_session(uint16_t powerup_ms, bool send_skip_rom,
                           const uint8_t *cmd, uint8_t cmd_len,
                           uint8_t *rsp, uint8_t rsp_len) {
    digitalWrite(PIN_ENABLE, HIGH);
    if (powerup_ms) safe_delay(powerup_ms);
    if (!g_ow.reset()) { digitalWrite(PIN_ENABLE, LOW); return false; }
    if (send_skip_rom) { g_ow.write(0xCC, 0); delayMicroseconds(BUS_BYTE_GAP_US); }
    for (uint8_t i = 0; i < cmd_len; i++) { g_ow.write(cmd[i], 0); delayMicroseconds(BUS_BYTE_GAP_US); }
    for (uint8_t i = 0; i < rsp_len; i++) { rsp[i] = g_ow.read();  delayMicroseconds(BUS_BYTE_GAP_US); }
    digitalWrite(PIN_ENABLE, LOW);
    return true;
}

// ─── Model read ──────────────────────────────────────────────
static bool model_looks_valid(const char *m) {
    return strlen(m) >= 4 && ((m[0]=='B' && m[1]=='L') || (m[0]=='D' && m[1]=='C'));
}

static bool read_model(char out[8]) {
    uint8_t rsp[16] = {0};
    if (cmd_cc(CMD_MODEL, sizeof(CMD_MODEL), rsp, 16) != BUS_OK) { out[0] = '\0'; return false; }
    for (uint8_t i = 0; i < 7; i++) {
        uint8_t c = rsp[i];
        if (c < 0x20 || c >= 0x7F) { out[i] = '\0'; break; }
        out[i] = (char)c;
    }
    out[7] = '\0';
    return model_looks_valid(out);
}

static bool read_model_type5(char out[8]) {
    // ENABLE must stay HIGH throughout — cutting power between the
    // session enter and model read resets the battery's internal state.

    digitalWrite(PIN_ENABLE, HIGH);
    safe_delay(BUS_SETTLE_ON_MS);

    // Session enter: CC 99
    if (!g_ow.reset()) { digitalWrite(PIN_ENABLE, LOW); snprintf(out, 8, "BL18xx"); return false; }
    g_ow.write(0xCC, 0);
    delayMicroseconds(BUS_BYTE_GAP_US);
    g_ow.write(0x99, 0);

    safe_delay(INTER_CMD_MS);

    // Model read: bare 0x31 — returns two bytes, high byte first
    if (!g_ow.reset()) { digitalWrite(PIN_ENABLE, LOW); snprintf(out, 8, "BL18xx"); return false; }
    g_ow.write(0x31, 0);
    delayMicroseconds(BUS_BYTE_GAP_US);
    uint8_t hi = g_ow.read();
    delayMicroseconds(BUS_BYTE_GAP_US);
    uint8_t lo = g_ow.read();

    digitalWrite(PIN_ENABLE, LOW);

    if ((hi != 0xFF || lo != 0xFF) && (hi != 0x00 || lo != 0x00)) {
        snprintf(out, 8, "BL%02X%02X", lo, hi);
        return true;
    }
    snprintf(out, 8, "BL18xx");
    return false;
}

// ─── Universal no-response recovery ──────────────────────────
static bool wait_for_response() {
    LOG.print(F("  [No response] - waiting"));
    uint32_t started = millis();
    while (millis() - started < NO_RESPONSE_TIMEOUT_MS) {
        wdt_kick();
        power_cycle_bus();
        if (g_ow.reset() == 1) {
            uint32_t elapsed = millis() - started;
            LOG.print(F(" OK (")); LOG.print(elapsed); LOG.println(F(" ms)"));
            return true;
        }
        LOG.print(F("."));
    }
    LOG.print(F(" TIMEOUT (")); LOG.print(NO_RESPONSE_TIMEOUT_MS); LOG.println(F(" ms)"));
    return false;
}

static bool poll_until_response(uint8_t frame[BASIC_INFO_LEN]) {
    LOG.print(F("  Waiting"));
    uint32_t started = millis();
    while (millis() - started < NO_RESPONSE_TIMEOUT_MS) {
        wdt_kick();
        if (read_basic_info(frame) == BASIC_INFO_OK) {
            uint32_t elapsed = millis() - started;
            LOG.print(F(" (")); LOG.print(elapsed); LOG.println(F(" ms)"));
            return true;
        }
        LOG.print(F("."));
        power_cycle_bus();
    }
    LOG.print(F(" TIMEOUT (")); LOG.print(NO_RESPONSE_TIMEOUT_MS); LOG.println(F(" ms)"));
    return false;
}

// ─── Battery type detection ───────────────────────────────────
static int8_t detect_battery_type_raw(const uint8_t rom[8], const uint8_t d[BASIC_INFO_LEN],
                                       bool lock_only) {
    if (rom[3] < 100) return lock_only ? -1 : (int8_t)BatteryType::T5;
    if (d[17]  == 30) return lock_only ? -1 : (int8_t)BatteryType::T6;

    { static const uint8_t c[] = {0xDC,0x0B}; uint8_t r[17]={0};
      if (cmd_cc(c,sizeof(c),r,17)==BUS_OK && r[16]==TYPE_PROBE_MAGIC) return 0; }

    { if (enter_testmode()==BUS_OK) {
          static const uint8_t c[] = {0xDC,0x0A}; uint8_t r[17]={0};
          BusResult br = cmd_cc(c,sizeof(c),r,17);
          exit_testmode();
          power_cycle_bus();  // quiet settle after testmode exit — do NOT poll here
          if (br==BUS_OK && r[16]==TYPE_PROBE_MAGIC) return 2; } }

    { static const uint8_t c[] = {0xD4,0x2C,0x00,0x02}; uint8_t r[3]={0};
      if (cmd_cc(c,sizeof(c),r,3)==BUS_OK && r[2]==TYPE_PROBE_MAGIC) return 3; }

    return lock_only ? -1 : 0;
}

static BatteryType detect_battery_type(const uint8_t rom[8], const uint8_t d[BASIC_INFO_LEN]) {
    return (BatteryType)detect_battery_type_raw(rom, d, false);
}
static int8_t lock_detect_type(const uint8_t rom[8], const uint8_t d[BASIC_INFO_LEN]) {
    return detect_battery_type_raw(rom, d, true);
}

// ─── Parse / derive ───────────────────────────────────────────
static void parse_basic_info(const uint8_t d[BASIC_INFO_LEN], RawBasicInfo &o) {
    o.batt_type    = nybble_pair(d, 22, 23);
    o.capacity     = nybble_pair(d, 32, 33);
    o.failure_code = nybble_get(d, 40);
    o.overdischarge = nybble_pair(d, 48, 49);
    o.overload      = nybble_pair(d, 50, 51);
    o.cell_failure  = (nybble_get(d, 44) & 0x04) != 0;
    o.cycles = ((uint16_t)(nybble_get(d,52)&0x1)<<12)
             | ((uint16_t) nybble_get(d,53)       <<8)
             | ((uint16_t) nybble_get(d,54)       <<4)
             |  (uint16_t) nybble_get(d,55);
    o.checksum_n[0]     = nybble_get(d, 41);
    o.checksum_n[1]     = nybble_get(d, 42);
    o.checksum_n[2]     = nybble_get(d, 43);
    o.aux_checksum_n[0] = nybble_get(d, 62);
    o.aux_checksum_n[1] = nybble_get(d, 63);
}

static void derive_battery_info(const uint8_t d[BASIC_INFO_LEN], BatteryInfo &info) {
    const RawBasicInfo &r = info.raw;
    info.cell_count  = (r.batt_type < 13) ? 4 : (r.batt_type < 30) ? 5 : 10;
    info.capacity_ah = r.capacity / 10.0f;
    info.checksums_ok[0] = (checksum_calc(d, 0,  15) == r.checksum_n[0]);
    info.checksums_ok[1] = (checksum_calc(d, 16, 31) == r.checksum_n[1]);
    info.checksums_ok[2] = (checksum_calc(d, 32, 40) == r.checksum_n[2]);
    info.aux_checksums_ok[0] = (checksum_calc(d, 44, 47) == r.aux_checksum_n[0]);
    info.aux_checksums_ok[1] = (checksum_calc(d, 48, 61) == r.aux_checksum_n[1]);

    // Build lock cause bitfield — every known cause of a battery not charging
    info.lock_causes = 0;
    if (!info.checksums_ok[0])                                        info.lock_causes |= LF_CS0;
    if (!info.checksums_ok[2])                                        info.lock_causes |= LF_CS2;
    if (nybble_get(d, 34) != 0)                                       info.lock_causes |= LF_N34;

    info.locked = (info.lock_causes != 0);
}

static inline void refresh_info_from_frame(const uint8_t d[BASIC_INFO_LEN], BatteryInfo &info) {
    parse_basic_info(d, info.raw);
    derive_battery_info(d, info);
}

// ─── Health calculations ──────────────────────────────────────
static float calc_health_type56(const RawBasicInfo &r) {
    int16_t f_ol = max((int16_t)((int16_t)r.overload    - 29), (int16_t)0);
    int16_t f_od = max((int16_t)(35 - (int16_t)r.overdischarge), (int16_t)0);
    float   dmg  = r.cycles + r.cycles * (f_ol + f_od) / 32.0f;
    uint16_t scale = HEALTH_SCALE_STANDARD;
    for (size_t i = 0; i < HEALTH_SCALE_EXTENDED_COUNT; i++)
        if (r.capacity == HEALTH_SCALE_EXTENDED_CODES[i]) { scale = HEALTH_SCALE_EXTENDED; break; }
    return max(0.0f, HEALTH_MAX - dmg / scale);
}

static float calc_health_type023(uint16_t raw, uint8_t cap) {
    if (cap == 0) return 0.0f;
    float ratio = (float)raw / cap;
    return (ratio > 80.0f) ? HEALTH_MAX : max(0.0f, ratio / 10.0f - 5.0f);
}

static float calc_health_damage_rating(const uint8_t d[BASIC_INFO_LEN]) {
    uint8_t dmg = (nybble_get(d, 46) >> 1) & 0x07;
    if (dmg < 3)  return HEALTH_MAX;
    if (dmg >= 7) return 0.0f;
    return max(0.0f, HEALTH_MAX - (float)(dmg - 2));
}

// ─── Secondary reads (types 0, 2, 3) ─────────────────────────
static bool cc_probe(const uint8_t *cmd, uint8_t *rsp, uint8_t rsp_len) {
    if (cmd_cc(cmd, 4, rsp, rsp_len) != BUS_OK) return false;
    return rsp[rsp_len - 1] == TYPE_PROBE_MAGIC;
}

static uint8_t type023_idx(BatteryType t) {
    return (t == BatteryType::T2) ? 1 : (t == BatteryType::T3) ? 2 : 0;
}

static uint16_t read_health_raw(BatteryType t) {
    static const uint8_t cmds[3][4] = {
        {0xD4,0x50,0x01,0x02}, {0xD6,0x04,0x05,0x02}, {0xD6,0x38,0x02,0x02}
    };
    if (!is_type_023(t)) return 0;
    uint8_t r[3]={0};
    return cc_probe(cmds[type023_idx(t)], r, 3) ? le16(r) : 0;
}

static uint8_t read_od_count(BatteryType t) {
    static const uint8_t cmds[3][4] = {
        {0xD4,0xBA,0x00,0x01}, {0xD6,0x8D,0x05,0x01}, {0xD6,0x09,0x03,0x01}
    };
    if (!is_type_023(t)) return 0;
    uint8_t r[2]={0};
    return cc_probe(cmds[type023_idx(t)], r, 2) ? r[0] : 0;
}

static uint16_t read_overload_count(BatteryType t) {
    switch (t) {
        case BatteryType::T0: {
            static const uint8_t c[] = {0xD4,0x8D,0x00,0x07}; uint8_t r[8]={0};
            if (!cc_probe(c,r,8)) return 0;
            uint16_t a  = ((uint16_t)(r[0]>>6)&0x03)|((uint16_t)r[1]<<2);
            uint16_t b  =  (uint16_t) r[3]           |(((uint16_t)r[4]&0x03)<<8);
            uint16_t c2 = ((uint16_t)(r[5]>>4))      |(((uint16_t)r[6]&0x3F)<<4);
            return a+b+c2;
        }
        case BatteryType::T2: {
            static const uint8_t c[] = {0xD6,0x5F,0x05,0x07}; uint8_t r[8]={0};
            return cc_probe(c,r,8) ? (uint16_t)(r[0]+r[2]+r[3]+r[5]+r[6]) : 0;
        }
        case BatteryType::T3: {
            static const uint8_t c[] = {0xD6,0x5B,0x03,0x04}; uint8_t r[6]={0};
            return cc_probe(c,r,6) ? (uint16_t)(r[0]+r[2]+r[3]) : 0;
        }
        default: return 0;
    }
}

static uint32_t read_charge_level() {
    static const uint8_t cmd[] = {0xD7,0x19,0x00,0x04};
    uint8_t r[5]={0};
    if (!cc_probe(cmd,r,5)) return 0;
    return (uint32_t)r[0]|((uint32_t)r[1]<<8)|((uint32_t)r[2]<<16)|((uint32_t)r[3]<<24);
}

// ─── Temperature (type 6 only) ───────────────────────────────
static float read_temperature_type6() {
    static const uint8_t CMD_TEMP6[] = {0xD2};
    bus_enable();
    uint8_t r[1] = {0};
    bool ok = g_ow.reset() != 0;
    if (ok) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write(CMD_TEMP6[0], 0);
        delayMicroseconds(BUS_SETTLE_US);
        r[0] = g_ow.read();
    }
    bus_disable();
    return ok ? (TEMP6_A + TEMP6_B * r[0]) / 100.0f : TEMP_INVALID;
}

// ─── Voltage reads ────────────────────────────────────────────
static bool read_voltages_type023(VoltageReadResult &vr, uint8_t ncells) {
    static const uint8_t ext[]    = {0xD7,0x00,0x00,0xFF};
    static const uint8_t short_[] = {0xD7,0x00,0x00,0x0C};
    uint8_t r[27]={0};
    uint8_t n = min(ncells, (uint8_t)5);
    bool ok = (cmd_cc(ext, sizeof(ext), r, 27)==BUS_OK) && (le16(&r[0])>0);
    if (ok) {
        vr.vpack = le16(&r[0]) / 1000.0f;
        for (uint8_t i = 0; i < n; i++) vr.cells.v[i] = le16(&r[2+i*2]) / 1000.0f;
        vr.cells.n = n; vr.cells.valid = true;
        uint16_t rc = le16(&r[14]), rm = le16(&r[16]);
        vr.t_cell   = (rc != 0 && rc != 0xFFFF) ? rc / 100.0f : TEMP_INVALID;
        vr.t_mosfet = (rm != 0 && rm != 0xFFFF) ? rm / 100.0f : TEMP_INVALID;
        vr.ok = true;
        return true;
    }
    uint8_t s[13]={0};
    if (cmd_cc(short_,sizeof(short_),s,13)!=BUS_OK || s[12]!=TYPE_PROBE_MAGIC) return false;
    vr.vpack = le16(&s[0]) / 1000.0f;
    for (uint8_t i = 0; i < n; i++) vr.cells.v[i] = le16(&s[2+i*2]) / 1000.0f;
    vr.cells.n = n; vr.cells.valid = true;
    vr.ok = true;
    return true;
}


// ─── Type 5 voltage reads ────────────────────────────────────
// Two-path approach for the two known Type 5 variants:
//
// Fast path: battery session is already warm from read_model_type5.
//   CC 31 responds immediately — read all cells and return.
//
// Slow path: battery ADC needs multiple read cycles to settle.
//   Re-enter session with CC 99, then poll until all cells valid.

static bool read_voltages_type5(VoltageReadResult &vr, uint8_t ncells) {
    static const uint8_t CELL_CMDS[] = { 0x31, 0x32, 0x33, 0x34, 0x35 };
    static const uint8_t TC[]        = { TYPE5_CMD_TEMP };
    uint8_t n = min(ncells, (uint8_t)5);

    // ── Fast path ────────────────────────────────────────────────
    // Try CC 31 immediately. If cell 1 returns a valid voltage the
    // session is warm and we can read all cells in one pass.
    {
        uint8_t r[2] = {0};
        cmd_cc(&CELL_CMDS[0], 1, r, 2);  // managed — enables bus with 150ms settle
        uint16_t mv = le16(r);
        if (mv >= 2500 && mv <= 4500) {
            float pack = mv/1000.0f;
            vr.cells.v[0] = mv/1000.0f;
            bus_enable();  // bus was disabled by cmd_cc — re-enable for raw reads
            for (uint8_t i = 1; i < n; i++) {
                r[0] = r[1] = 0;
                cmd_cc_raw(&CELL_CMDS[i], 1, r, 2);
                mv = le16(r);
                if (mv >= 500 && mv <= 4800) { vr.cells.v[i] = mv/1000.0f; pack += vr.cells.v[i]; }
            }
            r[0] = r[1] = 0; cmd_cc_raw(TC, 1, r, 2);
            uint16_t traw = le16(r);
            if (traw > 0 && traw != 0xFFFF) vr.t_cell = traw/100.0f;
            bus_disable();
            vr.cells.n = n; vr.cells.valid = true; vr.vpack = pack; vr.ok = true;
            return true;
        }
    }

    // ── Slow path ────────────────────────────────────────────────
    // Cell 1 returned 0 — re-enter session and poll until the ADC
    // has valid data for all cells.
    uint8_t prime[2] = {0};
    raw_cc_session(0, true,  &TYPE5_CMD_SESSION, 1, prime, 0);
    safe_delay(INTER_CMD_MS);
    raw_cc_session(0, false, &TYPE5_CMD_MODEL,   1, prime, 2);

    float best_v[5] = {0}; uint8_t best_any = 0;
    float best_pack = 0, best_temp = TEMP_INVALID;

    for (uint8_t attempt = 0; attempt < TYPE5_MAX_ATTEMPTS; attempt++) {
        wdt_kick();
        float v[5] = {0}; uint8_t got = 0; float pack = 0;
        // First cell uses managed cmd_cc (150ms settle after bus was off)
        {
            uint8_t r[2] = {0};
            cmd_cc(&CELL_CMDS[0], 1, r, 2);
            uint16_t mv = le16(r);
            if (mv >= 500 && mv <= 4800) { v[0] = mv/1000.0f; pack += v[0]; got++; }
        }
        // Remaining cells use raw — bus already enabled by first read
        bus_enable();
        for (uint8_t i = 1; i < n; i++) {
            uint8_t r[2] = {0};
            cmd_cc_raw(&CELL_CMDS[i], 1, r, 2);
            uint16_t mv = le16(r);
            if (mv >= 500 && mv <= 4800) { v[i] = mv/1000.0f; pack += v[i]; got++; }
        }
        uint8_t tr[2] = {0}; cmd_cc_raw(TC, 1, tr, 2);
        bus_disable();
        uint16_t traw = le16(tr);
        float t = (traw > 0 && traw != 0xFFFF) ? traw/100.0f : TEMP_INVALID;
        if (got >= best_any) {
            best_any = got; best_pack = pack; best_temp = t;
            for (uint8_t i = 0; i < n; i++) best_v[i] = v[i];
        }
        if (got == n) break;
    }

    if (best_any == 0) return false;
    for (uint8_t i = 0; i < n; i++) vr.cells.v[i] = best_v[i];
    vr.cells.n = n; vr.cells.valid = true; vr.vpack = best_pack;
    vr.t_cell = best_temp; vr.ok = true;
    return true;
}

static bool read_voltages_type6(VoltageReadResult &vr, uint8_t ncells) {
    uint8_t ign[1]={0}, r[20]={0};
    bus_enable();
    { static const uint8_t c[]={0x10,0x21}; cmd_cc_raw(c,sizeof(c),ign,0); safe_delay(10); }
    bool ok = g_ow.reset() != 0;
    if (ok) {
        delayMicroseconds(BUS_BYTE_GAP_US);
        g_ow.write(TYPE6_VOLT_READ, 0);
        delayMicroseconds(BUS_SETTLE_US);
        for (uint8_t i = 0; i < 20; i++) { r[i] = g_ow.read(); delayMicroseconds(BUS_BYTE_GAP_US); }
    }
    bus_disable();
    if (!ok) return false;
    uint8_t n = min(ncells, (uint8_t)MAX_CELLS);
    float pack = 0;
    for (uint8_t i = 0; i < n; i++) {
        vr.cells.v[i] = (6000.0f - le16(&r[i*2])/10.0f)/1000.0f;
        pack += vr.cells.v[i];
    }
    vr.cells.n = n; vr.cells.valid = true; vr.vpack = pack; vr.ok = true;
    return true;
}

// ─── Diagnostic report ───────────────────────────────────────
static void print_report(const BatteryInfo &info, const VoltageReadResult &vr,
                         const uint8_t d[BASIC_INFO_LEN]) {
    LOG.print(F("Model          : ")); LOG.println(info.model);
    LOG.print(F("ROM ID         : "));
    if (info.rom_id_valid) {
        for (uint8_t i=0;i<8;i++) { if(info.rom_id[i]<0x10) LOG.print('0'); LOG.print(info.rom_id[i],HEX); if(i<7) LOG.print(' '); }
        LOG.println();
        char db[13]; snprintf(db,sizeof(db),"%02d/%02d/20%02d",info.rom_id[2],info.rom_id[1],info.rom_id[0]);
        LOG.print(F("Mfg date       : ")); LOG.println(db);
    } else {
        LOG.println(F("(unavailable)"));
    }
    LOG.print(F("Detected type  : ")); LOG.println(batt_type_to_int(info.type));
    LOG.print(F("Battery type   : ")); LOG.print(info.raw.batt_type); LOG.print(F("  ("));
    if      (info.raw.batt_type<13) LOG.print(F("4 cell BL14xx"));
    else if (info.raw.batt_type<30) LOG.print(F("5 cell BL18xx"));
    else                            LOG.print(F("10 cell BL36xx"));
    LOG.println(')');
    LOG.print(F("Capacity       : ")); LOG.print(info.capacity_ah,1); LOG.println(F(" Ah"));

    print_sep();
    LOG.print(F("Lock status    : ")); LOG.println(info.locked ? F("LOCKED") : F("UNLOCKED"));
    LOG.print(F("Cell failure   : ")); LOG.println(info.raw.cell_failure ? F("YES") : F("No"));
    LOG.print(F("Checksum 0-15  : ")); LOG.println(info.checksums_ok[0] ? F("OK") : F("BAD"));
    LOG.print(F("Checksum 16-31 : ")); LOG.println(info.checksums_ok[1] ? F("OK") : F("BAD"));
    LOG.print(F("Checksum 32-40 : ")); LOG.println(info.checksums_ok[2] ? F("OK") : F("BAD"));
    LOG.print(F("Aux CSum 44-47 : ")); LOG.println(info.aux_checksums_ok[0] ? F("OK") : F("BAD"));
    LOG.print(F("Aux CSum 48-61 : ")); LOG.println(info.aux_checksums_ok[1] ? F("OK") : F("BAD"));
    { char buf[64];
      if (d[1]==0x26)      LOG.println(F("Byte 1  origin : China/Murata (26)"));
      else if (d[1]==0x36) LOG.println(F("Byte 1  origin : Vietnam/Samsung (36)"));
      else if (d[1]==0x31) LOG.println(F("Byte 1  origin : Old family (31)"));
      else { snprintf(buf,sizeof(buf),"Byte 1  origin : Unknown (%02X)",d[1]); LOG.println(buf); }
      uint8_t n34=nybble_get(d,34);
      snprintf(buf,sizeof(buf),"Nybble 34 lock : %s (%X)",(n34==0?"OK":"BAD"),n34); LOG.println(buf);
      snprintf(buf,sizeof(buf),"Byte 19 status : (%02X)",d[19]); LOG.println(buf);
      LOG.print(F("Nybble 40 code : ")); print_failure_code(info.raw.failure_code); }
    if (info.locked) print_lock_causes(info, d);

    print_sep();
    if (info.raw.cycles > 0) {
        LOG.print(F("Cycle count    : ")); LOG.println(info.raw.cycles);
    }
    if (info.health.rating > 0.0f) {
        LOG.print(F("Health         : ")); LOG.print(info.health.rating,2); LOG.print(F(" / 4  ("));
        int h=(int)round(info.health.rating);
        for (int i=0;i<4;i++) LOG.print(i<h ? '#' : '-');
        LOG.println(')');
    }
    if (is_type_023(info.type)) {
        if (info.health.od_count > 0) {
            LOG.print(F("OD #           : ")); LOG.println(info.health.od_count);
            if (info.raw.cycles > 0) {
                LOG.print(F("OD %           : ")); LOG.print(4.0f+100.0f*info.health.od_count/info.raw.cycles,1); LOG.println(F(" %"));
            }
        }
        if (info.health.overload_count > 0) {
            LOG.print(F("Overload #     : ")); LOG.println(info.health.overload_count);
            if (info.raw.cycles > 0) {
                LOG.print(F("Overload %     : ")); LOG.print(4.0f+100.0f*info.health.overload_count/info.raw.cycles,1); LOG.println(F(" %"));
            }
        }
    }
    if (is_type_56(info.type)) {
        if (info.raw.overdischarge > 0) {
            LOG.print(F("OD %           : ")); LOG.print(-5.0f*info.raw.overdischarge+160.0f,1); LOG.println(F(" %"));
        }
        if (info.raw.overload > 0) {
            LOG.print(F("Overload %     : ")); LOG.print( 5.0f*info.raw.overload-160.0f,1); LOG.println(F(" %"));
        }
    }
    if (info.temperature_c != TEMP_INVALID) {
        LOG.print(F("Temp (Cells)   : ")); LOG.print(info.temperature_c,1); LOG.println(F(" C"));
    }
    if (info.temperature_mosfet_c != TEMP_INVALID) {
        LOG.print(F("Temp (Mosfet)  : ")); LOG.print(info.temperature_mosfet_c,1); LOG.println(F(" C"));
    }

    print_sep();
    LOG.print(F("Pack voltage   : ")); LOG.println(vr.vpack,3);
    if (!vr.cells.valid || vr.cells.n == 0) {
        LOG.println(F("(no cells reported)"));
    } else {
        float vmax=vr.cells.v[0], vmin=vr.cells.v[0];
        for (uint8_t i=0;i<vr.cells.n;i++) {
            char lb[22]; snprintf(lb,sizeof(lb),"Cell %2u        : ",(unsigned)i+1);
            LOG.print(lb); LOG.println(vr.cells.v[i],3);
            if (vr.cells.v[i]>vmax) vmax=vr.cells.v[i];
            if (vr.cells.v[i]<vmin) vmin=vr.cells.v[i];
        }
        LOG.print(F("Cell imbalance : ")); LOG.println(vmax-vmin,3);
    }

    if (is_type_023(info.type) && info.health.charge_level > 0 && info.raw.capacity > 0) {
        float ratio = (float)info.health.charge_level / info.raw.capacity / SOC_DIVISOR;
        uint8_t soc = (ratio<10.0f) ? 1 : (uint8_t)min((int)(ratio/10.0f),7);
        print_sep();
        LOG.print(F("State of charge: ")); LOG.print(soc); LOG.println(F(" / 7"));
    }

    print_sep();
    print_frame(d);
}

// ─── Frame repair / write ────────────────────────────────────
static bool charger_write_frame(const uint8_t src[BASIC_INFO_LEN]) {
    if (g_charger_arm_issued) {
        LOG.println(F("  Arm already issued - power-cycle required."));
        return false;
    }
    {
        uint8_t r[BASIC_INFO_LEN]={0};
        if (cmd_cc(CMD_CC_F0,sizeof(CMD_CC_F0),r,BASIC_INFO_LEN)!=BUS_OK) {
            LOG.println(F("  Arm: no presence.")); return false;
        }
        g_charger_arm_issued = true;
        safe_delay(INTER_CMD_MS);
    }
    {
        uint8_t payload[2+BASIC_INFO_LEN];
        payload[0]=FRAME_WRITE_OPCODE; payload[1]=FRAME_WRITE_PAD;
        memcpy(&payload[2],src,BASIC_INFO_LEN);
        if (cmd_33_no_rom(payload,sizeof(payload))!=BUS_OK) {
            LOG.println(F("  Write: no presence.")); return false;
        }
        safe_delay(INTER_CMD_MS);
    }
    {
        if (cmd_33_no_rom(CMD_STORE,sizeof(CMD_STORE))!=BUS_OK) {
            LOG.println(F("  Store: no presence.")); return false;
        }
        safe_delay(INTER_CMD_MS);
    }
    return true;
}

static bool do_protected_write(const uint8_t frame[BASIC_INFO_LEN],
                               const __FlashStringHelper *log_prefix) {
    if (enter_testmode() != BUS_OK) {
        LOG.print(log_prefix); LOG.println(F("No presence entering testmode."));
        if (!wait_for_response()) return false;
        if (enter_testmode() != BUS_OK) {
            LOG.print(log_prefix); LOG.println(F("Still no presence. Aborting write."));
            return false;
        }
    }
    if (!charger_write_frame(frame)) {
        exit_testmode(); power_cycle_bus(); return false;
    }
    exit_testmode();
    return true;
}

// Send DA04 error register clear. Call after a successful frame write.
// Frame repair — sets nybble 34 = 0 and recalculates checksums.
// Confirmed by testing: only nybble 34, CS0, and CS2 are charger-validated.
// All other frame data is left untouched.
static bool repair_frame(uint8_t data32[BASIC_INFO_LEN]) {
    uint8_t frame[BASIC_INFO_LEN];
    memcpy(frame, data32, BASIC_INFO_LEN);

    // Set nybble 34 = 0 — the only charger lock nybble (byte 17 high nybble preserved)
    frame[17] = (frame[17] & 0xF0) | 0x00;

    // Recalculate CS0 and CS2 — the only checksums the charger validates
    nybble_set(frame, 41, checksum_calc(frame,  0, 15));  // CS0
    nybble_set(frame, 43, checksum_calc(frame, 32, 40));  // CS2
    print_frame(frame, F("  Repair frame : "));

    if (!do_protected_write(frame, F("  "))) return false;
    power_cycle_bus();

    uint8_t verify[BASIC_INFO_LEN] = {0};
    if (!poll_until_response(verify)) {
        LOG.println(F("  No response during verify."));
        return false;
    }

    bool ok0 = (checksum_calc(verify,  0, 15) == nybble_get(verify, 41));
    bool ok2 = (checksum_calc(verify, 32, 40) == nybble_get(verify, 43));
    uint8_t n34 = nybble_get(verify, 34);
    char buf[48];
    snprintf(buf, sizeof(buf), "  Lock check : CS0=%s  CS2=%s  N34=%s (%u)",
        ok0 ? "OK" : "BAD",
        ok2 ? "OK" : "BAD",
        n34 == 0 ? "OK" : "BAD", (unsigned)n34);
    LOG.println(buf);

    if (ok0 && ok2 && n34 == 0) {
        memcpy(data32, verify, BASIC_INFO_LEN);
        return true;
    }

    memcpy(data32, verify, BASIC_INFO_LEN);
    return false;
}

static bool type_supports_unlock(BatteryType t) { return is_type_023(t); }

// ─── Scan steps ───────────────────────────────────────────────
// Wi-Fi interrupts can stretch 1-Wire bit timing, so a frame is only trusted
// once two consecutive reads return byte-identical data.
static BasicInfoResult read_basic_info_stable(uint8_t d[BASIC_INFO_LEN]) {
    uint8_t prev[BASIC_INFO_LEN];
    BasicInfoResult r = read_basic_info(d);
    if (r != BASIC_INFO_OK) return r;

    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        memcpy(prev, d, BASIC_INFO_LEN);
        wdt_kick();
        delay(INTER_CMD_MS);
        BasicInfoResult again = read_basic_info(d);
        if (again != BASIC_INFO_OK) { memcpy(d, prev, BASIC_INFO_LEN); return r; }
        if (memcmp(prev, d, BASIC_INFO_LEN) == 0) return BASIC_INFO_OK;
        LOG.println(F("  Frame mismatch, re-reading..."));
    }
    LOG.println(F("WARNING: frame unstable after retries; data may be unreliable."));
    return BASIC_INFO_OK;
}

static BasicInfoResult step_read_basic_info(uint8_t d[BASIC_INFO_LEN]) {
    uint32_t started = millis();
    bool printed = false;
    while (millis() - started < NO_RESPONSE_TIMEOUT_MS) {
        BasicInfoResult r = read_basic_info_stable(d);
        if (r != BASIC_INFO_NO_RESPONSE) {
            if (printed) {
                uint32_t elapsed = millis() - started;
                LOG.print(F(" OK (")); LOG.print(elapsed); LOG.println(F(" ms)"));
            }
            return r;
        }
        if (!printed) { LOG.print(F("  Waiting")); printed = true; }
        LOG.print(F("."));
        wdt_kick();
        power_cycle_bus();
    }
    if (printed) LOG.println();
    if (!wait_for_response()) return BASIC_INFO_NO_RESPONSE;
    return read_basic_info_stable(d);
}

static void step_identify(BatteryInfo &info, const uint8_t d[BASIC_INFO_LEN]) {
    info.rom_id_valid = read_rom_id(info.rom_id);
    if (!info.rom_id_valid)
        LOG.println(F("WARNING: ROM ID read failed; using fallback type detection."));
    info.type = detect_battery_type(info.rom_id, d);
    (void)((info.type == BatteryType::T5) ? read_model_type5(info.model) : read_model(info.model));
}

static VoltageReadResult step_read_voltages(const BatteryInfo &info) {
    VoltageReadResult vr = voltage_result_init();
    switch (info.type) {
        case BatteryType::T5: safe_delay(10); read_voltages_type5(vr, info.cell_count); break;
        case BatteryType::T6: read_voltages_type6(vr, info.cell_count); break;
        default:              read_voltages_type023(vr, info.cell_count); break;
    }
    return vr;
}

static void step_read_health(BatteryInfo &info, const uint8_t d[BASIC_INFO_LEN]) {
    info.health = {};
    if (is_type_56(info.type)) {
        info.health.rating = calc_health_type56(info.raw);
    } else if (is_type_023(info.type)) {
        info.health.rating         = calc_health_type023(read_health_raw(info.type), info.raw.capacity);
        info.health.od_count       = read_od_count(info.type);
        info.health.overload_count = read_overload_count(info.type);
        info.health.charge_level   = (info.type==BatteryType::T0) ? read_charge_level() : 0;
    } else {
        info.health.rating = calc_health_damage_rating(d);
    }
}

// ─── Unlock sequence ─────────────────────────────────────────
static void step_handle_lock(BatteryInfo &info, uint8_t d[BASIC_INFO_LEN]) {
    if (!info.locked) {
        LOG.println(F("Battery UNLOCKED."));
        led_flash(COL_GREEN);
        if (type_supports_unlock(info.type)) power_cycle_bus();
        return;
    }
    if (!type_supports_unlock(info.type)) {
        LOG.print(F("Unlock not supported for type "));
        LOG.print(batt_type_to_int(info.type));
        LOG.println(F(". Skipping."));
        led_flash(COL_RED); return;
    }

    LOG.println(F("Battery LOCKED."));
    led_yellow();

    bool unlocked = false;

    for (uint8_t attempt = 1; attempt <= UNLOCK_MAX_CYCLES && !unlocked; attempt++) {
        wdt_kick();

        if (attempt == 1) {
            // Attempt 1: DA04 error register clear.
            // Handles naturally locked batteries (overdischarge, overload).
            // Type 2 batteries self-repair entirely via DA04.
            LOG.println(F("--- Attempt 1: DA04 reset"));
            led_yellow();
            power_cycle_bus();
            if (enter_testmode() == BUS_OK) {
                uint8_t rom[8]={0}, r[9]={0};
                cmd_33_with_rom(CMD_RESET_ERRORS, sizeof(CMD_RESET_ERRORS), rom, r, 9);
                exit_testmode();
                power_cycle_bus();
                uint8_t verify[BASIC_INFO_LEN] = {0};
                if (poll_until_response(verify)) {
                    memcpy(d, verify, BASIC_INFO_LEN);
                    refresh_info_from_frame(d, info);
                    if (!info.locked) {
                        LOG.println(F("  -> UNLOCKED by DA04."));
                        unlocked = true;
                        continue;
                    }
                    LOG.println(F("  -> Still locked. Trying frame repair."));
                }
            }
            // Fall through to frame repair on same attempt
        }

        // Attempt 1 fallthrough + attempts 2-6: frame repair
        LOG.print(F("--- Attempt ")); LOG.print(attempt); LOG.println(F(": Frame repair"));
        led_purple();
        if (repair_frame(d)) {
            refresh_info_from_frame(d, info);
            if (!info.locked) {
                LOG.println(F("  -> UNLOCKED."));
                unlocked = true;
            } else {
                LOG.println(F("  -> Still locked. Retrying."));
            }
        } else {
            LOG.println(F("  -> Frame repair failed."));
        }
    }

    if (unlocked) { LOG.println(F("Battery successfully unlocked.")); led_flash(COL_GREEN); power_cycle_bus(); }
    else          { LOG.println(F("Battery still locked."));           led_flash(COL_RED); }
}

// ═══════════════════════════════════════════════════════════════
//  OMEGA LOCK
//  Sets nybble 34 (original Makita charger lock nybble) non-zero.
//  Present in ALL Makita LXT batteries. Charger rejects if non-zero.
//  DA04 cannot undo this lock. Only repair_frame() can restore it.
// ═══════════════════════════════════════════════════════════════

static void omega_mutate(uint8_t v[BASIC_INFO_LEN]) {
    // Set nybble 34 = 4, preserve nybble 35 (high nybble of byte 17)
    v[17] = (v[17] & 0xF0) | 0x04;
    // Recalculate CS2 (covers nybbles 32-40, includes byte 17)
    nybble_set(v, 43, checksum_calc(v, 32, 40));
}

static bool omega_verify(const uint8_t v[BASIC_INFO_LEN]) {
    return nybble_get(v, 34) != 0;
}

static bool omega_already_locked(const uint8_t frame[BASIC_INFO_LEN]) {
    return nybble_get(frame, 34) != 0;
}

static bool run_lock() {
    if (g_in_testmode) exit_testmode();
    power_cycle_bus(); led_blue();

    uint8_t rom_id[8] = {0};
    bool rom_ok = read_rom_id(rom_id);

    uint8_t frame[BASIC_INFO_LEN] = {0};
    BasicInfoResult fr = step_read_basic_info(frame);
    if (fr != BASIC_INFO_OK) {
        if (fr == BASIC_INFO_PRE_TYPE0) { LOG.println(F("  [Lock] Pre-type-0 HC08 - unsupported.")); led_yellow(); }
        else                            { LOG.println(F("  [Lock] ERROR: No valid frame."));          led_flash(COL_RED); }
        bus_disable(); return false;
    }
    if (!rom_ok) LOG.println(F("  [Lock] WARNING: ROM ID read failed."));

    int8_t type = lock_detect_type(rom_id, frame);
    LOG.print(F("  [Lock] Type: "));
    if (type < 0) { LOG.println(F("unsupported (5, 6, or unknown).")); led_yellow(); bus_disable(); return false; }
    LOG.println(type);

    if (omega_already_locked(frame)) {
        LOG.println(F("  [Lock] Battery already OMEGA LOCKED (nybble 34 != 0)."));
        led_flash(COL_RED); bus_disable(); return true;
    }

    bool ok = false;
    for (uint8_t attempt = 1; attempt <= LOCK_MAX_ATTEMPTS && !ok; attempt++) {
        LOG.print(F("  [Lock] Attempt ")); LOG.println(attempt);
        wdt_kick();
        uint8_t v[BASIC_INFO_LEN];
        memcpy(v, frame, BASIC_INFO_LEN);
        omega_mutate(v);
        led_purple();
        if (!do_protected_write(v, F("  [Lock] "))) { bus_disable(); return false; }
        power_cycle_bus();
        uint8_t verify[BASIC_INFO_LEN] = {0};
        if (!poll_until_response(verify)) { LOG.println(F("  [Lock] No response.")); break; }
        LOG.print(F("  [Lock] Nybble 34 = ")); LOG.println(nybble_get(verify, 34));
        ok = omega_verify(verify);
    }

    print_sep();
    if (ok) LOG.println(F("  [Lock] OMEGA LOCKED. Nybble 34 set non-zero."));
    else    LOG.println(F("  [Lock] FAILED."));
    print_sep();
    led_result(ok); bus_disable();
    return ok;
}

// ─── Snapshot store (shared with the web UI) ─────────────────
static void snap_lock()   { if (g_snap_mutex) xSemaphoreTake(g_snap_mutex, portMAX_DELAY); }
static void snap_unlock() { if (g_snap_mutex) xSemaphoreGive(g_snap_mutex); }

static void snapshot_store(const BatteryInfo &info, const VoltageReadResult &vr,
                           const uint8_t d[BASIC_INFO_LEN], bool imbalanced, float imbalance_v) {
    snap_lock();
    g_snap.valid        = true;
    g_snap.pre_type0    = false;
    g_snap.timestamp_ms = millis();
    g_snap.info         = info;
    g_snap.vr           = vr;
    memcpy(g_snap.frame, d, BASIC_INFO_LEN);
    g_snap.imbalanced   = imbalanced;
    g_snap.imbalance_v  = imbalance_v;
    g_snap.last_error[0] = '\0';
    snap_unlock();
}

static void snapshot_error(const char *msg, bool pre_type0 = false) {
    snap_lock();
    g_snap.valid     = false;
    g_snap.pre_type0 = pre_type0;
    g_snap.timestamp_ms = millis();
    strncpy(g_snap.last_error, msg, sizeof(g_snap.last_error) - 1);
    g_snap.last_error[sizeof(g_snap.last_error) - 1] = '\0';
    snap_unlock();
}

static void snapshot_clear() {
    snap_lock();
    g_snap = ScanSnapshot{};
    snap_unlock();
}

// ─── Main scan ───────────────────────────────────────────────
// allow_unlock = false : read-only scan (default for the web UI)
// allow_unlock = true  : run the DA04 + frame-repair unlock sequence
static bool run_scan(bool allow_unlock) {
    if (g_in_testmode) exit_testmode();
    set_activity(allow_unlock ? "Đang mở khóa pin…" : "Đang đọc dữ liệu pin…");
    power_cycle_bus(); led_blue();

    BatteryInfo info{};
    info.temperature_c = info.temperature_mosfet_c = TEMP_INVALID;
    info.type = BatteryType::UNKNOWN;
    uint8_t data32[BASIC_INFO_LEN] = {0};

    BasicInfoResult br = step_read_basic_info(data32);
    if (br == BASIC_INFO_NO_RESPONSE) {
        LOG.println(F("ERROR: Battery not responding after retries. Aborting."));
        snapshot_error("Pin không phản hồi trên bus 1-Wire");
        led_flash(COL_RED);
        bus_disable(); g_charger_arm_issued = false; return false;
    }
    if (br == BASIC_INFO_PRE_TYPE0) {
        print_sep();
        LOG.println(F("WARNING: Pre-type-0 HC08 battery detected!"));
        LOG.println(F("         Freescale MC908JK3E BMS -- no cell protection."));
        LOG.println(F("         DO NOT charge on any charger."));
        print_sep();
        snapshot_error("Pin đời cũ HC08 (MC908JK3E) - KHÔNG được sạc", true);
        led_flash(COL_RED); bus_disable(); g_charger_arm_issued = false; return true;
    }

    refresh_info_from_frame(data32, info);
    step_identify(info, data32);
    print_sep();
    LOG.print(F("Battery found  : ")); LOG.println(info.model);

    VoltageReadResult vr = step_read_voltages(info);
    info.temperature_c        = vr.t_cell;
    info.temperature_mosfet_c = vr.t_mosfet;
    if (info.type == BatteryType::T6) info.temperature_c = read_temperature_type6();
    step_read_health(info, data32);

    if (!battery_present()) {
        LOG.println(F("Battery removed during scan. Aborting."));
        snapshot_error("Pin bị rút ra khi đang quét");
        bus_disable(); g_charger_arm_issued = false; return false;
    }

    if (!vr.ok) LOG.println(F("WARNING: voltage read failed; cell data may be missing."));
    print_sep(); print_report(info, vr, data32); print_sep();

    // Detect cell imbalance — warn via orange blink after result
    bool  imbalanced  = false;
    float imbalance_v = 0.0f;
    if (vr.cells.valid && vr.cells.n > 1) {
        float vmax=vr.cells.v[0], vmin=vr.cells.v[0];
        for (uint8_t i=1;i<vr.cells.n;i++) {
            if (vr.cells.v[i]>vmax) vmax=vr.cells.v[i];
            if (vr.cells.v[i]<vmin) vmin=vr.cells.v[i];
        }
        imbalance_v = vmax - vmin;
        if (imbalance_v >= IMBALANCE_THRESHOLD_V) {
            imbalanced = true;
            LOG.print(F("WARNING: Cell imbalance "));
            LOG.print(imbalance_v, 3);
            LOG.println(F("V - check balancing tabs."));
        }
    }

    if (allow_unlock) {
        step_handle_lock(info, data32);
    } else {
        LOG.print(F("Lock status    : "));
        LOG.println(info.locked ? F("LOCKED (use the Unlock button)") : F("UNLOCKED"));
        led_flash(info.locked ? COL_YELLOW : COL_GREEN);
    }
    print_sep(); LOG.println(F("Complete."));

    snapshot_store(info, vr, data32, imbalanced, imbalance_v);

    // Set imbalance blink — orange over result colour
    g_imbalance     = imbalanced;
    Colour rc = info.locked ? COL_RED : COL_GREEN;
    g_result_r = rc.r; g_result_g = rc.g; g_result_b = rc.b;

    bus_disable(); g_charger_arm_issued = false;
    return true;
}

// ─── LED task ─────────────────────────────────────────────────
// Runs the breathing pulse / imbalance blink independently of the
// battery task. Volatile flags are the only shared state.
static void led_task(void *) {
    for (;;) {
        if (g_pulse_active) {
            led_pulse(g_pulse_scan_mode ? COL_WHITE : COL_RED);
            vTaskDelay(pdMS_TO_TICKS(LED_PULSE_INTERVAL_MS));
            continue;
        }
        if (g_imbalance) {
            vTaskDelay(pdMS_TO_TICKS(IMBALANCE_BLINK_INTERVAL_MS - IMBALANCE_BLINK_ON_MS));
            led_set(COL_ORANGE);
            vTaskDelay(pdMS_TO_TICKS(IMBALANCE_BLINK_ON_MS));
            led_set({(uint8_t)g_result_r, (uint8_t)g_result_g, (uint8_t)g_result_b});
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(LED_PULSE_INTERVAL_MS));
    }
}

// ═══════════════════════════════════════════════════════════════
//  Web interface
// ═══════════════════════════════════════════════════════════════
static WebServer g_http(HTTP_PORT);
static DNSServer g_dns;

static void json_kv_f(String &o, const char *k, float v, uint8_t dec, bool valid = true) {
    o += '"'; o += k; o += "\":";
    if (!valid || isnan(v)) { o += "null"; return; }
    char b[16]; dtostrf(v, 0, dec, b); o += b;
}

static const char *failure_code_text(uint8_t fc) {
    switch (fc) {
        case FC_OK:         return "OK - không lỗi";
        case FC_OVERLOADED: return "Quá tải / quá dòng";
        case 3:             return "Lỗi sạc (đứt cầu chì, hỏng mosfet, hoặc cell > 4.37V)";
        case 4:             return "Lỗi (chưa xác định nguyên nhân)";
        case FC_WARNING:    return "Cảnh báo";
        case 7:             return "Chênh NTC > 50°C / lỗi EEPROM / cell > 4.22V / lệch cell > 300mV";
        case FC_DEAD:       return "Dung lượng thực dưới 70% dung lượng danh định";
        default:            return "Mã lỗi không xác định";
    }
}

static const char *origin_text(uint8_t b1) {
    switch (b1) {
        case 0x26: return "Trung Quốc / Murata (26)";
        case 0x36: return "Việt Nam / Samsung (36)";
        case 0x31: return "Dòng cũ (31)";
        default:   return "Không xác định";
    }
}

static const char *state_text() {
    if (g_busy) return "busy";
    switch (g_state) {
        case WAIT_BATTERY: return "waiting";
        case SCAN_PENDING: return "pending";
        case IDLE:         return "idle";
        default:           return "error";
    }
}

static void build_status_json(String &o) {
    ScanSnapshot s;
    snap_lock();
    s = g_snap;
    snap_unlock();

    o.reserve(2600);
    o += '{';
    o += "\"fw\":\"" FIRMWARE_VERSION "\",";
    o += "\"state\":\"";  o += state_text(); o += "\",";
    o += "\"activity\":\""; json_escape(g_activity, o); o += "\",";
    o += "\"busy\":";      o += g_busy ? "true" : "false";      o += ',';
    o += "\"present\":";   o += g_present ? "true" : "false";   o += ',';
    o += "\"mode\":\"";    o += (g_mode == MODE_LOCK ? "lock" : "scan"); o += "\",";
    o += "\"auto_unlock\":"; o += g_auto_unlock ? "true" : "false"; o += ',';
    o += "\"uptime\":";    o += (millis() / 1000);              o += ',';
    o += "\"heap\":";      o += (uint32_t)ESP.getFreeHeap();    o += ',';
    o += "\"clients\":";   o += WiFi.softAPgetStationNum();     o += ',';
    o += "\"pre_type0\":"; o += s.pre_type0 ? "true" : "false"; o += ',';
    o += "\"error\":";
    if (s.last_error[0]) { o += '"'; json_escape(s.last_error, o); o += '"'; } else o += "null";
    o += ',';
    o += "\"have\":"; o += s.valid ? "true" : "false";

    if (s.valid) {
        const BatteryInfo &i = s.info;
        char buf[80];

        o += ",\"age_s\":"; o += (millis() - s.timestamp_ms) / 1000;
        o += ",\"model\":\""; json_escape(i.model, o); o += '"';

        o += ",\"rom_valid\":"; o += i.rom_id_valid ? "true" : "false";
        o += ",\"rom\":\"";
        for (uint8_t k = 0; k < 8; k++) { snprintf(buf, sizeof(buf), "%02X%s", i.rom_id[k], k < 7 ? " " : ""); o += buf; }
        o += '"';
        o += ",\"mfg\":";
        if (i.rom_id_valid) {
            snprintf(buf, sizeof(buf), "\"%02d/%02d/20%02d\"", i.rom_id[2], i.rom_id[1], i.rom_id[0]);
            o += buf;
        } else o += "null";

        o += ",\"type\":";      o += batt_type_to_int(i.type);
        o += ",\"type_raw\":";  o += i.raw.batt_type;
        o += ",\"family\":\"";
        o += (i.raw.batt_type < 13) ? "4 cell BL14xx" : (i.raw.batt_type < 30) ? "5 cell BL18xx" : "10 cell BL36xx";
        o += '"';
        o += ",\"cell_count\":"; o += i.cell_count;
        o += ','; json_kv_f(o, "capacity_ah", i.capacity_ah, 1);

        o += ",\"locked\":";       o += i.locked ? "true" : "false";
        o += ",\"cell_failure\":"; o += i.raw.cell_failure ? "true" : "false";
        o += ",\"cs\":[";
        for (uint8_t k = 0; k < 3; k++) { o += i.checksums_ok[k] ? "true" : "false"; if (k < 2) o += ','; }
        o += "],\"aux\":[";
        for (uint8_t k = 0; k < 2; k++) { o += i.aux_checksums_ok[k] ? "true" : "false"; if (k < 1) o += ','; }
        o += ']';
        o += ",\"lock_causes\":[";
        {
            bool first = true;
            if (i.lock_causes & LF_CS0) { o += "\"Sai checksum CS0 (nybble 0-15)\""; first = false; }
            if (i.lock_causes & LF_CS2) { if (!first) o += ','; o += "\"Sai checksum CS2 (nybble 32-40)\""; first = false; }
            if (i.lock_causes & LF_N34) {
                if (!first) o += ',';
                snprintf(buf, sizeof(buf), "\"Nybble 34 = 0x%X (phải bằng 0 - khóa của bộ sạc)\"", nybble_get(s.frame, 34));
                o += buf;
            }
        }
        o += ']';
        o += ",\"unlock_supported\":"; o += type_supports_unlock(i.type) ? "true" : "false";

        o += ",\"origin\":\"";  o += origin_text(s.frame[1]); o += '"';
        o += ",\"n34\":";       o += nybble_get(s.frame, 34);
        snprintf(buf, sizeof(buf), ",\"byte19\":\"0x%02X\"", s.frame[19]); o += buf;
        o += ",\"failure_code\":"; o += i.raw.failure_code;
        o += ",\"failure_text\":\""; json_escape(failure_code_text(i.raw.failure_code), o); o += '"';

        o += ",\"cycles\":"; o += i.raw.cycles;
        o += ','; json_kv_f(o, "health", i.health.rating, 2, i.health.rating > 0.0f);
        o += ",\"od_count\":";  o += i.health.od_count;
        o += ",\"ol_count\":";  o += i.health.overload_count;
        o += ',';
        if (is_type_023(i.type) && i.raw.cycles > 0 && i.health.od_count > 0)
            json_kv_f(o, "od_pct", 4.0f + 100.0f * i.health.od_count / i.raw.cycles, 1);
        else if (is_type_56(i.type) && i.raw.overdischarge > 0)
            json_kv_f(o, "od_pct", -5.0f * i.raw.overdischarge + 160.0f, 1);
        else json_kv_f(o, "od_pct", 0, 1, false);
        o += ',';
        if (is_type_023(i.type) && i.raw.cycles > 0 && i.health.overload_count > 0)
            json_kv_f(o, "ol_pct", 4.0f + 100.0f * i.health.overload_count / i.raw.cycles, 1);
        else if (is_type_56(i.type) && i.raw.overload > 0)
            json_kv_f(o, "ol_pct", 5.0f * i.raw.overload - 160.0f, 1);
        else json_kv_f(o, "ol_pct", 0, 1, false);

        o += ",\"soc\":";
        if (is_type_023(i.type) && i.health.charge_level > 0 && i.raw.capacity > 0) {
            float ratio = (float)i.health.charge_level / i.raw.capacity / SOC_DIVISOR;
            uint8_t soc = (ratio < 10.0f) ? 1 : (uint8_t)min((int)(ratio / 10.0f), 7);
            o += soc;
        } else o += "null";

        o += ','; json_kv_f(o, "t_cell",   i.temperature_c,        1, i.temperature_c        != TEMP_INVALID);
        o += ','; json_kv_f(o, "t_mosfet", i.temperature_mosfet_c, 1, i.temperature_mosfet_c != TEMP_INVALID);

        o += ','; json_kv_f(o, "vpack", s.vr.vpack, 3, s.vr.ok);
        o += ",\"cells\":[";
        if (s.vr.cells.valid) {
            for (uint8_t k = 0; k < s.vr.cells.n; k++) {
                dtostrf(s.vr.cells.v[k], 0, 3, buf); o += buf;
                if (k < s.vr.cells.n - 1) o += ',';
            }
        }
        o += ']';
        o += ','; json_kv_f(o, "imbalance", s.imbalance_v, 3, s.vr.cells.valid && s.vr.cells.n > 1);
        o += ",\"imbalanced\":"; o += s.imbalanced ? "true" : "false";

        o += ",\"frame\":\"";
        for (uint8_t k = 0; k < BASIC_INFO_LEN; k++) {
            snprintf(buf, sizeof(buf), "%02X%s", s.frame[k], k < BASIC_INFO_LEN - 1 ? " " : "");
            o += buf;
        }
        o += '"';
    }
    o += '}';
}

static void handle_state() {
    uint32_t since = g_http.hasArg("since") ? (uint32_t)strtoul(g_http.arg("since").c_str(), nullptr, 10) : 0;
    // Cap the backlog so the first poll cannot allocate the whole ring buffer.
    uint32_t seq_now = LOG.seq();
    if (seq_now - since > LOG_MAX_RESPONSE_LINES) since = seq_now - LOG_MAX_RESPONSE_LINES;
    String out;
    out.reserve(3600);
    out += "{\"status\":";
    build_status_json(out);
    out += ",\"log\":[";
    uint32_t seq = LOG.collect(since, out);
    out += "],\"seq\":";
    out += seq;
    out += '}';
    g_http.sendHeader("Cache-Control", "no-store");
    g_http.send(200, "application/json; charset=utf-8", out);
}

static void reply_cmd(bool ok, const char *msg) {
    String o = "{\"ok\":";
    o += ok ? "true" : "false";
    o += ",\"msg\":\"";
    json_escape(msg, o);
    o += "\"}";
    g_http.send(ok ? 200 : 409, "application/json; charset=utf-8", o);
}

static void handle_cmd() {
    String a = g_http.arg("a");

    if (a == "clearlog")   { LOG.clear();  reply_cmd(true, "Đã xóa nhật ký"); return; }
    if (a == "clearscan")  { snapshot_clear(); reply_cmd(true, "Đã xóa kết quả"); return; }
    if (a == "autounlock") {
        g_auto_unlock = (g_http.arg("v") == "1");
        reply_cmd(true, g_auto_unlock ? "Bật tự động mở khóa" : "Tắt tự động mở khóa");
        return;
    }

    if (g_busy || g_action != ACT_NONE) { reply_cmd(false, "Thiết bị đang bận"); return; }

    if (a == "scan")   { g_action = ACT_SCAN;   reply_cmd(true, "Bắt đầu quét");    return; }
    if (a == "unlock") { g_action = ACT_UNLOCK; reply_cmd(true, "Bắt đầu mở khóa"); return; }
    if (a == "omega")  { g_action = ACT_OMEGA_LOCK; reply_cmd(true, "Bắt đầu khóa Omega"); return; }

    reply_cmd(false, "Lệnh không hợp lệ");
}

static const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="vi"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b0f14">
<title>Makita Battery Monitor</title>
<style>
:root{
 --bg:#0b0f14; --card:#141a22; --card2:#1b2430; --line:#243040;
 --txt:#e8eef6; --dim:#8ea0b5; --acc:#00c2ff; --ok:#25d07d; --warn:#ffb020; --bad:#ff4d5e; --pur:#b06cff;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--txt);
 font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,system-ui,sans-serif;
 padding:0 12px 96px;font-size:15px}
header{position:sticky;top:0;z-index:20;background:rgba(11,15,20,.94);backdrop-filter:blur(8px);
 margin:0 -12px;padding:12px 14px 10px;border-bottom:1px solid var(--line)}
h1{margin:0;font-size:17px;font-weight:650;letter-spacing:.2px}
.sub{color:var(--dim);font-size:12px;margin-top:3px;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--bad);display:inline-block}
.dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-top:12px}
.card h2{margin:0 0 10px;font-size:13px;font-weight:600;color:var(--dim);text-transform:uppercase;letter-spacing:.8px}
.row{display:flex;justify-content:space-between;gap:10px;padding:7px 0;border-bottom:1px dashed rgba(255,255,255,.05);font-size:14px}
.row:last-child{border-bottom:0}
.row .k{color:var(--dim)}
.row .v{font-weight:600;text-align:right;word-break:break-word}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.big{display:flex;align-items:baseline;gap:8px;margin:2px 0 12px}
.big b{font-size:38px;font-weight:700;line-height:1}
.big span{color:var(--dim);font-size:14px}
.badge{display:inline-block;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:700}
.b-ok{background:rgba(37,208,125,.15);color:var(--ok)}
.b-bad{background:rgba(255,77,94,.15);color:var(--bad)}
.b-warn{background:rgba(255,176,32,.15);color:var(--warn)}
.b-dim{background:rgba(142,160,181,.15);color:var(--dim)}
.cell{margin:9px 0}
.cell .lab{display:flex;justify-content:space-between;font-size:13px;margin-bottom:4px}
.bar{height:9px;border-radius:6px;background:var(--card2);overflow:hidden}
.bar i{display:block;height:100%;border-radius:6px;background:linear-gradient(90deg,var(--acc),var(--ok));transition:width .4s}
.bar i.lo{background:linear-gradient(90deg,#ff8a3d,var(--bad))}
.bar i.hi{background:linear-gradient(90deg,var(--acc),var(--pur))}
.hbar{height:12px;border-radius:8px;background:var(--card2);overflow:hidden;margin-top:6px}
.hbar i{display:block;height:100%;transition:width .4s}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.tile{background:var(--card2);border-radius:11px;padding:11px}
.tile .k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.6px}
.tile .v{font-size:19px;font-weight:700;margin-top:3px}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:4px}
.chip{font-size:11px;padding:3px 8px;border-radius:8px;background:var(--card2);color:var(--dim)}
.chip.ok{color:var(--ok)} .chip.bad{color:var(--bad);background:rgba(255,77,94,.12)}
ul.causes{margin:8px 0 0;padding-left:18px;color:var(--bad);font-size:13px}
ul.causes li{margin:3px 0}
pre.frame{background:#0a0e13;border:1px solid var(--line);border-radius:10px;padding:10px;margin:0;
 font-size:12px;line-height:1.6;color:#9fd0ff;white-space:pre-wrap;word-break:break-all}
#log{background:#080b0f;border:1px solid var(--line);border-radius:10px;padding:10px;height:230px;
 overflow-y:auto;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:11.5px;line-height:1.55;color:#b8c7d9}
#log div{white-space:pre-wrap;word-break:break-word}
#log .w{color:var(--warn)} #log .e{color:var(--bad)} #log .g{color:var(--ok)}
.actions{position:fixed;left:0;right:0;bottom:0;z-index:30;background:rgba(11,15,20,.96);
 backdrop-filter:blur(8px);border-top:1px solid var(--line);padding:10px 12px calc(10px + env(safe-area-inset-bottom));
 display:flex;gap:8px}
button{flex:1;border:0;border-radius:11px;padding:13px 8px;font-size:14px;font-weight:650;color:#04121c;
 background:var(--acc);cursor:pointer}
button.sec{background:var(--card2);color:var(--txt);border:1px solid var(--line)}
button.warn{background:var(--warn)}
button.danger{background:var(--bad);color:#fff}
button:disabled{opacity:.45}
.toolbar{display:flex;gap:8px;align-items:center;justify-content:space-between;margin-bottom:8px}
.toolbar button{flex:0 0 auto;padding:7px 12px;font-size:12px}
label.sw{display:flex;align-items:center;gap:9px;font-size:13.5px;color:var(--txt);padding:5px 0}
label.sw input{width:44px;height:25px;appearance:none;background:var(--card2);border-radius:999px;position:relative;
 outline:0;border:1px solid var(--line);transition:.2s;flex:0 0 auto}
label.sw input:checked{background:var(--acc);border-color:var(--acc)}
label.sw input::after{content:"";position:absolute;top:2px;left:2px;width:19px;height:19px;border-radius:50%;
 background:#fff;transition:.2s}
label.sw input:checked::after{left:21px}
.hint{color:var(--dim);font-size:12px;margin-top:6px;line-height:1.5}
.hide{display:none}
#toast{position:fixed;left:50%;transform:translateX(-50%);bottom:86px;background:#1e2733;color:var(--txt);
 padding:10px 16px;border-radius:10px;font-size:13px;border:1px solid var(--line);opacity:0;transition:.25s;z-index:40}
#toast.on{opacity:1}
</style></head><body>
<header>
  <h1>🔋 Makita Battery Monitor</h1>
  <div class="sub">
    <span><span id="dot" class="dot"></span> <span id="conn">đang kết nối…</span></span>
    <span id="act">—</span>
    <span id="modep" class="badge b-dim">QUÉT</span>
  </div>
</header>

<div class="card" id="cardState">
  <h2>Trạng thái</h2>
  <div class="big"><b id="vpack">--</b><span>V tổng</span>
    <span style="margin-left:auto" id="lockBadge" class="badge b-dim">chưa quét</span></div>
  <div class="row"><span class="k">Model</span><span class="v" id="model">—</span></div>
  <div class="row"><span class="k">Loại (giao thức)</span><span class="v" id="type">—</span></div>
  <div class="row"><span class="k">Cấu hình cell</span><span class="v" id="family">—</span></div>
  <div class="row"><span class="k">Dung lượng danh định</span><span class="v" id="cap">—</span></div>
  <div class="row"><span class="k">Ngày sản xuất</span><span class="v" id="mfg">—</span></div>
  <div class="row"><span class="k">ROM ID</span><span class="v mono" id="rom">—</span></div>
  <div class="row"><span class="k">Xuất xứ (byte 1)</span><span class="v" id="origin">—</span></div>
  <div id="errBox" class="hint hide"></div>
</div>

<div class="card">
  <h2>Điện áp từng cell</h2>
  <div id="cells"></div>
  <div class="grid" style="margin-top:10px">
    <div class="tile"><div class="k">Chênh lệch cell</div><div class="v" id="imb">—</div></div>
    <div class="tile"><div class="k">Điện áp tổng</div><div class="v" id="vpack2">—</div></div>
  </div>
  <div id="imbWarn" class="hint hide" style="color:var(--warn)">
    ⚠ Lệch cell ≥ 0.300 V — nhiều khả năng đứt lá cân bằng (balancing tab). Cần mở pin kiểm tra.
  </div>
</div>

<div class="card">
  <h2>Tình trạng &amp; tuổi thọ</h2>
  <div class="row"><span class="k">Sức khỏe (health)</span><span class="v" id="health">—</span></div>
  <div class="hbar"><i id="healthBar" style="width:0%"></i></div>
  <div class="grid" style="margin-top:12px">
    <div class="tile"><div class="k">Số chu kỳ sạc</div><div class="v" id="cycles">—</div></div>
    <div class="tile"><div class="k">Mức pin (SOC)</div><div class="v" id="soc">—</div></div>
    <div class="tile"><div class="k">Nhiệt độ cell</div><div class="v" id="tcell">—</div></div>
    <div class="tile"><div class="k">Nhiệt độ mosfet</div><div class="v" id="tmos">—</div></div>
    <div class="tile"><div class="k">Xả kiệt (OD)</div><div class="v" id="od">—</div></div>
    <div class="tile"><div class="k">Quá tải (OL)</div><div class="v" id="ol">—</div></div>
  </div>
</div>

<div class="card">
  <h2>Khóa &amp; chẩn đoán</h2>
  <div class="row"><span class="k">Trạng thái khóa</span><span class="v" id="lock2">—</span></div>
  <div class="row"><span class="k">Mã lỗi (nybble 40)</span><span class="v" id="fail">—</span></div>
  <div class="row"><span class="k">Lỗi cell</span><span class="v" id="cfail">—</span></div>
  <div class="row"><span class="k">Nybble 34 (khóa sạc)</span><span class="v" id="n34">—</span></div>
  <div class="row"><span class="k">Byte 19 (status)</span><span class="v mono" id="b19">—</span></div>
  <div class="row"><span class="k">Checksum</span><span class="v"><span class="chips" id="csums"></span></span></div>
  <ul class="causes hide" id="causes"></ul>
</div>

<div class="card">
  <h2>Khung dữ liệu thô (32 byte)</h2>
  <pre class="frame mono" id="frame">—</pre>
</div>

<div class="card">
  <h2>Tùy chọn</h2>
  <label class="sw"><input type="checkbox" id="autoUnlock"><span>Tự động mở khóa ngay khi phát hiện pin bị khóa</span></label>
  <div class="hint">Khi tắt (mặc định), thao tác quét chỉ <b>đọc</b> — không ghi gì vào BMS. Ghi chỉ xảy ra khi bạn bấm “Mở khóa” hoặc “Khóa Omega”.</div>
  <label class="sw" style="margin-top:6px"><input type="checkbox" id="autoScroll" checked><span>Tự cuộn nhật ký</span></label>
</div>

<div class="card">
  <div class="toolbar">
    <h2 style="margin:0">Nhật ký giao tiếp</h2>
    <button class="sec" onclick="cmd('clearlog')">Xóa</button>
  </div>
  <div id="log"></div>
</div>

<div class="actions">
  <button id="bScan" onclick="cmd('scan')">Quét lại</button>
  <button id="bUnlock" class="warn" onclick="doUnlock()">Mở khóa</button>
  <button id="bOmega" class="danger" onclick="doOmega()">Khóa Omega</button>
</div>
<div id="toast"></div>

<script>
let seq = 0, fails = 0, st = {};
const $ = id => document.getElementById(id);

function toast(m){const t=$('toast');t.textContent=m;t.classList.add('on');
  clearTimeout(t._t);t._t=setTimeout(()=>t.classList.remove('on'),2200);}

async function cmd(a, v){
  try{
    const q = '/api/cmd?a=' + a + (v===undefined?'':'&v='+v);
    const r = await fetch(q, {method:'POST'});
    const j = await r.json();
    toast(j.msg);
    poll();
  }catch(e){ toast('Không gửi được lệnh'); }
}
function doUnlock(){
  if(confirm('Mở khóa sẽ GHI dữ liệu vào BMS của pin (DA04 + sửa frame). Tiếp tục?')) cmd('unlock');
}
function doOmega(){
  if(confirm('KHÓA OMEGA sẽ ghi nybble 34 khác 0 khiến bộ sạc từ chối pin này. Có thể gỡ bằng nút Mở khóa. Tiếp tục?')) cmd('omega');
}

function fmt(v, d, unit){ return (v===null||v===undefined)?'—':(Number(v).toFixed(d)+(unit||'')); }

function renderCells(s){
  const box = $('cells');
  const cs = s.cells || [];
  if(!cs.length){ box.innerHTML = '<div class="hint">Chưa có dữ liệu cell.</div>'; return; }
  const mx = Math.max(...cs), mn = Math.min(...cs);
  box.innerHTML = cs.map((v,i)=>{
    const pct = Math.max(2, Math.min(100, ((v-2.5)/(4.2-2.5))*100));
    const cls = v<3.0 ? 'lo' : (v>4.15 ? 'hi' : '');
    const tag = (cs.length>1 && v===mn) ? ' ↓' : ((cs.length>1 && v===mx) ? ' ↑' : '');
    return `<div class="cell"><div class="lab"><span>Cell ${i+1}${tag}</span><b>${v.toFixed(3)} V</b></div>
            <div class="bar"><i class="${cls}" style="width:${pct}%"></i></div></div>`;
  }).join('');
}

function render(s){
  st = s;
  $('act').textContent = s.activity || '';
  $('modep').textContent = s.mode === 'lock' ? 'KHÓA OMEGA' : 'QUÉT';
  $('modep').className = 'badge ' + (s.mode === 'lock' ? 'b-bad' : 'b-dim');
  $('autoUnlock').checked = !!s.auto_unlock;

  const busy = s.busy;
  ['bScan','bUnlock','bOmega'].forEach(id => $(id).disabled = busy);
  $('bUnlock').disabled = busy || !(s.have && s.unlock_supported);
  $('bOmega').disabled  = busy || !(s.have && s.unlock_supported);

  const eb = $('errBox');
  if(s.error){ eb.textContent = '⚠ ' + s.error; eb.classList.remove('hide'); }
  else eb.classList.add('hide');

  if(!s.have){
    $('lockBadge').textContent = s.present ? 'đang xử lý' : 'chưa có pin';
    $('lockBadge').className = 'badge b-dim';
    return;
  }

  $('vpack').textContent  = fmt(s.vpack,2);
  $('vpack2').textContent = fmt(s.vpack,3,' V');
  $('model').textContent  = s.model || '—';
  $('type').textContent   = 'Type ' + s.type + ' (raw ' + s.type_raw + ')';
  $('family').textContent = s.family;
  $('cap').textContent    = fmt(s.capacity_ah,1,' Ah');
  $('mfg').textContent    = s.mfg || '—';
  $('rom').textContent    = s.rom_valid ? s.rom : 'không đọc được';
  $('origin').textContent = s.origin;

  const lb = $('lockBadge');
  lb.textContent = s.locked ? 'ĐANG BỊ KHÓA' : 'KHÔNG KHÓA';
  lb.className = 'badge ' + (s.locked ? 'b-bad' : 'b-ok');
  $('lock2').innerHTML = s.locked
    ? '<span class="badge b-bad">LOCKED</span>'
    : '<span class="badge b-ok">UNLOCKED</span>';

  renderCells(s);
  $('imb').textContent = fmt(s.imbalance,3,' V');
  $('imbWarn').classList.toggle('hide', !s.imbalanced);

  const h = (s.health===null||s.health===undefined) ? null : Math.max(0, Math.min(4, s.health));
  const hn = h===null ? 0 : Math.round(h);
  $('health').textContent = (h===null) ? '—' : (h.toFixed(2) + ' / 4  ' + '█'.repeat(hn) + '░'.repeat(4-hn));
  const hb = $('healthBar');
  hb.style.width = (h===null?0:(h/4*100)) + '%';
  hb.style.background = h===null ? 'var(--card2)' : (h>=3 ? 'var(--ok)' : (h>=1.5 ? 'var(--warn)' : 'var(--bad)'));

  $('cycles').textContent = s.cycles ?? '—';
  $('soc').textContent    = (s.soc===null||s.soc===undefined) ? '—' : (s.soc + ' / 7');
  $('tcell').textContent  = fmt(s.t_cell,1,' °C');
  $('tmos').textContent   = fmt(s.t_mosfet,1,' °C');
  $('od').textContent     = (s.od_pct===null?'—':s.od_pct.toFixed(1)+' %') + (s.od_count? ' ('+s.od_count+')':'');
  $('ol').textContent     = (s.ol_pct===null?'—':s.ol_pct.toFixed(1)+' %') + (s.ol_count? ' ('+s.ol_count+')':'');

  $('fail').textContent  = s.failure_code + ' — ' + s.failure_text;
  $('cfail').innerHTML   = s.cell_failure ? '<span class="badge b-bad">CÓ</span>' : '<span class="badge b-ok">Không</span>';
  $('n34').innerHTML     = s.n34===0 ? '<span class="badge b-ok">0 (OK)</span>' : '<span class="badge b-bad">'+s.n34+' (khóa)</span>';
  $('b19').textContent   = s.byte19;

  const names=['CS0 0-15','CS1 16-31','CS2 32-40','AUX 44-47','AUX 48-61'];
  const vals=[...s.cs, ...s.aux];
  $('csums').innerHTML = vals.map((ok,i)=>`<span class="chip ${ok?'ok':'bad'}">${names[i]} ${ok?'OK':'SAI'}</span>`).join('');

  const cz = $('causes');
  if(s.lock_causes && s.lock_causes.length){
    cz.innerHTML = s.lock_causes.map(c=>'<li>'+c+'</li>').join('');
    cz.classList.remove('hide');
  } else cz.classList.add('hide');

  const f = s.frame.split(' ');
  let out = '';
  for(let i=0;i<f.length;i+=8) out += f.slice(i,i+8).join(' ') + '\n';
  $('frame').textContent = out.trim();
}

function appendLog(lines){
  if(!lines.length) return;
  const el = $('log');
  const stick = $('autoScroll').checked;
  lines.forEach(l=>{
    const d = document.createElement('div');
    if(/ERROR|FAILED|BAD -|no presence|TIMEOUT/i.test(l)) d.className='e';
    else if(/WARNING|LOCKED|Still locked/i.test(l)) d.className='w';
    else if(/UNLOCKED|Complete|OK \(/i.test(l)) d.className='g';
    d.textContent = l;
    el.appendChild(d);
  });
  while(el.childElementCount > 400) el.removeChild(el.firstChild);
  if(stick) el.scrollTop = el.scrollHeight;
}

async function poll(){
  try{
    const r = await fetch('/api/state?since=' + seq, {cache:'no-store'});
    const j = await r.json();
    if(j.seq < seq){ $('log').innerHTML=''; }   // log was cleared
    seq = j.seq;
    appendLog(j.log);
    render(j.status);
    fails = 0;
    $('dot').classList.add('on');
    $('conn').textContent = 'đã kết nối · ' + j.status.clients + ' máy';
  }catch(e){
    console.error(e);
    if(++fails > 2){ $('dot').classList.remove('on'); $('conn').textContent='mất kết nối'; }
  }
}
$('autoUnlock').addEventListener('change', e => cmd('autounlock', e.target.checked ? 1 : 0));
poll();
setInterval(poll, 1000);
</script>
</body></html>)HTMLPAGE";

static void handle_root() {
    g_http.sendHeader("Cache-Control", "no-store");
    g_http.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handle_not_found() {
    // Captive-portal style: bounce everything else to the UI.
    g_http.sendHeader("Location", "http://192.168.4.1/", true);
    g_http.send(302, "text/plain", "");
}

static void web_begin() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
    // Power save off keeps the UI responsive while the 1-Wire task holds
    // interrupts disabled during bit-banging.
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Reduced TX power: the phone sits next to the jig, and the current peaks
    // of full power brown out boards fed through a thin USB cable.
    WiFi.setTxPower((wifi_power_t)AP_TX_POWER);

    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    g_dns.start(DNS_PORT, "*", IPAddress(192,168,4,1));

    g_http.on("/",           HTTP_GET,  handle_root);
    g_http.on("/api/state",  HTTP_GET,  handle_state);
    g_http.on("/api/cmd",    HTTP_POST, handle_cmd);
    g_http.on("/api/cmd",    HTTP_GET,  handle_cmd);
    g_http.onNotFound(handle_not_found);
    g_http.begin();

    print_sep();
    LOG.print(F("  Wi-Fi AP       : ")); LOG.println(AP_SSID);
    LOG.print(F("  Web UI         : http://")); LOG.println(WiFi.softAPIP());
    print_sep();
}

// ═══════════════════════════════════════════════════════════════
//  Battery task — hot-swap state machine (was loop() on RP2040)
// ═══════════════════════════════════════════════════════════════
static void run_action(PendingAction act, DeviceMode cur_mode) {
    g_busy = true;
    g_pulse_active = false;
    g_imbalance    = false;
    bool ok;
    if (act == ACT_OMEGA_LOCK || (act == ACT_SCAN && cur_mode == MODE_LOCK)) {
        set_activity("Đang khóa Omega…");
        ok = run_lock();
    } else {
        ok = run_scan(act == ACT_UNLOCK || (g_auto_unlock && cur_mode == MODE_SCAN));
    }
    g_state = ok ? IDLE : UNSUPPORTED;
    if (!ok) LOG.println(F("  Remove battery and try again."));
    set_activity(ok ? "Hoàn tất" : "Lỗi - rút pin và thử lại");
    g_busy = false;
}

static void battery_task(void *) {
    wdt_begin();
    g_state = WAIT_BATTERY;
    set_activity("Đang chờ pin…");

    for (;;) {
        wdt_kick();
        uint32_t now = millis();
        DeviceMode cur_mode = g_last_mode;

        // Mode-change detection (debounced, 4x50 ms ~= 200 ms)
        if (now - g_last_mode_poll >= MODE_DEBOUNCE_MS) {
            g_last_mode_poll = now;
            DeviceMode raw = mode_read();
            if (raw == g_last_mode) {
                g_mode_debounce = 0;
            } else if (++g_mode_debounce >= MODE_DEBOUNCE_COUNT) {
                g_mode_debounce = 0; g_last_mode = raw; cur_mode = raw; g_mode = raw;
                if (g_state==IDLE || g_state==WAIT_BATTERY || g_state==UNSUPPORTED) {
                    led_off(); g_state = WAIT_BATTERY;
                    print_sep();
                    switch (cur_mode) {
                        case MODE_LOCK:
                            LOG.println(F("  Mode changed -> OMEGA LOCK MODE"));
                            LOG.println(F("  GPIO5-GPIO7 bridged. Insert battery to lock."));
                            break;
                        default:
                            LOG.println(F("  Mode changed -> SCAN / UNLOCK MODE"));
                            LOG.println(F("  Bridge removed. Insert battery to scan."));
                            break;
                    }
                    print_sep();
                }
            }
        }

        // Serial command (scan mode only)
        if (cur_mode == MODE_SCAN && LOG.available()) {
            char c = (char)LOG.read();
            if ((c=='s'||c=='S') && g_action == ACT_NONE) {
                LOG.println(F("Manual rescan requested."));
                g_action = ACT_SCAN;
            }
        }

        // Web-requested action takes priority over the presence poll
        PendingAction act = g_action;
        if (act != ACT_NONE) {
            g_action = ACT_NONE;
            print_sep();
            LOG.println(act == ACT_UNLOCK      ? F("  Web request: UNLOCK")
                      : act == ACT_OMEGA_LOCK  ? F("  Web request: OMEGA LOCK")
                                               : F("  Web request: SCAN"));
            print_sep();
            run_action(act, cur_mode);
            g_last_poll = millis();
            continue;
        }

        // Presence poll
        if (now - g_last_poll >= POLL_INTERVAL_MS) {
            g_last_poll = now;
            bool present = battery_present();
            g_present = present;
            if (g_state==WAIT_BATTERY && present) {
                print_sep();
                if (cur_mode == MODE_LOCK) LOG.println(F("  [Lock] Battery detected - omega locking..."));
                else                       LOG.println(F("  Battery detected - starting scan."));
                print_sep();
                run_action(ACT_SCAN, cur_mode);
                g_last_poll = millis();
                continue;
            } else if ((g_state==IDLE || g_state==UNSUPPORTED) && !present) {
                print_sep();
                if (cur_mode == MODE_LOCK) LOG.println(F("  [Lock] Battery removed. Waiting for next..."));
                else                       LOG.println(F("  Battery removed. Waiting..."));
                print_sep();
                led_off(); g_state = WAIT_BATTERY; g_imbalance = false;
                set_activity("Đang chờ pin…");
            }
        }

        // Idle LED pulse — driven by the LED task via g_pulse_active
        if (g_state == WAIT_BATTERY) {
            g_pulse_scan_mode = (cur_mode == MODE_SCAN);
            g_pulse_active    = true;
        } else {
            g_pulse_active = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── Arduino entry points ─────────────────────────────────────
static TaskHandle_t g_led_task  = nullptr;
static TaskHandle_t g_batt_task = nullptr;

static const char *reset_reason_text() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "POWERON (cap nguon binh thuong)";
        case ESP_RST_EXT:      return "EXT (nut reset)";
        case ESP_RST_SW:       return "SW (restart bang phan mem)";
        case ESP_RST_PANIC:    return "PANIC (crash - xem backtrace o tren)";
        case ESP_RST_INT_WDT:  return "INT_WDT (watchdog ngat)";
        case ESP_RST_TASK_WDT: return "TASK_WDT (watchdog tac vu)";
        case ESP_RST_WDT:      return "WDT (watchdog khac)";
        case ESP_RST_BROWNOUT: return "BROWNOUT (sut ap - nguon/cap USB yeu)";
        case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "UNKNOWN";
    }
}

void setup() {
    LOG.begin(115200);
    delay(300);   // give USB-CDC time to enumerate so boot lines are not lost

    g_snap_mutex = xSemaphoreCreateMutex();
    g_led_mutex  = xSemaphoreCreateMutex();
    g_log_mutex  = xSemaphoreCreateRecursiveMutex();
    LOG.attach_mutex(g_log_mutex);

    pinMode(PIN_ENABLE, OUTPUT); digitalWrite(PIN_ENABLE, LOW);
    pinMode(PIN_MODE_OUT, OUTPUT); digitalWrite(PIN_MODE_OUT, HIGH);
    pinMode(PIN_MODE_IN1, INPUT_PULLDOWN);

    if (!neopixel_begin()) LOG.println(F("WARNING: NeoPixel RMT init failed; LED disabled."));
    led_off();

    delay(200);  // let the mode pin settle before the first read

    print_sep();
    LOG.print(F("  Makita Battery Monitor - ESP32-C3 v")); LOG.println(F(FIRMWARE_VERSION));
    LOG.print(F("  Reset reason   : ")); LOG.println(reset_reason_text());
    LOG.print(F("  Free heap      : ")); LOG.println((uint32_t)ESP.getFreeHeap());
    LOG.print(F("  DATA=GPIO"));   LOG.print(PIN_ONEWIRE);
    LOG.print(F("  ENABLE=GPIO")); LOG.print(PIN_ENABLE);
    LOG.print(F("  LED=GPIO"));    LOG.print(NEOPIXEL_OUT_PIN);
    LOG.print(F("  MODE=GPIO"));   LOG.print(PIN_MODE_OUT);
    LOG.print('/');                LOG.println(PIN_MODE_IN1);

    DeviceMode m = mode_read();
    g_last_mode = m; g_mode = m;
    switch (m) {
        case MODE_LOCK:
            LOG.println(F("  OMEGA LOCK MODE - types 0 / 2 / 3 only"));
            LOG.println(F("  GPIO5-GPIO7 bridged. Remove bridge for scan mode."));
            break;
        default:
            LOG.println(F("  SCAN / UNLOCK MODE"));
            LOG.println(F("  's' = rescan | auto-detects connect/disconnect"));
            LOG.println(F("  Bridge GPIO5-GPIO7 for omega lock."));
            break;
    }

    web_begin();
    LOG.println(F("Waiting for battery..."));

    LOG.print(F("  Build          : LED task "));
    LOG.print(ENABLE_LED_TASK ? F("on") : F("off"));
    LOG.print(F(", battery task "));
    LOG.println(ENABLE_BATTERY_TASK ? F("on") : F("off"));
#if ENABLE_LED_TASK
    xTaskCreate(led_task,     "led",  4096, nullptr, 1, &g_led_task);
#endif
#if ENABLE_BATTERY_TASK
    xTaskCreate(battery_task, "batt", 12288, nullptr, 3, &g_batt_task);
#endif
}

// Heartbeat on serial only (kept out of the web log): a counter that restarts
// from zero makes an unexpected reboot obvious.
static void heartbeat() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 5000) return;
    last = now;
    // Stack head-room in bytes; a value approaching 0 means the task is about
    // to overflow its stack.
    unsigned led_free  = g_led_task  ? uxTaskGetStackHighWaterMark(g_led_task)  * 4 : 0;
    unsigned batt_free = g_batt_task ? uxTaskGetStackHighWaterMark(g_batt_task) * 4 : 0;
    Serial.printf("[hb] up=%us heap=%u minheap=%u clients=%u stack led=%u batt=%u\n",
                  (unsigned)(now / 1000), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(), (unsigned)WiFi.softAPgetStationNum(),
                  led_free, batt_free);
}

void loop() {
    g_dns.processNextRequest();
    g_http.handleClient();
    heartbeat();
    delay(2);
}
