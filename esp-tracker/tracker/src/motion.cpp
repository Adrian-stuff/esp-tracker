#include "motion.h"
#include "modem.h"
#include "../include/config.h"
#include <WiFi.h>
#include <string.h>

namespace motion {

static MotionState s_state      = MotionState::Unknown;
static uint32_t    s_changedAt  = 0;
static uint32_t    s_lastScan   = 0;
static uint32_t    s_lastCell   = 0;
static bool        s_blind      = false;

// Last seen BSSID set, stored as truncated hashes so a scan costs ~40 bytes
// of RAM rather than a list of strings.
static uint32_t s_prev[MOTION_MAX_APS];
static uint8_t  s_prevN = 0;

static uint32_t s_prevCellId = 0;
static int8_t   s_prevCsq    = 0;

// Raw scan data for WiFi reporting — BSSID, RSSI, SSID for each visible AP.
static constexpr uint8_t MAX_SCAN_APS = 8;
static ScanAp  s_scanAp[MAX_SCAN_APS];
static uint8_t s_scanApN = 0;

static uint32_t hashBssid(const uint8_t* mac) {
    uint32_t h = 2166136261u;                    // FNV-1a
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

static void setState(MotionState next) {
    if (next != s_state) { s_state = next; s_changedAt = millis(); }
}

// TIER 0 — free. The modem is already attached; asking it what cell it is on
// costs one AT command and no extra radio time.
static void tierCell() {
    uint16_t mcc, mnc, lac; uint32_t cid; int8_t rssi;
    if (!modem::cellInfo(mcc, mnc, lac, cid, rssi)) return;

    if (s_prevCellId && cid != s_prevCellId) {
        // Changed serving cell: definitely moved, and by a meaningful distance.
        setState(MotionState::Moving);
        s_lastScan = 0;                          // force a Tier 1 scan now
    }
    s_prevCellId = cid;
    s_prevCsq    = rssi;
}

// TIER 1 — the authority. ~2 s of radio, ~80 mA, so roughly 0.05 mAh a scan.
static void tierWifi() {
    int n = WiFi.scanNetworks(false /* sync */, false /* hidden */);
    if (n < 0) return;

    uint32_t cur[MOTION_MAX_APS];
    uint8_t  curN = 0;
    s_scanApN = 0;
    for (int i = 0; i < n && curN < MOTION_MAX_APS; i++) {
        cur[curN++] = hashBssid(WiFi.BSSID(i));
        // Capture raw data for WiFi reporting (up to MAX_SCAN_APS)
        if (s_scanApN < MAX_SCAN_APS) {
            const uint8_t* mac = WiFi.BSSID(i);
            memcpy(s_scanAp[s_scanApN].bssid, mac, 6);
            s_scanAp[s_scanApN].rssi = WiFi.RSSI(i);
            const char* ssid = WiFi.SSID(i).c_str();
            strncpy(s_scanAp[s_scanApN].ssid, ssid, sizeof(s_scanAp[0].ssid) - 1);
            s_scanAp[s_scanApN].ssid[sizeof(s_scanAp[0].ssid) - 1] = '\0';
            s_scanApN++;
        }
    }
    WiFi.scanDelete();

    // No APs at all and no cell movement: we genuinely cannot tell. Say so
    // rather than reporting a confident "stationary".
    if (curN == 0 && s_prevN == 0) {
        s_blind = true;
        return;
    }
    s_blind = false;

    if (s_prevN == 0) {                          // first scan, nothing to compare
        memcpy(s_prev, cur, curN * sizeof(uint32_t));
        s_prevN = curN;
        return;
    }

    // Jaccard similarity of the two BSSID sets.
    uint8_t inter = 0;
    for (uint8_t i = 0; i < curN; i++)
        for (uint8_t j = 0; j < s_prevN; j++)
            if (cur[i] == s_prev[j]) { inter++; break; }
    uint8_t uni = curN + s_prevN - inter;
    float   sim = uni ? (float)inter / (float)uni : 0.0f;

    setState(sim >= MOTION_SIMILAR_THRESHOLD ? MotionState::Stationary
                                             : MotionState::Moving);

    memcpy(s_prev, cur, curN * sizeof(uint32_t));
    s_prevN = curN;
}

void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();                           // scanning only, never associating
    s_changedAt = millis();
}

void service() {
    uint32_t now = millis();

    if (now - s_lastCell >= MOTION_CELL_CHECK_MS) {
        s_lastCell = now;
        tierCell();
    }

    // Scan less often once we are confident nothing is happening.
    uint32_t interval = (s_state == MotionState::Stationary)
                        ? MOTION_SCAN_STATIONARY_MS : MOTION_SCAN_MOVING_MS;
    if (now - s_lastScan >= interval) {
        s_lastScan = now;
        tierWifi();
    }
}

MotionState state()      { return s_state; }
uint32_t    stableFor()  { return millis() - s_changedAt; }
bool        blind()      { return s_blind; }
const ScanAp* lastScan() { return s_scanAp; }
uint8_t     lastScanCount() { return s_scanApN; }

}
