#include "ui.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "totp.h"
#include "vault.h"
#include "approvals.h"
#include "mqttbus.h"
#include "pwgen.h"
#include "blehid.h"

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
struct Rect { int x, y, w, h; };
const Rect kTileTotp{80, 150, 340, 360};
const Rect kTileApprove{470, 150, 340, 360};
const Rect kTileVault{860, 150, 340, 360};
const Rect kBtnGen{80, 540, 520, 120};      // home footer: password generator
const Rect kBtnPair{680, 540, 520, 120};    // home footer: BLE pairing

// Password-generator controls.
const Rect kPgLenMinus{120, 180, 80, 80};
const Rect kPgLenPlus{300, 180, 80, 80};
const Rect kPgUpper{120, 300, 260, 60};
const Rect kPgLower{120, 380, 260, 60};
const Rect kPgDigit{120, 460, 260, 60};
const Rect kPgSym{120, 540, 260, 60};
const Rect kPgAmbig{440, 540, 380, 60};
const Rect kPgRegen{440, 300, 360, 90};
const Rect kPgType{440, 410, 360, 90};
const Rect kPgSave{860, 410, 360, 90};

// Approval action buttons.
const Rect kApDeny{140, 520, 440, 140};
const Rect kApApprove{700, 520, 440, 140};

// Current password-generator policy (persists across regen while on screen).
PwGenPolicy g_pol;

bool hit(const Rect& r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
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

  auto t = M5.Touch.getDetail();
  if (t.wasClicked()) handleTouch_(t.x, t.y);

  if (s_clipClearAt && millis() > s_clipClearAt) {
    s_clip = ""; s_clipClearAt = 0;
  }

  // TOTP + Approval animate (countdown rings); others redraw on dirty.
  bool tick = (millis() - lastTickMs_) > 250;
  if ((screen_ == Screen::Totp || screen_ == Screen::Approval) && tick)
    dirty_ = true;

  if (!dirty_ && !tick) return;
  lastTickMs_ = millis();

  M5.Display.startWrite();
  switch (screen_) {
    case Screen::Setup: drawSetup_(); break;
    case Screen::Home: drawHome_(); break;
    case Screen::Totp: drawTotp_(); break;
    case Screen::Approval: drawApproval_(); break;
    case Screen::VaultUnlock: drawVaultUnlock_(); break;
    case Screen::Vault: drawVault_(); break;
    case Screen::ItemDetail: drawItemDetail_(); break;
    case Screen::PwGen: drawPwGen_(); break;
    default: drawHome_(); break;
  }
  drawStatusBar_();
  if (toastUntilMs_ && millis() < toastUntilMs_) drawToast_();
  M5.Display.endWrite();
  dirty_ = false;
}

void Ui::goTo(Screen s) { screen_ = s; dirty_ = true; }

void Ui::toast(const String& text, uint16_t color, uint32_t ms) {
  toastText_ = text; toastColor_ = color;
  toastUntilMs_ = millis() + ms; dirty_ = true;
}

