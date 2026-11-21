/*
 * CAN Bus Simulator — XIAO ESP32S3
 *
 * Replays SavvyCAN CSV log files from an SD card over CAN bus.
 * Wiring:
 *   CAN  — D0(GPIO1)→TJA1051T TXD, D4(GPIO5)←TJA1051T RXD through a 10k/20k divider
 *          (moved off D1/GPIO2 2026-11-20: that input stopped reading the bus)
 *        — TJA1051T S pin → GND, VCC → 5V, VIO → 3.3V
 *        — 120Ω across CANH/CANL on this end of the bench bus
 *   SD   — D8(GPIO7)→SCK, D9(GPIO8)←MISO, D10(GPIO9)→MOSI, D3(GPIO4)→CS
 *        — SD module VCC → 3.3V
 *
 * Serial console (115200):
 *   ls              list CSV files on SD
 *   play <file>     start playback
 *   stop            stop playback
 *   loop            toggle loop mode (default: on)
 *   speed <n>       playback speed multiplier (e.g. 0.5 / 1 / 2)
 *   status          show current config
 *   ack on|off      require acknowledgements like a real ECU (default off)
 *   txtest          stop, drive TXD low/high by hand, check RXD echoes it
 */

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include "driver/twai.h"
#include "esp_timer.h"

// ── Pins ──────────────────────────────────────────────────────────────────────
static constexpr gpio_num_t kCanTx  = GPIO_NUM_1;
static constexpr gpio_num_t kCanRx  = GPIO_NUM_5;  // D4; D1 (GPIO2) suspected damaged
static constexpr uint8_t    kSdCs   = 4;
static constexpr uint8_t    kSdSck  = 7;
static constexpr uint8_t    kSdMiso = 8;
static constexpr uint8_t    kSdMosi = 9;

// ── State ─────────────────────────────────────────────────────────────────────
static SPIClass sdSpi(FSPI);
static bool     gLooping     = true;
static float    gSpeed       = 1.0f;
static int32_t  gMaxGapUs    = 1000000;  // cap inter-frame gaps at 1 second by default
static bool     gStopRequest = false;
static bool     gPlaying     = false;
static char     gFile[64]    = {};

// non-blocking console line buffer
static char gCmdBuf[128];
static int  gCmdLen = 0;

// ── Forward declarations ──────────────────────────────────────────────────────
void handleConsole();

// ── CAN ───────────────────────────────────────────────────────────────────────
// false (default): NO_ACK, a frame counts as sent without an acknowledgement, so the
// replay is clean with only a listen-only dongle on the bus.
// true: NORMAL, like a real ECU: an unacknowledged frame is resent until something
// acks it. With only a listen-only dongle attached this reproduces a car whose OBD
// port sits on its own segment with no other listener (2024 Blazer, 2026-11-04).
// The env xiao_s3_ack builds with SIM_REQUIRE_ACK=1, for tests run with no console
// (the sim powered from the dongle's port).
#ifndef SIM_REQUIRE_ACK
#define SIM_REQUIRE_ACK 0
#endif
static bool gRequireAck = SIM_REQUIRE_ACK;

static bool canStart() {
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        kCanTx, kCanRx, gRequireAck ? TWAI_MODE_NORMAL : TWAI_MODE_NO_ACK);
    g.tx_queue_len = 32;  // default 5 is too shallow for burst replays
    twai_timing_config_t  t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    return twai_start() == ESP_OK;
}

static void canRestart() {
    twai_stop();
    twai_driver_uninstall();
    Serial.printf("CAN: %s (%s)\n", canStart() ? "restarted" : "restart FAILED",
                  gRequireAck ? "NORMAL, frames resent until acked" : "NO_ACK");
}

