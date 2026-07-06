#!/usr/bin/env python3
"""
nfc-reader.py — SolariNet NFC 2FA local reader daemon.

Exposes a loopback-only HTTP interface that returns the UID (and, later, a
challenge-response envelope) of the card currently presented to a reader. The
dashboard's browser code calls this directly during enrolment and 2FA verify;
the daemon never talks to the SolariNet API or database — it only reads cards.

Backends (pluggable, --backend):
  mock    - emits a fixed UID; no hardware. Default so the flow is testable now.
  pcsc    - PC/SC via pyscard. For the hydrogen USB reader (ACR122U/CCID) and any
            CCID reader. UID via APDU FF CA 00 00 00.
  libnfc  - PN532 over UART/I2C/SPI via nfcpy (or a libnfc CLI shell-out). For a
            bare built-in reader IF one is ever positively identified.

Wire format: see reader/README.md and DESIGN.md §5. All responses are JSON.
No third-party dependency is required for the mock backend or the HTTP server
(stdlib http.server only); pcsc needs `pyscard`, libnfc needs `nfcpy`.

Security posture: binds 127.0.0.1 only; CORS restricted to the configured
dashboard origin; the daemon holds no secrets and performs no auth of its own
(the browser's authenticated session and the server-issued challenge are what
bind a read to a login attempt). See DESIGN.md §4.3 for the optional reader HMAC.
"""

import argparse
import json
import sys
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

VERSION = "0.1"


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# --------------------------------------------------------------------------- #
# Backends. Each implements read_card(timeout, challenge, ticket) -> dict|None #
# Returning None means "no card presented within timeout".                    #
# Raising NoReaderError means "backend has no usable hardware".               #
# --------------------------------------------------------------------------- #
class NoReaderError(RuntimeError):
    pass


class Backend:
    name = "base"

    def health(self):
        return {"ok": True, "backend": self.name, "version": VERSION}

    def read_card(self, timeout, challenge=None, ticket=None):
        raise NotImplementedError


class MockBackend(Backend):
    """Emits a deterministic UID so the enrol/verify flow works with no hardware.

    Override the UID with --mock-uid to simulate enrolling different cards.
    """

    name = "mock"

    def __init__(self, uid="04DEADBEEF0102", card_type="MOCK", delay=0.2):
        self.uid = uid.upper()
        self.card_type = card_type
        self.delay = delay

    def read_card(self, timeout, challenge=None, ticket=None):
        # Simulate a brief tap latency, then always "present a card".
        time.sleep(min(self.delay, max(0.0, timeout)))
        return {
            "uid": self.uid,
            "atr": None,
            "type": self.card_type,
            "mode": "uid",
            "crypto": None,  # Mode B stub: a real backend fills this in.
        }


class PcscBackend(Backend):
    """PC/SC (CCID) backend via pyscard — the recommended production path.

    Works with the hydrogen USB reader (ACR122U-class) and any CCID reader. Reads
    the stored UID with the PC/SC-standard "Get Data" APDU FF CA 00 00 00, which
    ACS/CCID readers answer for ISO-14443-A/B cards.
    """

    name = "pcsc"

    def __init__(self):
        try:
            from smartcard.System import readers  # noqa: F401
            from smartcard.CardType import AnyCardType  # noqa: F401
        except Exception as exc:  # pragma: no cover - env dependent
            raise NoReaderError(f"pyscard not available: {exc}")
        self._readers_fn = None

    def _readers(self):
        from smartcard.System import readers
        rs = readers()
        if not rs:
            raise NoReaderError("no PC/SC readers present")
        return rs

    @staticmethod
    def _card_type_from_atr(atr_bytes):
        # Best-effort: PC/SC ATR historical bytes encode the card standard.
        # Full mapping (RID A000000306...) is in the PC/SC "part 3" tables; here
        # we return a coarse hint and let the server treat it as advisory.
        atr = "".join("%02X" % b for b in atr_bytes)
        if "004F0C" in atr:  # PIX prefix commonly present
            if "0003" in atr:
                return "MIFARE"
            if "0044" in atr or "0002" in atr:
                return "NTAG/Ultralight"
        return "ISO14443"

    def read_card(self, timeout, challenge=None, ticket=None):
        from smartcard.CardType import AnyCardType
        from smartcard.CardRequest import CardRequest
        from smartcard.Exceptions import CardRequestTimeoutException

        self._readers()  # raises NoReaderError if none
        cardtype = AnyCardType()
        req = CardRequest(timeout=max(1, int(timeout)), cardType=cardtype)
        try:
            svc = req.waitforcard()
        except CardRequestTimeoutException:
            return None
        conn = svc.connection
        conn.connect()
        atr = conn.getATR()
        # FF CA 00 00 00 -> UID; SW 90 00 on success.
        data, sw1, sw2 = conn.transmit([0xFF, 0xCA, 0x00, 0x00, 0x00])
        if (sw1, sw2) != (0x90, 0x00):
            return None
        uid = "".join("%02X" % b for b in data)
        return {
            "uid": uid,
            "atr": "".join("%02X" % b for b in atr),
            "type": self._card_type_from_atr(atr),
            "mode": "uid",  # Mode B (DESFire auth) would branch here.
            "crypto": None,
        }


