# solari-glance — Linux half of the UNO Q on-board 8x13 matrix glance.
#
# Reads the facts the solariPanel daemon publishes at
# /run/solari-panel/glance.json (atomic rename, refreshed every 2 s) and
# renders them as a 104-byte grayscale frame (0..7, row-major, 13x8) pushed
# to the MCU sketch's "draw" provider over the router bridge.
#
# Layout (13 wide x 8 tall):
#   row 0      status dots: c0 poll, c2 serial link, c4 STATE seen,
#              c12 alarm (bright = armed+unacked, mid = acked)
#   rows 2-3   score bar, 0..200 -> 0..13 px
#   row 5      up count      (bright)
#   row 6      degraded count (mid)
#   row 7      down count    (blinking bright)
#   armed+unacked alarm: full-matrix pulse overlaid (mirrors the wall beacon)
#   sleeping panel: everything halved
#   stale/missing facts: lone blinking pixel at row 0 col 6

import json
import threading
import time

from arduino.app_utils import App, Bridge, Logger

# The app container mounts the app dir at /app; the host's
# solari-glance-sync.path unit mirrors /run/solari-panel/glance.json here on
# every atomic rename (the container cannot see /run/solari-panel directly).
GLANCE_PATH = "/app/glance.json"
W, H = 13, 8
TICK_S = 0.25          # animation tick (blink/pulse phases)
STALE_S = 15           # facts older than this are not trusted

logger = Logger("solari-glance")


def readFacts():
    """Return the daemon's glance dict, or None when missing/stale/torn."""
    try:
        with open(GLANCE_PATH) as f:
            facts = json.load(f)
    except (OSError, ValueError):
        return None
    if time.time() - facts.get("ts", 0) > STALE_S:
        return None
    return facts


def buildFrame(facts, phase):
    """Map facts -> 104 grayscale bytes. phase is an int animation counter."""
    fb = [0] * (W * H)
    blink = phase % 4 < 2          # ~2 Hz at a 0.25 s tick

    if facts is None:
        # Feed alive but daemon facts missing/stale: lone blinker mid-top.
        fb[6] = 5 if blink else 1
        return bytes(fb)

    def dot(col, level):
        fb[col] = level

    # --- row 0: link chain dots ------------------------------------------
    dot(0, 6 if facts.get("pollOk") else (7 if blink else 0))
    dot(2, 6 if facts.get("serialUp") else (7 if blink else 0))
    dot(4, 6 if facts.get("stateSeen") else 2)

    alarm = facts.get("alarmActive", 0)
    acked = facts.get("alarmAcked", 0)
    if alarm:
        dot(12, (7 if blink else 3) if not acked else 3)

    # --- rows 2-3: score bar (0..200 clamps to full width) ---------------
    score = min(max(int(facts.get("score", 0)), 0), 200)
    px = round(score * W / 200)
    for c in range(px):
        fb[2 * W + c] = 4
        fb[3 * W + c] = 4

    # Heartbeat: when the bar is empty (score 0 = calm fleet) breathe a
    # single mid-bar pixel, ~3 s period, so healthy still reads as alive.
    if px == 0 and not facts.get("alarmActive", 0):
        beat = [0, 1, 2, 3, 4, 5, 4, 3, 2, 1, 0, 0][phase % 12]
        fb[2 * W + 6] = beat
        fb[3 * W + 6] = beat

    # --- rows 5-7: fleet strip -------------------------------------------
    def strip(row, count, level):
        for c in range(min(int(count), W)):
            fb[row * W + c] = level

    strip(5, facts.get("up", 0), 5)
    strip(6, facts.get("degraded", 0), 3)
    strip(7, facts.get("down", 0), 7 if blink else 2)

    # --- alarm pulse overlay (mirrors the wall panel's beacon) -----------
    if alarm and not acked:
        pulse = [0, 1, 2, 3, 4, 3, 2, 1][phase % 8]
        fb = [max(v, pulse) for v in fb]

    # --- sleeping panel dims the whole glance ----------------------------
    if facts.get("sleeping"):
        fb = [v // 2 for v in fb]

    return bytes(fb)


def feed():
    """Push a frame every TICK_S forever; bridge errors just retry."""
    phase = 0
    facts = None
    lastRead = 0.0
    while True:
        nowS = time.monotonic()
        if nowS - lastRead >= 1.0:           # re-read facts at 1 Hz
            facts = readFacts()
            lastRead = nowS
        try:
            Bridge.call("draw", buildFrame(facts, phase))
        except Exception as e:               # bridge hiccup: log once a while
            if phase % 40 == 0:
                logger.warning(f"draw failed: {e}")
        phase += 1
        time.sleep(TICK_S)


threading.Thread(target=feed, daemon=True).start()
App.run()
