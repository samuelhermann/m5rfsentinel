// RF Sentinel EU: minimal standalone build for M5StickC / M5StickC Plus.
//
// Vibe coded with an AI assistant (Claude Code). Adapted from RF Sentinel (CIS-C0, GPL-3.0) and
// OUI-SPY Unified (colonelpanichacks, MIT); see README.md for credits.
//
// Receive-only. Listens to BLE adverts and WiFi frames (promiscuous mode, channels 1-13)
// and flags any device whose address starts with a prefix from the European watchlist
// (oui_eu.h, generated from the app's UK/FR/DE/IT/NL/PT/ES/SE presets).
//
//   Button A (front)  click: dismiss the current alert for 60 s; when idle, step through the last
//                     8 devices heard (address, signal, age); past the last one, back to idle
//                     hold: scan mode, both -> BLE only -> WiFi only
//   Button B (side)   mute / unmute the buzzer
//   Button PWR (left) stealth: screen off, LED and buzzer still alert; click again to wake
//   Serial 115200     prints every hit; send 't' to fire a test alert
#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "oui_eu.h"

static const uint32_t HOLD_MS = 8000;        // alert stays up this long after the last sighting
static const uint32_t COOLDOWN_MS = 60000;   // same device doesn't re-strobe / re-beep inside this
static const uint32_t BLE_MS = 1500;         // BLE and WiFi share one radio: BLE for this long,
static const uint32_t DWELL_MS = 110;        // then WiFi for 13 channels x this, and round again
static const uint8_t STRONG_SCORE = 70;      // app "probable" and up strobes for longer
static const uint8_t ALERT_MIN_SCORE = 40;   // weaker matches (module makers, Parrot) only go in the hit list

struct Hit {
  uint8_t mac[6];  // most significant byte first
  const OuiEntry* e;
  int8_t rssi;
  bool ble;
};

static QueueHandle_t hitQ;
static volatile uint32_t nBle, nWifi;
static NimBLEScan* bleScan;

enum Scan : uint8_t { SCAN_BOTH, SCAN_BLE, SCAN_WIFI };
static Scan scanMode = SCAN_BOTH;
static bool stealth = false;  // screen off; LED and buzzer still alert

// ---------------------------------------------------------------- matching

static const OuiEntry* lookup(const uint8_t m[6]) {
  uint64_t mac = 0;
  for (int i = 0; i < 6; i++) mac = (mac << 8) | m[i];
  const OuiEntry* best = nullptr;
  for (int i = 0; i < OUI_EU_COUNT; i++) {
    const OuiEntry& e = OUI_EU[i];
    if ((mac >> (4 * (12 - e.nibbles))) == e.val && (!best || e.nibbles > best->nibbles)) best = &e;
  }
  return best;
}

static void report(const uint8_t m[6], int rssi, bool ble) {
  const OuiEntry* e = lookup(m);
  if (!e) return;
  Hit h;
  memcpy(h.mac, m, 6);
  h.e = e;
  h.rssi = (int8_t)rssi;
  h.ble = ble;
  xQueueSend(hitQ, &h, 0);  // full queue: drop, the device will be heard again
}

static void onWifi(void* buf, wifi_promiscuous_pkt_type_t) {
  auto* p = (wifi_promiscuous_pkt_t*)buf;
  if (p->rx_ctrl.sig_len < 24) return;
  nWifi++;
  const uint8_t* tx = p->payload + 10;  // transmitter address
  if (tx[0] & 0x02) return;             // locally administered = randomized, carries no maker
  report(tx, p->rx_ctrl.rssi, false);
}

class BleCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    nBle++;
    const NimBLEAddress a = d->getAddress();
    if (a.getType() != BLE_ADDR_PUBLIC) return;  // random addresses carry no maker either
    const uint8_t* v = a.getVal();               // least significant byte first
    const uint8_t m[6] = {v[5], v[4], v[3], v[2], v[1], v[0]};
    report(m, d->getRSSI(), true);
  }
};
static BleCb bleCb;

// ---------------------------------------------------------------- display

static bool muted = false;
static int W, H;
static float K;  // layout is drawn for 160x80 and scaled to the screen (Plus is 240x135)