// Recover from bus-off if the error counter tripped. Called periodically during playback.
static void canRecoverIfNeeded() {
    twai_status_info_t s;
    if (twai_get_status_info(&s) != ESP_OK) return;
    if (s.state == TWAI_STATE_BUS_OFF) {
        twai_initiate_recovery();
        Serial.printf("[CAN] bus-off (bus_err=%lu) — recovery initiated\n", s.bus_error_count);
    } else if (s.state == TWAI_STATE_STOPPED) {
        // Recovery ends in STOPPED, not RUNNING: the driver has to be started again,
        // or every later transmit fails and the sim is silent until it reboots.
        Serial.printf("[CAN] recovered — restart %s\n", twai_start() == ESP_OK ? "ok" : "FAILED");
    }
}

// ── CSV parser ────────────────────────────────────────────────────────────────
// SavvyCAN format: timestamp,0xID,true|false,Rx|Tx,bus,len,d0,..,d7
static bool parseLine(char* buf, twai_message_t& msg, double& ts) {
    char* p = strtok(buf, ",");  if (!p) return false;  ts = atof(p);
    p = strtok(NULL, ",");       if (!p) return false;  msg.identifier = strtoul(p, NULL, 16);
    p = strtok(NULL, ",");       if (!p) return false;  msg.extd = (p[0] == 't') ? 1 : 0;
    strtok(NULL, ",");  // dir  — ignored, replay both Rx and Tx to reproduce full bus traffic
    strtok(NULL, ",");  // bus  — ignored
    p = strtok(NULL, ",");  if (!p) return false;
    msg.data_length_code = (uint8_t)constrain(atoi(p), 0, 8);
    msg.rtr = 0;  msg.ss = 0;  msg.self = 0;  msg.dlc_non_comp = 0;
    memset(msg.data, 0, 8);
    for (int i = 0; i < msg.data_length_code; i++) {
        p = strtok(NULL, ",\r\n");
        if (!p) break;
        msg.data[i] = (uint8_t)strtoul(p, NULL, 16);
    }
    return true;
}

// ── SD helpers ────────────────────────────────────────────────────────────────
static void listCsvFiles() {
    File root = SD.open("/");
    if (!root) { Serial.println("SD open failed"); return; }
    bool found = false;
    Serial.println("CSV files on SD:");
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        const char* name = f.name();
        size_t n = strlen(name);
        if (!f.isDirectory() && n > 4 && strcasecmp(name + n - 4, ".csv") == 0) {
            Serial.printf("  %-36s  %lu B\n", name, (unsigned long)f.size());
            found = true;
        }
    }
    if (!found) Serial.println("  (none)");
    root.close();
}

static bool firstCsv(char* out, size_t maxLen) {
    File root = SD.open("/");
    if (!root) return false;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        const char* name = f.name();
        size_t n = strlen(name);
        if (!f.isDirectory() && n > 4 && strcasecmp(name + n - 4, ".csv") == 0) {
            snprintf(out, maxLen, "/%s", name);
            root.close();
            return true;
        }
    }
    root.close();
    return false;
}