// ---- status bar ------------------------------------------------------------
void Ui::drawStatusBar_() {
  auto& d = M5.Display;
  d.fillRect(0, 0, kW, kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(middle_left);
  d.setTextSize(2);

  char clk[16] = "--:--";
  time_t now = time(nullptr);
  if (totp::timeIsValid()) {
    struct tm tmv; localtime_r(&now, &tmv);
    strftime(clk, sizeof(clk), "%H:%M", &tmv);
  }
  d.drawString(clk, 16, kStatusH / 2);

  d.setTextDatum(middle_right);
  String right;
  right += (WiFi.status() == WL_CONNECTED) ? "WiFi " : "no-wifi ";
  right += mqttBus().connected() ? "MQTT " : "mqtt? ";
  if (bleHid().available())
    right += bleHid().connected() ? "BLE " : "ble- ";
  right += vault().isUnlocked() ? "unlocked " : "locked ";
  if (approvals().pendingCount()) right += "[!" + String(approvals().pendingCount()) + "] ";
  d.drawString(right, kW - 16, kStatusH / 2);
}

// ---- home ------------------------------------------------------------------
static void tile(const char* title, const String& subtitle, const Rect& r,
                 uint16_t accent) {
  auto& d = M5.Display;
  d.fillRoundRect(r.x, r.y, r.w, r.h, 24, TFT_DARKGREY);
  d.fillRoundRect(r.x, r.y, r.w, 12, 24, accent);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(4);
  d.drawString(title, r.x + 28, r.y + 44);
  d.setTextSize(2);
  d.setTextColor(TFT_LIGHTGREY);
  d.drawString(subtitle.c_str(), r.x + 28, r.y + 112);
}

static void button(const Rect& r, const char* label, uint16_t color,
                   bool filled = false) {
  auto& d = M5.Display;
  if (filled) d.fillRoundRect(r.x, r.y, r.w, r.h, 18, color);
  else d.drawRoundRect(r.x, r.y, r.w, r.h, 18, color);
  d.setTextColor(filled ? TFT_BLACK : color);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
  d.setTextDatum(top_left);
}

void Ui::drawHome_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("SolariNet Authenticator", 80, 84);

  auto accounts = config().loadTotpAccounts();
  tile("TOTP", String(accounts.size()) + " codes", kTileTotp, TFT_CYAN);
  size_t pend = approvals().pendingCount();
  tile("Approvals", pend ? (String(pend) + " PENDING") : String("idle"),
       kTileApprove, pend ? TFT_YELLOW : TFT_GREEN);
  tile("Vault", vault().isUnlocked() ? "unlocked" : "locked", kTileVault,
       TFT_GREEN);

  button(kBtnGen, "Generate Password", TFT_CYAN);
  const char* pairLbl = !bleHid().available() ? "BLE unavailable"
                        : bleHid().connected() ? "BLE keyboard: connected"
                                               : "Pair BLE keyboard";
  button(kBtnPair, pairLbl, bleHid().available() ? TFT_ORANGE : TFT_DARKGREY);
}

// ---- TOTP ------------------------------------------------------------------
static void drawRing(int cx, int cy, int r, float frac, uint16_t color) {
  M5.Display.drawArc(cx, cy, r, r - 8, 0, (int)(360 * frac), color);
}

void Ui::drawTotp_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("TOTP", 40, 60);
  d.setTextDatum(top_right);
  d.drawString("[ + Add ]", kW - 40, 60);
  d.setTextDatum(top_left);

  time_t now = time(nullptr);
  auto accounts = config().loadTotpAccounts();
  int y = 130;
  for (auto& a : accounts) {
    totp::Code code;
    String shown = totp::compute(a, now, code) ? code.digits : String("------");
    if (shown.length() == 6) shown = shown.substring(0, 3) + " " + shown.substring(3);

    d.setTextSize(2);
    d.setTextColor(TFT_LIGHTGREY);
    d.drawString((a.issuer + "  " + a.label).c_str(), 60, y);
    d.setTextSize(5);
    d.setTextColor(TFT_CYAN);
    d.drawString(shown.c_str(), 60, y + 30);

    // "Type code" (BLE-HID) action, if a keyboard is connected.
    if (bleHid().connected()) {
      d.setTextSize(2);
      d.setTextColor(TFT_ORANGE);
      d.drawString("[ Type ]", 520, y + 44);
    }

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
}

// ---- Approval prompt -------------------------------------------------------
void Ui::drawApproval_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  const ApprovalRequest* r = approvals().pending();
  if (!r) {
    d.setTextColor(TFT_LIGHTGREY);
    d.setTextDatum(middle_center);
    d.setTextSize(3);
    d.drawString("No pending approvals", kW / 2, kH / 2);
    d.setTextDatum(top_left);
    return;
  }
  uint32_t left = approvals().secondsLeft(*r);

  d.setTextColor(TFT_YELLOW);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("Approval requested", 60, 70);

  d.setTextSize(4);
  d.setTextColor(TFT_WHITE);
  d.drawString(r->subject, 60, 140);
  d.setTextSize(3);
  d.setTextColor(TFT_LIGHTGREY);
  d.drawString(r->action + " -> " + r->detail, 60, 210);
  d.setTextSize(2);
  d.drawString("from " + r->sourceIp, 60, 270);

  // TTL countdown ring (top-right).
  float frac = r->ttl ? (float)left / r->ttl : 0;
  drawRing(kW - 160, 200, 90, frac, left <= 5 ? TFT_RED : TFT_YELLOW);
  d.setTextDatum(middle_center);
  d.setTextSize(4);
  d.setTextColor(TFT_WHITE);
  d.drawString(String(left) + "s", kW - 160, 200);
  d.setTextDatum(top_left);

  button(kApDeny, "DENY", TFT_RED, /*filled=*/true);
  button(kApApprove, "APPROVE", TFT_GREEN, /*filled=*/true);

  if (approvals().pendingCount() > 1) {
    d.setTextSize(2);
    d.setTextColor(TFT_LIGHTGREY);
    d.drawString("+" + String(approvals().pendingCount() - 1) + " more queued",
                 60, 320);
  }
}

