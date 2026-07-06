#include "ui.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "totp.h"
#include "vault.h"
#include "notify.h"

namespace solari {

static Ui g_ui;
Ui& ui() { return g_ui; }

// ---- clipboard buffer (see ui.h) -------------------------------------------
static String s_clip;
static uint32_t s_clipClearAt = 0;
void ui_setClipboard(const String& text, uint32_t clearAfterMs) {
  s_clip = text;
  s_clipClearAt = millis() + clearAfterMs;
  g_ui.toast("Copied — clears in " + String(clearAfterMs / 1000) + "s",
             TFT_GREEN);
}

// ---- layout constants ------------------------------------------------------
namespace {
constexpr int kW = 1280, kH = 720;
constexpr int kStatusH = 44;
// three home tiles across the middle band
struct Rect { int x, y, w, h; };
const Rect kTileTotp{80, 160, 340, 400};
const Rect kTileVault{470, 160, 340, 400};
const Rect kTileAlerts{860, 160, 340, 400};

uint16_t sevColor(Severity s) {
  switch (s) {
    case Severity::Crit: return TFT_RED;
    case Severity::Warn: return TFT_ORANGE;
    default: return TFT_DARKGREY;
  }
}
}  // namespace

void Ui::begin() {
  M5.Display.setRotation(1);           // landscape
  M5.Display.setBrightness(180);
  screen_ = config().settings().provisioned ? Screen::Home : Screen::Setup;
  dirty_ = true;
}

// ---- main loop -------------------------------------------------------------
void Ui::loop() {
  M5.update();  // refresh touch/button state

  // Touch handling (single-touch is enough for these screens).
  auto t = M5.Touch.getDetail();
  if (t.wasClicked()) handleTouch_(t.x, t.y);

  // Clipboard auto-clear.
  if (s_clipClearAt && millis() > s_clipClearAt) {
    s_clip = "";
    s_clipClearAt = 0;
  }

  // TOTP screen animates every frame (countdown ring); others redraw on dirty.
  bool tick = (millis() - lastTickMs_) > 250;
  if (screen_ == Screen::Totp && tick) dirty_ = true;
  if (toastUntilMs_ && millis() < toastUntilMs_) { /* keep redrawing toast */ }

  if (!dirty_ && !tick) return;
  lastTickMs_ = millis();

  M5.Display.startWrite();
  switch (screen_) {
    case Screen::Setup: drawSetup_(); break;
    case Screen::Home: drawHome_(); break;
    case Screen::Totp: drawTotp_(); break;
    case Screen::VaultUnlock: drawVaultUnlock_(); break;
    case Screen::Vault: drawVault_(); break;
    case Screen::ItemDetail: drawItemDetail_(); break;
    case Screen::Alerts: drawAlerts_(); break;
    default: drawHome_(); break;
  }
  drawStatusBar_();
  if (toastUntilMs_ && millis() < toastUntilMs_) drawToast_();
  M5.Display.endWrite();
  dirty_ = false;
}

void Ui::goTo(Screen s) { screen_ = s; dirty_ = true; }

void Ui::toast(const String& text, uint16_t color, uint32_t ms) {
  toastText_ = text;
  toastColor_ = color;
  toastUntilMs_ = millis() + ms;
  dirty_ = true;
}

// ---- status bar ------------------------------------------------------------
void Ui::drawStatusBar_() {
  auto& d = M5.Display;
  d.fillRect(0, 0, kW, kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(middle_left);
  d.setTextSize(2);

  // clock (from RTC/SNTP-disciplined system time)
  char clk[16] = "--:--";
  time_t now = time(nullptr);
  if (totp::timeIsValid()) {
    struct tm tmv; localtime_r(&now, &tmv);
    strftime(clk, sizeof(clk), "%H:%M", &tmv);
  }
  d.drawString(clk, 16, kStatusH / 2);

  // right-aligned status glyphs: wifi, mqtt, vault lock, unread count
  d.setTextDatum(middle_right);
  String right;
  right += (WiFi.status() == WL_CONNECTED) ? "WiFi " : "no-wifi ";
  right += notify().connected() ? "MQTT " : "mqtt? ";
  right += vault().isUnlocked() ? "unlocked " : "locked ";
  if (notify().unreadCount()) right += "(" + String(notify().unreadCount()) + ") ";
  d.drawString(right, kW - 16, kStatusH / 2);
}

// ---- home ------------------------------------------------------------------
static void tile(const char* title, const char* subtitle, const Rect& r,
                 uint16_t accent) {
  auto& d = M5.Display;
  d.fillRoundRect(r.x, r.y, r.w, r.h, 24, TFT_DARKGREY);
  d.fillRoundRect(r.x, r.y, r.w, 12, 24, accent);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(4);
  d.drawString(title, r.x + 28, r.y + 48);
  d.setTextSize(2);
  d.setTextColor(TFT_LIGHTGREY);
  d.drawString(subtitle, r.x + 28, r.y + 120);
}

void Ui::drawHome_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("SolariNet Companion", 80, 90);

  auto accounts = config().loadTotpAccounts();
  tile("TOTP", (String(accounts.size()) + " codes").c_str(), kTileTotp, TFT_CYAN);
  tile("Vault", vault().isUnlocked() ? "unlocked" : "locked", kTileVault,
       TFT_GREEN);
  tile("Alerts", (String(notify().unreadCount()) + " unread").c_str(),
       kTileAlerts, TFT_ORANGE);
}

// ---- TOTP ------------------------------------------------------------------
static void drawRing(int cx, int cy, int r, float frac, uint16_t color) {
  // frac in [0,1] of time remaining. Draw as an arc; M5GFX has drawArc.
  M5.Display.drawArc(cx, cy, r, r - 8, 0, (int)(360 * frac), color);
}

void Ui::drawTotp_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("TOTP", 40, 60);
  // "+ Add" button (top-right) -> QR scan or manual entry (see handleTouch_)
  d.setTextDatum(top_right);
  d.drawString("[ + Add ]", kW - 40, 60);
  d.setTextDatum(top_left);