// ── Playback ──────────────────────────────────────────────────────────────────
static void playFile(const char* path) {
    File f = SD.open(path);
    if (!f) { Serial.printf("Cannot open: %s\n", path); return; }

    Serial.printf("► %s  [%.1fx  loop=%s  'stop' to halt]\n",
                  path, gSpeed, gLooping ? "on" : "off");

    // skip header — any line that doesn't start with a digit
    char buf[256];
    while (f.available()) {
        int len = 0;
        while (f.available() && len < (int)sizeof(buf) - 1) {
            char c = f.read();
            if (c == '\n') break;
            if (c != '\r') buf[len++] = c;
        }
        buf[len] = '\0';
        if (isdigit((unsigned char)buf[0])) {
            // first data line — put it back by seeking, or just process it inline
            // Since SD doesn't support unget, we process it below in the main loop
            // by re-entering through the unified read path. Instead, use a flag.
            break;
        }
    }

    // Reset and replay from the first data line. Reopen to avoid seek complexity.
    f.close();
    f = SD.open(path);
    if (!f) return;

    bool     firstFrame = true;
    double   firstTs    = 0.0;
    double   prevTs     = 0.0;
    int64_t  wallNow    = 0;   // running wall-clock target in µs
    uint32_t count      = 0;
    uint32_t queueFull  = 0;   // twai_transmit timed out: TX queue full
    int64_t  lastServiceUs = 0;
    int64_t  lastStatsUs   = esp_timer_get_time();

    while (f.available() && !gStopRequest) {
        // Read one line
        int len = 0;
        while (f.available() && len < (int)sizeof(buf) - 1) {
            char c = f.read();
            if (c == '\n') break;
            if (c != '\r') buf[len++] = c;
        }
        buf[len] = '\0';
        if (len == 0) continue;
        if (!isdigit((unsigned char)buf[0])) continue;  // skip header / blank

        twai_message_t msg;
        double ts;
        if (!parseLine(buf, msg, ts)) continue;

        if (firstFrame) {
            firstTs    = ts;
            prevTs     = ts;
            wallNow    = esp_timer_get_time();
            firstFrame = false;
        }

        // Clamp inter-frame gap — handles both large forward jumps (pauses, corrupt
        // timestamps) and negative jumps (timestamp rollback after corrupt frame).
        double gapUs = ts - prevTs;
        if (gapUs > gMaxGapUs || gapUs < 0) {
            if (gapUs > 0)
                Serial.printf("[gap] %.0f ms capped to %lu ms\n",
                              gapUs / 1000.0, (unsigned long)(gMaxGapUs / 1000));
            gapUs = (gapUs < 0) ? 0 : gMaxGapUs;
        }
        wallNow += (int64_t)(gapUs / gSpeed);
        prevTs   = ts;

        int64_t targetUs = wallNow;
        while (!gStopRequest) {
            int64_t remaining = targetUs - esp_timer_get_time();
            if (remaining <= 0) break;
            if (remaining > 5000) {
                handleConsole();
                canRecoverIfNeeded();
                delay(1);
            } else {
                delayMicroseconds((uint32_t)remaining);
                break;
            }
        }

        if (!gStopRequest) {
            if (twai_transmit(&msg, pdMS_TO_TICKS(2)) == ESP_OK) count++;
            else queueFull++;  // queue full, or the driver isn't running
        }

        // A dense capture never leaves a gap over 5 ms, so the wait above never
        // serves the console. Serve it here too, and report progress every 2 s,
        // so a replay that isn't getting onto the bus is visible.
        int64_t now = esp_timer_get_time();
        if (now - lastServiceUs > 20000) {
            lastServiceUs = now;
            handleConsole();
            canRecoverIfNeeded();
        }
        if (now - lastStatsUs > 2000000) {
            lastStatsUs = now;
            twai_status_info_t st;
            if (twai_get_status_info(&st) == ESP_OK) {
                const char* states[] = {"STOPPED", "RUNNING", "BUS_OFF", "RECOVERING"};
                Serial.printf("[stats] queued=%u queue_full=%u state=%s tx_err=%lu "
                              "rx_err=%lu tx_failed=%lu bus_err=%lu arb_lost=%lu\n",
                              count, queueFull, st.state < 4 ? states[st.state] : "?",
                              st.tx_error_counter, st.rx_error_counter,
                              st.tx_failed_count, st.bus_error_count, st.arb_lost_count);
            }
        }
    }

    f.close();
    if (!gStopRequest) Serial.printf("■ done — %u frames queued, %u dropped (queue full)\n", count, queueFull);
}