// ---- password generator ----------------------------------------------------
void Ui::regeneratePassword_() { pwgen().generate(g_pol); dirty_ = true; }

static void toggleRow(const Rect& r, const char* label, bool on) {
  auto& d = M5.Display;
  d.drawRoundRect(r.x, r.y, r.w, r.h, 12, on ? TFT_GREEN : TFT_DARKGREY);
  d.setTextColor(on ? TFT_GREEN : TFT_LIGHTGREY);
  d.setTextDatum(middle_left);
  d.setTextSize(3);
  d.drawString((String(on ? "[x] " : "[ ] ") + label).c_str(),
               r.x + 20, r.y + r.h / 2);
  d.setTextDatum(top_left);
}

void Ui::drawPwGen_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("Password Generator", 40, 60);

  // Length stepper.
  d.setTextSize(3);
  d.setTextColor(TFT_LIGHTGREY);
  d.drawString("Length", 120, 140);
  button(kPgLenMinus, "-", TFT_CYAN);
  button(kPgLenPlus, "+", TFT_CYAN);
  d.setTextSize(5);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(middle_center);
  d.drawString(String(g_pol.length), 240, 220);
  d.setTextDatum(top_left);

  toggleRow(kPgUpper, "A-Z", g_pol.upper);
  toggleRow(kPgLower, "a-z", g_pol.lower);
  toggleRow(kPgDigit, "0-9", g_pol.digits);
  toggleRow(kPgSym, "!@#", g_pol.symbols);
  toggleRow(kPgAmbig, "avoid ambiguous", g_pol.avoidAmbiguous);

  // Generated value.
  const String& pw = pwgen().last();
  d.fillRoundRect(440, 150, 780, 110, 16, TFT_NAVY);
  d.setTextColor(pw.isEmpty() ? TFT_LIGHTGREY : TFT_CYAN);
  d.setTextDatum(middle_left);
  d.setTextSize(3);
  d.drawString(pw.isEmpty() ? "(enable a class, then Regenerate)" : pw,
               460, 205);
  d.setTextDatum(top_left);

  button(kPgRegen, "Regenerate", TFT_CYAN);
  button(kPgType, bleHid().connected() ? "Type it" : "Type (no BLE)",
         bleHid().connected() ? TFT_ORANGE : TFT_DARKGREY);
  button(kPgSave, "Save to Vault", TFT_GREEN);
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
  // TODO(unlock): masked entry field + on-screen keyboard; on submit call
  // vault().unlock(secret, preferOffline=!wifiUp) and route on result.
  d.setTextDatum(top_left);
}

void Ui::drawVault_() {
  auto& d = M5.Display;
  d.fillRect(0, kStatusH, kW, kH - kStatusH, TFT_BLACK);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("Vault", 40, 60);
  // TODO(search): search box + tappable rows; row tap -> ItemDetail.
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
  // TODO(detail): name/username, [Show password], [Copy], and — when a BLE
  // keyboard is connected — [Type password]/[Type TOTP] via bleHid().
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(3);
  d.drawString("Item", 40, 60);
}

void Ui::drawSetup_() {
  auto& d = M5.Display;
  d.fillRect(0, 0, kW, kH, TFT_NAVY);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(middle_center);
  d.setTextSize(3);
  d.drawString("SolariNet Authenticator — Setup", kW / 2, 180);
  d.setTextSize(2);
  d.drawString("Provision Wi-Fi, MQTT broker, and enroll the device key.",
               kW / 2, 250);
  d.drawString("(first-boot wizard — TODO)", kW / 2, 300);
  // Show the enrolled public key so the operator can paste it into the broker.
  String pub = approvals().publicKeyHex();
  if (!pub.isEmpty()) {
    d.setTextSize(1);
    d.setTextColor(TFT_CYAN);
    d.drawString("device pubkey (enroll in broker):", kW / 2, 380);
    d.drawString(pub.substring(0, 66), kW / 2, 405);
    d.drawString(pub.substring(66), kW / 2, 425);
  }
  d.setTextDatum(top_left);
  // TODO(setup): multi-step wizard — Wi-Fi scan+join, MQTT host/creds, device
  // id, then provisioned=true; saveSettings(). Enroll pubkey to authbroker.
}