class LibnfcBackend(Backend):
    """PN532 (UART/I2C/SPI) backend via nfcpy — for a bare built-in reader.

    NOTE: not usable on xenon (no PN532 present; i2c-5 is the Intel PCH SMBus —
    see HARDWARE_TODO.md). Kept so a positively-identified PN532 needs only a
    connstring, e.g. --libnfc-path 'tty:USB0:pn532' or 'i2c:/dev/i2c-N'.
    """

    name = "libnfc"

    def __init__(self, path="usb"):
        self.path = path
        try:
            import nfc  # noqa: F401
        except Exception as exc:  # pragma: no cover - env dependent
            raise NoReaderError(f"nfcpy not available: {exc}")

    def read_card(self, timeout, challenge=None, ticket=None):
        import nfc

        result = {}

        def on_connect(tag):
            uid = getattr(tag, "identifier", b"")
            result["uid"] = uid.hex().upper()
            result["type"] = str(getattr(tag, "type", "PN532"))
            return False  # release immediately

        try:
            clf = nfc.ContactlessFrontend(self.path)
        except Exception as exc:
            raise NoReaderError(f"cannot open PN532 at {self.path}: {exc}")
        try:
            clf.connect(
                rdwr={"on-connect": on_connect},
                terminate=lambda: False,
            )
        finally:
            clf.close()
        if "uid" not in result:
            return None
        result.setdefault("atr", None)
        result["mode"] = "uid"
        result["crypto"] = None
        return result


BACKENDS = {"mock": MockBackend, "pcsc": PcscBackend, "libnfc": LibnfcBackend}


# --------------------------------------------------------------------------- #
# HTTP layer                                                                  #
# --------------------------------------------------------------------------- #
def make_handler(backend, origin):
    class Handler(BaseHTTPRequestHandler):
        server_version = "SolariNfcReader/" + VERSION

        def _cors(self):
            # Loopback daemon; allow only the configured dashboard origin.
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
            self.send_header("Vary", "Origin")

        def _json(self, code, obj):
            body = json.dumps(obj).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self._cors()
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if code != 204:
                self.wfile.write(body)

        def do_OPTIONS(self):  # noqa: N802
            self.send_response(204)
            self._cors()
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):  # noqa: N802
            parsed = urlparse(self.path)
            q = parse_qs(parsed.query)
            if parsed.path == "/health":
                self._json(200, backend.health())
                return
            if parsed.path == "/read":
                timeout = int((q.get("timeout", ["15"])[0]) or "15")
                timeout = max(1, min(timeout, 60))
                challenge = q.get("challenge", [None])[0]
                ticket = q.get("ticket", [None])[0]
                try:
                    card = backend.read_card(timeout, challenge, ticket)
                except NoReaderError as exc:
                    self._json(503, {"ok": False, "error": "no_reader",
                                     "detail": str(exc)})
                    return
                except Exception as exc:  # keep the daemon alive
                    self._json(500, {"ok": False, "error": "read_error",
                                     "detail": str(exc)})
                    return
                if card is None:
                    self._json(204, {})  # no card within timeout
                    return
                out = {"ok": True, "ts": now_iso()}
                out.update(card)
                # Echo the opaque server-issued values back so the browser can
                # bind this read to its pending login/enrol transaction.
                if challenge is not None:
                    out["challenge"] = challenge
                if ticket is not None:
                    out["ticket"] = ticket
                self._json(200, out)
                return
            self._json(404, {"ok": False, "error": "not_found"})

        def log_message(self, fmt, *args):
            sys.stderr.write("[nfc-reader] %s - %s\n" % (
                self.address_string(), fmt % args))

    return Handler


def build_backend(args):
    if args.backend == "mock":
        return MockBackend(uid=args.mock_uid, card_type=args.mock_type)
    if args.backend == "pcsc":
        return PcscBackend()
    if args.backend == "libnfc":
        return LibnfcBackend(path=args.libnfc_path)
    raise SystemExit("unknown backend: %s" % args.backend)


def main(argv=None):
    ap = argparse.ArgumentParser(description="SolariNet NFC 2FA reader daemon")
    ap.add_argument("--backend", choices=list(BACKENDS), default="mock")
    ap.add_argument("--host", default="127.0.0.1",
                    help="bind address (loopback only; do not expose)")
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--origin", default="https://xenon:9443",
                    help="dashboard origin allowed via CORS")
    ap.add_argument("--mock-uid", default="04DEADBEEF0102")
    ap.add_argument("--mock-type", default="MOCK")
    ap.add_argument("--libnfc-path", default="usb",
                    help="nfcpy connstring, e.g. tty:USB0:pn532 or i2c:/dev/i2c-5")
    ap.add_argument("--selftest", action="store_true",
                    help="instantiate the backend, do one read, print, exit")
    args = ap.parse_args(argv)

    backend = build_backend(args)

    if args.selftest:
        print(json.dumps(backend.health()))
        try:
            card = backend.read_card(timeout=3)
            print(json.dumps({"read": card}))
        except NoReaderError as exc:
            print(json.dumps({"error": "no_reader", "detail": str(exc)}))
        return 0

    handler = make_handler(backend, args.origin)
    httpd = ThreadingHTTPServer((args.host, args.port), handler)
    sys.stderr.write(
        "[nfc-reader] backend=%s listening on http://%s:%d origin=%s\n"
        % (args.backend, args.host, args.port, args.origin))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