static const char* catLabel(Cat c) {
  return c == CAT_BODYCAM ? "BODY CAM" : c == CAT_ALPR ? "PLATE CAM" : c == CAT_CAMERA ? "DOORBELL" : c == CAT_DRONE ? "DRONE" : "POLICE";
}

static uint16_t catColor(Cat c) {
  auto& d = M5.Display;
  return c == CAT_BODYCAM ? d.color565(255, 0, 0)
       : c == CAT_ALPR    ? d.color565(255, 140, 0)
       : c == CAT_CAMERA  ? d.color565(150, 50, 255)
       : c == CAT_DRONE   ? d.color565(0, 170, 150)
                          : d.color565(0, 70, 255);
}

static void macStr(const uint8_t* m, char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void fmtCount(uint32_t n, char* out) {
  if (n < 1000) sprintf(out, "%u", (unsigned)n);
  else sprintf(out, "%.1fk", n / 1000.0f);
}

// One line of text, centered, opaque so it overwrites the previous one without clearing the screen.
static void line(const char* s, int y, const lgfx::IFont* big, const lgfx::IFont* small_, uint16_t fg, uint16_t bg) {
  auto& d = M5.Display;
  d.setTextSize(K);
  d.setFont(big);
  if (d.textWidth(s) > W - 6) d.setFont(small_);
  d.setTextColor(fg, bg);
  d.setTextDatum(top_center);
  d.setTextPadding(W);
  d.drawString(s, W / 2, (int)(y * K));
}

// ---------------------------------------------------------------- hit list

struct Seen {
  uint8_t mac[6];
  const OuiEntry* e;
  int8_t rssi;
  bool ble;
  uint32_t t;  // last heard
  uint16_t n;  // times heard
};
static Seen seen[8];  // newest first
static int nSeen;

// Returns true when the device is new to the list.
static bool remember(const Hit& h, uint32_t now) {
  int i = 0;
  while (i < nSeen && memcmp(seen[i].mac, h.mac, 6)) i++;
  bool isNew = i == nSeen;
  uint16_t n = isNew ? 0 : seen[i].n;
  if (n < 65535) n++;
  if (isNew) {
    if (nSeen < 8) {
      i = nSeen++;
    } else {  // full: the oldest weak entry goes first, and a weak newcomer never pushes out a strong one
      int v = -1;
      for (int j = 7; j >= 0 && v < 0; j--)
        if (seen[j].e->score < ALERT_MIN_SCORE) v = j;
      if (v < 0) {
        if (h.e->score < ALERT_MIN_SCORE) return false;
        v = 7;
      }
      i = v;
    }
  }
  memmove(&seen[1], &seen[0], i * sizeof(Seen));  // move the device to the front
  memcpy(seen[0].mac, h.mac, 6);
  seen[0].e = h.e;
  seen[0].rssi = h.rssi;
  seen[0].ble = h.ble;
  seen[0].t = now;
  seen[0].n = n;
  return isNew;
}

static void drawSeen(int idx, bool full, uint32_t now) {
  auto& d = M5.Display;
  uint16_t bg = TFT_BLACK;
  uint16_t grey = d.color565(170, 170, 170);
  d.startWrite();
  if (full) d.fillScreen(bg);
  if (!nSeen) {
    line("NO HITS YET", 30, &fonts::Font2, &fonts::Font0, grey, bg);
    d.endWrite();
    return;
  }
  const Seen& e = seen[idx];
  char s[40], m[18], age[8];
  uint32_t sec = (now - e.t) / 1000;
  if (sec < 60) sprintf(age, "%us", (unsigned)sec);
  else if (sec < 3600) sprintf(age, "%um", (unsigned)(sec / 60));
  else sprintf(age, "%uh", (unsigned)(sec / 3600));
  sprintf(s, "HIT %d/%d  %s%s", idx + 1, nSeen, e.ble ? "BLE" : "WIFI", e.e->score < ALERT_MIN_SCORE ? "  weak" : "");
  line(s, 2, &fonts::Font0, &fonts::Font0, d.color565(0, 190, 230), bg);
  line(catLabel(e.e->cat), 13, &fonts::Font2, &fonts::Font0, catColor(e.e->cat), bg);
  line(e.e->name, 31, &fonts::Font2, &fonts::Font0, TFT_WHITE, bg);
  macStr(e.mac, m);
  line(m, 51, &fonts::Font0, &fonts::Font0, TFT_WHITE, bg);
  sprintf(s, "%d dBm  x%u  %s ago", e.rssi, (unsigned)e.n, age);
  line(s, 63, &fonts::Font0, &fonts::Font0, grey, bg);
  d.endWrite();
}

static void drawIdle(bool full, uint32_t now) {
  auto& d = M5.Display;
  uint16_t bg = TFT_BLACK;
  d.startWrite();
  if (full) d.fillScreen(bg);

  // title and battery
  d.setFont(&fonts::Font0);
  d.setTextSize(K);
  d.setTextPadding(0);
  d.setTextDatum(top_left);
  d.setTextColor(d.color565(0, 190, 230), bg);
  char title[24] = "RF SENTINEL EU";
  if (muted || scanMode != SCAN_BOTH)
    sprintf(title, "RFS EU%s%s", muted ? " MUTED" : "", scanMode == SCAN_BLE ? " BLE" : scanMode == SCAN_WIFI ? " WIFI" : "");
  d.drawString(title, 3, (int)(3 * K));
  static int bat;
  static uint32_t batAt;
  if (!batAt || now - batAt > 5000) { bat = M5.Power.getBatteryLevel(); batAt = now | 1; }
  char b[8];
  sprintf(b, "%d%%", bat < 0 ? 0 : bat);
  d.setTextDatum(top_right);
  d.setTextPadding((int)(30 * K));
  d.setTextColor(bat > 40 ? TFT_GREEN : bat > 15 ? TFT_YELLOW : TFT_RED, bg);
  d.drawString(b, W - 3, (int)(3 * K));

  // breathing "SCANNING"
  float breath = 0.5f + 0.5f * sinf(now / 450.0f);
  line("SCANNING", 24, &fonts::Font4, &fonts::Font2, d.color565(0, 110 + 145 * breath, 60 + 70 * breath), bg);

  // counters
  char a[8], c[8], s[40];
  fmtCount(nBle, a);
  fmtCount(nWifi, c);
  sprintf(s, "BLE %s  WiFi %s", a, c);
  line(s, 58, &fonts::Font2, &fonts::Font0, d.color565(170, 170, 170), bg);
  d.endWrite();
}

static void drawAlert(const Hit& h, bool full, bool inv) {
  auto& d = M5.Display;
  uint16_t base = catColor(h.e->cat);
  uint16_t bg = inv ? TFT_BLACK : base;
  uint16_t fg = inv ? base : (h.e->cat == CAT_ALPR ? TFT_BLACK : TFT_WHITE);
  d.startWrite();
  if (full) {
    d.fillScreen(bg);
    d.setFont(&fonts::Font2);
    d.setTextSize(K);
    d.setTextPadding(0);
    d.setTextColor(fg, bg);
    d.setTextDatum(top_left);
    d.drawString(h.e->score >= STRONG_SCORE ? "ALERT" : "POSSIBLE", 4, (int)(2 * K));
    d.setTextDatum(top_right);
    d.drawString(h.ble ? "BLE" : "WIFI", W - 4, (int)(2 * K));
    line(catLabel(h.e->cat), 20, &fonts::Font4, &fonts::Font2, fg, bg);
    line(h.e->name, 48, &fonts::Font2, &fonts::Font0, fg, bg);
  }
  // MAC, signal and a strength bar: the part that keeps changing
  char m[18], s[40];
  macStr(h.mac, m);
  sprintf(s, "%s  %d", m, h.rssi);
  line(s, 66, &fonts::Font0, &fonts::Font0, fg, bg);
  int barMax = W - 8;
  int barW = barMax * constrain(h.rssi + 100, 0, 60) / 60;
  int barY = (int)(76 * K), barH = (int)(3 * K) + 1;
  d.fillRect(4, barY, barMax, barH, bg);
  d.fillRect(4, barY, barW, barH, fg);
  d.endWrite();
}

// ---------------------------------------------------------------- state

static bool active = false;
static Hit cur;
static uint32_t curSeen, alertStart;
static uint32_t hitCount;
static bool lastInv, lastStrobing;  // strobe phase last drawn
static int listIdx = -1;            // hit-list page on screen, -1 = idle screen
static uint32_t listAt;             // last button press on the list

static struct { uint8_t mac[6]; uint32_t t; } recent[8];
static int recentNext;
static uint8_t dismissMac[6];
static uint32_t dismissUntil;
static bool dismissed;

// True when this device hasn't raised an alert lately; remembers it.
static bool fresh(const uint8_t* mac, uint32_t now) {
  for (auto& r : recent)
    if (r.t && !memcmp(r.mac, mac, 6) && now - r.t < COOLDOWN_MS) { r.t = now; return false; }
  memcpy(recent[recentNext].mac, mac, 6);
  recent[recentNext].t = now ? now : 1;
  recentNext = (recentNext + 1) % 8;
  return true;
}

static void beep() {
  if (!muted && M5.Speaker.isEnabled()) M5.Speaker.tone(2800, 120);
}

static void setAlertLook(bool on) {
  M5.Display.setBrightness(stealth ? 0 : on ? 255 : 230);  // idle was 150 and read too dim on the StickC; full blast on a hit
  M5.Power.setLed(on ? 255 : 0);            // the red LED on the StickC, where there is one
}

static void injectTest() {
  static uint8_t n;
  Hit h;
  const OuiEntry* pick[5] = {};  // one per category, strongest first
  for (int i = OUI_EU_COUNT - 1; i >= 0; i--) pick[OUI_EU[i].cat] = &OUI_EU[i];
  h.e = pick[n % 5];
  uint64_t v = h.e->val << (4 * (12 - h.e->nibbles));
  for (int i = 5; i >= 0; i--, v >>= 8) h.mac[i] = v & 0xFF;
  h.mac[5] = ++n;  // a different device each time, so the cooldown doesn't swallow it
  h.rssi = -45;
  h.ble = n & 1;
  xQueueSend(hitQ, &h, 0);
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  if (M5.Display.width() < M5.Display.height()) M5.Display.setRotation(3);
  W = M5.Display.width();
  H = M5.Display.height();
  K = W / 160.0f;
  Serial.printf("\nRF Sentinel EU - %d prefixes, screen %dx%d\n", OUI_EU_COUNT, W, H);

  hitQ = xQueueCreate(16, sizeof(Hit));

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_country_t country = {"DE", 1, 13, 20, WIFI_COUNTRY_POLICY_MANUAL};  // Europe: channels 1-13
  esp_wifi_set_country(&country);
  wifi_promiscuous_filter_t filter = {WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(onWifi);
  esp_wifi_set_promiscuous(true);

  NimBLEDevice::init("");
  bleScan = NimBLEDevice::getScan();
  bleScan->setScanCallbacks(&bleCb, true);  // true: also repeat adverts, so signal strength stays live
  bleScan->setActiveScan(false);
  bleScan->setInterval(100);
  bleScan->setWindow(99);
  bleScan->start(0, false);  // runs until the loop hands the radio to WiFi

  setAlertLook(false);
  drawIdle(true, millis());
}

void loop() {
  static uint32_t phaseAt, lastDraw, lastStat;
  static bool bleOn = true;
  static uint8_t ch = 1;
  static bool lastActive;
  static bool redrawFull = true;
  uint32_t now = millis();
  M5.update();

  if (scanMode == SCAN_BLE) {
    if (!bleOn) {
      bleScan->start(0, false);
      bleOn = true;
    }
  } else if (scanMode == SCAN_WIFI) {
    if (bleOn) {
      bleScan->stop();
      bleOn = false;
      phaseAt = now;
    }
  } else if (bleOn && now - phaseAt >= BLE_MS) {
    bleScan->stop();
    bleOn = false;
    phaseAt = now;
  } else if (!bleOn && now - phaseAt >= 13 * DWELL_MS) {
    bleScan->start(0, false);
    bleOn = true;
    phaseAt = now;
  }
  if (!bleOn) {
    uint8_t want = (now - phaseAt) / DWELL_MS % 13 + 1;
    if (want != ch) {
      ch = want;
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
  }

  if (Serial.available() && Serial.read() == 't') injectTest();

  // new sightings
  Hit h;
  while (xQueueReceive(hitQ, &h, 0) == pdTRUE) {
    char m[18];
    macStr(h.mac, m);
    bool newToList = remember(h, now);
    bool weak = h.e->score < ALERT_MIN_SCORE;
    if (!weak || newToList)  // weak chip makers are everywhere: log each once
      Serial.printf("HIT %s %s rssi %d  %s / %s (%d)%s\n", h.ble ? "BLE " : "WIFI", m, h.rssi, catLabel(h.e->cat),
                    h.e->name, h.e->score, weak ? " weak, no alert" : "");
    if (weak) continue;
    if (dismissed && !memcmp(h.mac, dismissMac, 6) && now < dismissUntil) continue;
    bool same = active && !memcmp(h.mac, cur.mac, 6);
    if (active && !same && h.e->score < cur.e->score) continue;  // keep the stronger one up
    bool isNew = !same && fresh(h.mac, now);
    cur = h;
    curSeen = now;
    if (isNew) {
      alertStart = now;
      hitCount++;
      lastStrobing = false;  // so the first frame of a new alert beeps
    } else if (!active) {
      alertStart = now - 60000;  // seen again recently: show it, no new strobe
    }
    if (!same) redrawFull = true;
    if (!active) {
      active = true;
      listIdx = -1;
      setAlertLook(true);
    }
  }

  // buttons
  if (M5.BtnA.wasClicked()) {
    if (active) {
      memcpy(dismissMac, cur.mac, 6);
      dismissUntil = now + COOLDOWN_MS;
      dismissed = true;
      active = false;
    } else if (!stealth) {
      int next = listIdx + 1;
      listIdx = next >= (nSeen ? nSeen : 1) ? -1 : next;  // step through the list, then back to idle
      listAt = now;
      redrawFull = true;
    }
  }
  if (M5.BtnA.wasHold()) {
    scanMode = (Scan)((scanMode + 1) % 3);
    esp_wifi_set_promiscuous(scanMode != SCAN_BLE);  // BLE only: WiFi sniffing off, BLE gets the whole radio
    phaseAt = now;
    beep();
    Serial.printf("scan: %s\n", scanMode == SCAN_BLE ? "BLE only" : scanMode == SCAN_WIFI ? "WiFi only" : "BLE + WiFi");
    if (!active) redrawFull = true;
  }
  if (M5.BtnB.wasPressed()) {
    muted = !muted;
    if (!active) redrawFull = true;
  }
  if (M5.BtnPWR.wasClicked()) {
    stealth = !stealth;
    setAlertLook(active);
    Serial.printf("stealth: %s\n", stealth ? "on (screen off)" : "off");
    if (!stealth) redrawFull = true;
  }
  if (listIdx >= 0 && now - listAt > 15000) {
    listIdx = -1;
    redrawFull = true;
  }
  if (active && now - curSeen > HOLD_MS) active = false;

  if (active) {
    uint32_t age = now - alertStart;
    bool strobing = age < (cur.e->score >= STRONG_SCORE ? 3000u : 1200u);
    bool inv = strobing && ((age / 170) & 1);
    bool strong = cur.e->score >= STRONG_SCORE;
    if (strobing && !inv && (!lastStrobing || lastInv) && age < (strong ? 1000u : 340u)) beep();  // 3 beeps / 1
    if (redrawFull || inv != lastInv || strobing != lastStrobing || !lastActive) {
      drawAlert(cur, true, inv);
      redrawFull = false;
      lastDraw = now;
    } else if (!strobing && now - lastDraw > 400) {
      drawAlert(cur, false, false);
      lastDraw = now;
    }
    lastInv = inv;
    lastStrobing = strobing;
  } else {
    if (lastActive) {
      setAlertLook(false);
      redrawFull = true;
      lastInv = lastStrobing = false;
    }
    if (listIdx >= 0) {
      if (redrawFull || now - lastDraw > 500) {
        drawSeen(listIdx, redrawFull, now);
        redrawFull = false;
        lastDraw = now;
      }
    } else if (redrawFull || now - lastDraw > 120) {
      drawIdle(redrawFull, now);
      redrawFull = false;
      lastDraw = now;
    }
  }
  lastActive = active;

  if (now - lastStat > 5000) {
    lastStat = now;
    Serial.printf("[stat] ble=%u wifi=%u ch=%u hits=%u heap=%u\n", (unsigned)nBle, (unsigned)nWifi, ch,
                  (unsigned)hitCount, (unsigned)ESP.getFreeHeap());
  }
  delay(5);
}