  time_t now = time(nullptr);
  auto accounts = config().loadTotpAccounts();
  int y = 130;
  for (auto& a : accounts) {
    totp::Code code;
    String shown = totp::compute(a, now, code) ? code.digits : String("------");
    // group as "123 456" for readability
    if (shown.length() == 6) shown = shown.substring(0, 3) + " " + shown.substring(3);

    d.setTextSize(2);
    d.setTextColor(TFT_LIGHTGREY);
    d.drawString((a.issuer + "  " + a.label).c_str(), 60, y);
    d.setTextSize(5);
    d.setTextColor(TFT_CYAN);
    d.drawString(shown.c_str(), 60, y + 30);

    float frac = code.period ? (float)code.secondsLeft / code.period : 0;
    uint16_t ringc = code.secondsLeft <= 5 ? TFT_RED : TFT_GREEN;
    drawRing(kW - 120, y + 60, 44, frac, ringc);
    d.setTextSize(2);
    d.setTextDatum(middle_center);
    d.drawString(String(code.secondsLeft).c_str(), kW - 120, y + 60);
    d.setTextDatum(top_left);

    y += 130;
    if (y > kH - 120) break;  // TODO: scroll for long lists
  }
  if (accounts.empty()) {
    d.setTextSize(2);
    d.setTextColor(TFT_LIGHTGREY);
    d.drawString("No accounts yet. Tap [ + Add ] to scan a QR or enter a seed.",
                 60, 140);
  }
  // TODO(add-seed): QR path uses the camera if SOLARI_HAS_CAMERA (parse
  // otpauth:// URI); manual path opens a Base32 keyboard entry sheet.
}

// ---- vault -----------------------------------------------------------------
void Ui::drawVaultUnlock_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString("Unlock Vault", kW / 2, 180);
  d.setTextSize(2);
  d.setTextColor(TFT_LIGHTGREY);
  d.drawString("Enter master password / PIN", kW / 2, 240);
  // TODO(unlock): render a masked entry field + on-screen keyboard; on submit
  // call vault().unlock(secret, preferOffline=!wifiUp) and route on result:
  //   Ok -> Screen::Vault; BadPassword -> shake + clear; Network -> offline try
  d.setTextDatum(top_left);
}