// ── Console ───────────────────────────────────────────────────────────────────
static void processCommand(const char* cmd) {
    if (strcmp(cmd, "ls") == 0) {
        listCsvFiles();

    } else if (strncmp(cmd, "play ", 5) == 0) {
        const char* name = cmd + 5;
        snprintf(gFile, sizeof(gFile), "%s%s", name[0] == '/' ? "" : "/", name);
        gStopRequest = false;
        gPlaying     = true;

    } else if (strcmp(cmd, "stop") == 0 || strcmp(cmd, "q") == 0) {
        gStopRequest = true;
        gPlaying     = false;

    } else if (strcmp(cmd, "loop") == 0) {
        gLooping = !gLooping;
        Serial.printf("Loop: %s\n", gLooping ? "on" : "off");

    } else if (strncmp(cmd, "speed ", 6) == 0) {
        float s = atof(cmd + 6);
        gSpeed = (s > 0.0f) ? s : 1.0f;
        Serial.printf("Speed: %.2fx\n", gSpeed);

    } else if (strncmp(cmd, "maxgap ", 7) == 0) {
        int ms = atoi(cmd + 7);
        gMaxGapUs = (ms > 0) ? (int32_t)(ms * 1000) : 1000000;
        Serial.printf("Max gap: %d ms\n", (int)(gMaxGapUs / 1000));

    } else if (strcmp(cmd, "ack on") == 0 || strcmp(cmd, "ack off") == 0) {
        gRequireAck = strcmp(cmd, "ack on") == 0;
        canRestart();

    } else if (strcmp(cmd, "status") == 0) {
        Serial.printf("file=%s  loop=%s  speed=%.2f  playing=%s  ack=%s\n",
                      gFile[0] ? gFile : "(none)",
                      gLooping ? "on" : "off",
                      gSpeed,
                      gPlaying ? "yes" : "no",
                      gRequireAck ? "required" : "not required");
        twai_status_info_t s;
        if (twai_get_status_info(&s) == ESP_OK) {
            const char* states[] = {"STOPPED","RUNNING","BUS_OFF","RECOVERING"};
            Serial.printf("CAN state=%s  tx_err=%lu  rx_err=%lu  tx_failed=%lu  rx_missed=%lu\n",
                          s.state < 4 ? states[s.state] : "?",
                          s.tx_error_counter, s.rx_error_counter,
                          s.tx_failed_count, s.rx_missed_count);
        }

    } else if (strcmp(cmd, "txtest") == 0) {
        // Drive TXD low by hand with the TWAI driver off and read RXD back: a working
        // transceiver echoes TXD (low = dominant). Sample only briefly after each edge,
        // well inside the TJA1051's TXD dominant time-out: that turns the driver off
        // once TXD has been low for a few ms, so a slow or static check reads
        // recessive even on a good chip.
        gStopRequest = true;
        gPlaying     = false;
        twai_stop();
        twai_driver_uninstall();
        pinMode(kCanRx, INPUT);
        pinMode(kCanTx, OUTPUT);
        digitalWrite(kCanTx, HIGH);
        delay(2);
        int highLows = 0, lowLows = 0, firstLowUs = -1;
        for (int i = 0; i < 200; ++i) highLows += gpio_get_level(kCanRx) == 0;
        for (int rep = 0; rep < 20; ++rep) {
            int64_t t0 = esp_timer_get_time();
            gpio_set_level(kCanTx, 0);
            for (int i = 0; i < 50; ++i) {
                bool low = gpio_get_level(kCanRx) == 0;
                lowLows += low;
                if (low && firstLowUs < 0) firstLowUs = (int)(esp_timer_get_time() - t0);
            }
            gpio_set_level(kCanTx, 1);
            delayMicroseconds(200);
        }
        Serial.printf("txtest: TXD HIGH -> RXD low %d/200 (want 0)\n", highLows);
        Serial.printf("txtest: TXD LOW  -> RXD low %d/1000 (want ~1000), first low after %d us\n",
                      lowLows, firstLowUs);
        canRestart();

    } else if (strcmp(cmd, "txsquare") == 0) {
        // Square wave on TXD: 100 us low, 100 us high, well under the TJA1051's
        // dominant time-out, until any byte arrives on the console. A DC meter then
        // reads averages: TXD ~1.65 V; on a driving chip CANH ~3 V, CANL ~2 V and
        // RXD about half its high level; a chip that isn't driving stays at 2.5/2.5.
        gStopRequest = true;
        gPlaying     = false;
        twai_stop();
        twai_driver_uninstall();
        pinMode(kCanRx, INPUT);
        pinMode(kCanTx, OUTPUT);
        Serial.println("txsquare: running, send any key to stop");
        uint32_t lows = 0, samples = 0;
        while (!Serial.available()) {
            for (int k = 0; k < 2500; ++k) {
                gpio_set_level(kCanTx, 0);
                delayMicroseconds(50);
                lows += gpio_get_level(kCanRx) == 0;
                ++samples;
                delayMicroseconds(50);
                gpio_set_level(kCanTx, 1);
                delayMicroseconds(100);
            }
        }
        while (Serial.available()) Serial.read();
        gpio_set_level(kCanTx, 1);
        Serial.printf("txsquare: stopped, RXD low on %lu/%lu mid-pulse samples\n",
                      (unsigned long)lows, (unsigned long)samples);
        canRestart();

    } else if (strcmp(cmd, "txlow") == 0) {
        // Hold TXD low. NOT a valid check of the bus: the TJA1051's TXD dominant
        // time-out turns its driver off after a few ms, so a meter then reads
        // recessive even on a good chip. Only TXD and RXD at the pins mean anything.
        // "txrelease" restarts CAN.
        gStopRequest = true;
        gPlaying     = false;
        twai_stop();
        twai_driver_uninstall();
        pinMode(kCanRx, INPUT);
        pinMode(kCanTx, OUTPUT);
        digitalWrite(kCanTx, LOW);
        Serial.printf("txlow: TXD held LOW, RXD reads %s. 'txrelease' to restart CAN\n",
                      digitalRead(kCanRx) ? "HIGH" : "LOW");

    } else if (strcmp(cmd, "txrelease") == 0) {
        digitalWrite(kCanTx, HIGH);
        canRestart();

    } else if (strcmp(cmd, "help") == 0) {
        Serial.println("  ls              list CSV files on SD");
        Serial.println("  play <file>     start playback");
        Serial.println("  stop            stop playback");
        Serial.println("  loop            toggle loop mode");
        Serial.println("  speed <n>       playback speed multiplier");
        Serial.println("  maxgap <ms>     cap inter-frame gaps (default 1000)");
        Serial.println("  ack on|off      require acks like a real ECU (default off)");
        Serial.println("  status          show current config");
        Serial.println("  txtest          stop, drive TXD by hand, check RXD echoes it");
        Serial.println("  txsquare        5 kHz square wave on TXD for a meter; any key stops");
        Serial.println("  txlow           hold TXD low; txrelease restarts CAN");

    } else if (strlen(cmd) > 0) {
        Serial.printf("Unknown: '%s'  (type 'help')\n", cmd);
    }
}