void Ui::drawToast_() {
  auto& d = M5.Display;
  int w = 700, h = 70, x = (kW - w) / 2, yb = kH - 110;
  d.fillRoundRect(x, yb, w, h, 16, TFT_BLACK);
  d.drawRoundRect(x, yb, w, h, 16, toastColor_);
  d.setTextColor(toastColor_);
  d.setTextDatum(middle_center);
  d.setTextSize(2);
  d.drawString(toastText_.c_str(), kW / 2, yb + h / 2);
  d.setTextDatum(top_left);
}

// ---- touch routing ---------------------------------------------------------
void Ui::handleTouch_(int x, int y) {
  dirty_ = true;
  // Global: tapping the top-left status area returns Home.
  if (y < kStatusH && x < 200 && screen_ != Screen::Home) {
    goTo(Screen::Home);
    return;
  }
  switch (screen_) {
    case Screen::Home:
      if (hit(kTileTotp, x, y)) goTo(Screen::Totp);
      else if (hit(kTileApprove, x, y)) goTo(Screen::Approval);
      else if (hit(kTileVault, x, y))
        goTo(vault().isUnlocked() ? Screen::Vault : Screen::VaultUnlock);
      else if (hit(kBtnGen, x, y)) { regeneratePassword_(); goTo(Screen::PwGen); }
      else if (hit(kBtnPair, x, y) && bleHid().available()) {
        bleHid().advertise(true);
        toast("BLE advertising — pair from the host", TFT_ORANGE, 4000);
      }
      break;
    case Screen::Approval:
      if (hit(kApApprove, x, y)) {
        approvals().approve();
        toast("Approved", TFT_GREEN, 2500);
        goTo(approvals().pending() ? Screen::Approval : Screen::Home);
      } else if (hit(kApDeny, x, y)) {
        approvals().deny();
        toast("Denied", TFT_RED, 2500);
        goTo(approvals().pending() ? Screen::Approval : Screen::Home);
      }
      break;
    case Screen::PwGen:
      handlePwGenTouch_(x, y);
      break;
    case Screen::Totp:
      // TODO: [ + Add ] hit-test; per-row [ Type ] -> bleHid().typeTotp(code).
      break;
    default:
      break;
  }
}

void Ui::handlePwGenTouch_(int x, int y) {
  if (hit(kPgLenMinus, x, y)) { if (g_pol.length > 8) g_pol.length--; regeneratePassword_(); }
  else if (hit(kPgLenPlus, x, y)) { if (g_pol.length < 64) g_pol.length++; regeneratePassword_(); }
  else if (hit(kPgUpper, x, y)) { g_pol.upper = !g_pol.upper; regeneratePassword_(); }
  else if (hit(kPgLower, x, y)) { g_pol.lower = !g_pol.lower; regeneratePassword_(); }
  else if (hit(kPgDigit, x, y)) { g_pol.digits = !g_pol.digits; regeneratePassword_(); }
  else if (hit(kPgSym, x, y)) { g_pol.symbols = !g_pol.symbols; regeneratePassword_(); }
  else if (hit(kPgAmbig, x, y)) { g_pol.avoidAmbiguous = !g_pol.avoidAmbiguous; regeneratePassword_(); }
  else if (hit(kPgRegen, x, y)) { regeneratePassword_(); }
  else if (hit(kPgType, x, y)) {
    if (bleHid().connected() && !pwgen().last().isEmpty()) {
      bleHid().typePassword(pwgen().last());
      toast("Typed over BLE", TFT_ORANGE, 2500);
    } else {
      toast("Pair a BLE keyboard first", TFT_RED, 2500);
    }
  } else if (hit(kPgSave, x, y)) {
    // TODO(vault-store): open a "new credential" sheet (name/username), then
    // vault().addItem(..., pwgen().last()). For now just acknowledge.
    toast("Save to Vault — TODO (needs vault write path)", TFT_YELLOW, 3000);
  }
}

}  // namespace solari