void Ui::drawVault_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("Vault", 40, 60);
  // TODO(search): search box bound to vault().search(query); render results as
  // tappable rows (name / username). Row tap -> ItemDetail(openItemId_).
  auto results = vault().search("");
  int y = 130;
  d.setTextSize(2);
  for (auto& it : results) {
    d.setTextColor(TFT_WHITE);
    d.drawString(it.name.c_str(), 60, y);
    d.setTextColor(TFT_LIGHTGREY);
    d.drawString(it.username.c_str(), 60, y + 28);
    y += 76;
    if (y > kH - 60) break;  // TODO: scroll
  }
  if (results.empty()) {
    d.setTextColor(TFT_LIGHTGREY);
    d.drawString("Vault empty or locked.", 60, 140);
  }
}

void Ui::drawItemDetail_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  // TODO(detail): show name/username, [Show password] (calls
  // vault().revealPassword), [Copy user], [Copy password] (vault().copyField),
  // and a live TOTP if the item carries one (vault().revealTotp).
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("Item", 40, 60);
}

// ---- alerts ----------------------------------------------------------------
void Ui::drawAlerts_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("Alerts", 40, 60);
  notify().markAllRead();

  int y = 120;
  d.setTextSize(2);
  for (const auto& n : notify().feed()) {
    d.fillRect(40, y, 10, 60, sevColor(n.severity));  // severity spine
    d.setTextColor(TFT_WHITE);
    d.drawString(n.title.c_str(), 64, y);
    d.setTextColor(TFT_LIGHTGREY);
    String meta = n.source;
    if (n.ts) {
      char tb[16]; struct tm tmv; localtime_r(&n.ts, &tmv);
      strftime(tb, sizeof(tb), "%H:%M", &tmv);
      meta += "  " + String(tb);
    }
    d.drawString((meta + "  " + n.body).c_str(), 64, y + 28);
    y += 78;
    if (y > kH - 60) break;  // TODO: scroll for full history
  }
  if (notify().feed().empty()) {
    d.setTextColor(TFT_LIGHTGREY);
    d.drawString("No alerts yet.", 64, 140);
  }
}

void Ui::drawSetup_() {
  auto& d = M5.Display;
  d.fillRect(0, 0, kW, kH, TFT_NAVY);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString("SolariNet Companion — Setup", kW / 2, 200);
  d.setTextSize(2);
  d.drawString("Provision Wi-Fi, server URLs, and master password.", kW / 2, 270);
  d.drawString("(first-boot wizard — TODO)", kW / 2, 320);
  d.setTextDatum(top_left);
  // TODO(setup): multi-step wizard — Wi-Fi scan+join, MQTT host/creds, PM base
  // URL + email, then set config().settings().provisioned=true; saveSettings().
}

void Ui::drawToast_() {
  auto& d = M5.Display;
  int w = 600, h = 70, x = (kW - w) / 2, yb = kH - 120;
  d.fillRoundRect(x, yb, w, h, 16, TFT_BLACK);
  d.drawRoundRect(x, yb, w, h, 16, toastColor_);
  d.setTextColor(toastColor_);
  d.setTextDatum(middle_center);
  d.setTextSize(2);
  d.drawString(toastText_.c_str(), kW / 2, yb + h / 2);
  d.setTextDatum(top_left);
}

// ---- touch routing ---------------------------------------------------------
static bool hit(const Rect& r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void Ui::handleTouch_(int x, int y) {
  dirty_ = true;
  // Global: tapping the status bar (top-left area) returns Home.
  if (y < kStatusH && x < 200 && screen_ != Screen::Home) {
    goTo(Screen::Home);
    return;
  }
  switch (screen_) {
    case Screen::Home:
      if (hit(kTileTotp, x, y)) goTo(Screen::Totp);
      else if (hit(kTileVault, x, y))
        goTo(vault().isUnlocked() ? Screen::Vault : Screen::VaultUnlock);
      else if (hit(kTileAlerts, x, y)) goTo(Screen::Alerts);
      break;
    case Screen::Totp:
      // TODO: [ + Add ] hit-test (top-right) opens add-seed sheet.
      break;
    default:
      break;
  }
}

}  // namespace solari