void handleConsole() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            gCmdBuf[gCmdLen] = '\0';
            if (gCmdLen > 0) processCommand(gCmdBuf);
            gCmdLen = 0;
        } else if (gCmdLen < (int)sizeof(gCmdBuf) - 1) {
            gCmdBuf[gCmdLen++] = c;
        }
    }
}

// ── Arduino ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);

    Serial.println("\nCAN Simulator — XIAO ESP32S3");
    Serial.println("─────────────────────────────");

    sdSpi.begin(kSdSck, kSdMiso, kSdMosi);
    if (!SD.begin(kSdCs, sdSpi)) {
        Serial.println("SD: init failed — check wiring and CS pin");
    } else {
        Serial.printf("SD: ready  (%llu MB)\n",
                      (unsigned long long)SD.totalBytes() >> 20);
        listCsvFiles();
    }

    if (!canStart()) {
        Serial.println("CAN: init failed");
    } else {
        Serial.printf("CAN: ready @ 500 kbps (%s)\n", gRequireAck ? "NORMAL, acks required" : "NO_ACK");
    }

    Serial.println();

    if (firstCsv(gFile, sizeof(gFile))) {
        Serial.printf("Auto-play: %s\n", gFile);
        gPlaying = true;
    } else {
        Serial.println("No CSV found. Put a SavvyCAN log on the SD and type 'play <file>'.");
    }
}

void loop() {
    handleConsole();

    if (gPlaying && gFile[0]) {
        gStopRequest = false;
        playFile(gFile);
        if (gStopRequest || !gLooping) {
            gPlaying = false;
            if (!gStopRequest) Serial.println("Loop off — playback finished.");
        }
    } else {
        delay(10);
    }
}
