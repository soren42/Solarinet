# NFC 2FA — hardware TODO (what makes it live)

Everything in this repo works with the **mock** backend today. To go live you need
a positively-identified reader on each host. Below is exactly what to run, what the
output means, and what I already observed on xenon (2026-07-06).

---

## A. xenon (10.0.0.20) — the alleged built-in reader

### What I actually saw

Installed `libnfc-bin libnfc-dev pcscd pcsc-tools` (i2c-tools was already present)
and probed every angle:

| Check | Command | Result |
|-------|---------|--------|
| USB enumeration | `lsusb` | No NFC/CCID device. Only Hitachi hubs + an ESP32 JTAG unit. |
| Kernel NFC drivers | `lsmod \| grep -iE 'nfc\|pn5\|pn7\|nci'` | **none loaded** |
| ACPI NFC device | `ls /sys/bus/acpi/devices \| grep -iE 'NXP\|PN5\|PN7\|NFC'` | **none** |
| libnfc USB autoscan | `nfc-list` | `No NFC device found` (only tries acr122_usb + pn53x_usb) |
| I2C devices on bus 5 | `i2cdetect -y 5` | devices at **0x08** and **0x44** only |
| **What bus 5 is** | `cat /sys/class/i2c-dev/i2c-5/name` | **`SMBus I801 adapter at 0000:00:1f.4`** |
| PN532 probe on bus 5 | device config `pn532_i2c:/dev/i2c-5` + `nfc-scan-device` (as root, intrusive) | libnfc talks to addr 0x24 and gets **`Remote I/O error` / no ACK** — **no PN532 responds** |
| Identify 0x44 | `i2cget -y 5 0x44` | `0xff` (looks like a sensor/SPD, not an NFC controller) |

### Conclusion

**xenon has no identifiable NFC reader.** Bus 5 is the Intel PCH **SMBus** (i801) —
the 0x08/0x44 devices are motherboard housekeeping (SPD/sensor/SMBus host), *not* a
PN532 (which would ACK at 0x24) nor a PN7150 (0x28). A PN532's usual I2C address
`0x24` is silent. There is no USB CCID reader, no kernel NFC stack, no ACPI NFC node.

### To positively identify it (if you believe one exists)

Run these and look for the distinguishing output:

```bash
# 1. Is it a hidden USB/CCID device that just isn't an obvious "NFC" string?
lsusb -v 2>/dev/null | grep -iE 'Chip.?Card|CCID|bInterfaceClass.*11|ACS|NXP|SCM'
#   -> bInterfaceClass 11 (0x0B) == Smart Card / CCID. That's a PC/SC reader.

# 2. Is it a UART-attached PN532 on a header (very common for "built-in")?
ls /dev/ttyUSB* /dev/ttyACM* /dev/ttyS* 2>/dev/null
for d in /dev/ttyUSB* /dev/ttyACM*; do
  nfc-scan-device -v 2>/dev/null   # after adding a devices.d connstring tty:XXX:pn532
done
#   -> a line "chip: PN532 v1.6" means a PN532 UART reader was found.

# 3. Is it an SPI/I2C PN532 on GPIO (SBC-style)? Check DT/ACPI + every SMBus:
for b in $(i2cdetect -l | awk '{print $1}' | sed 's/i2c-//'); do
  echo "bus $b:"; sudo i2cdetect -y "$b" 2>/dev/null | grep -E ' 24 | 28 | 48 '
done
#   -> a device at 0x24 (PN532) or 0x28 (PN7150) is the reader. None found today.

# 4. Firmware/vendor truth: what does the board actually claim?
sudo dmidecode -t baseboard -t system   # board model -> look up its NFC spec
```

**Decision rule:** if (1) shows `bInterfaceClass 11` → it's a **CCID reader, use the
`pcsc` backend** (no kernel NFC modules needed, just `pcscd`). If (2)/(3) find a
**PN532/PN7150** → use the `libnfc` backend with the matching connstring. If none of
these produce a chip line (today's outcome) → **there is no reader; use hydrogen.**

---

## B. hydrogen (10.0.1.50, M4 Mac Mini) — USB PC/SC reader

This is the viable path. macOS has PC/SC (`pcscd`) built in; an ACR122U-class
reader is a standard CCID device.

```bash
# 1. Confirm macOS sees the reader (Terminal on hydrogen):
system_profiler SPUSBDataType | grep -iA3 -E 'ACR|NFC|CCID|Smart'

# 2. Python + pyscard (Homebrew python or system python3):
pip3 install pyscard            # needs Xcode CLT for the PC/SC C ext
python3 nfc-reader.py --backend pcsc --selftest      # tap a card -> prints its UID

# 3. Run the daemon (loopback only), pointed at the dashboard origin:
python3 nfc-reader.py --backend pcsc --origin https://xenon:9443
```

Notes:
- macOS ships the PC/SC framework; no `pcscd` install needed. If a third-party
  driver is required for a specific reader (rare for ACR122U), install the vendor
  CCID driver (`libacsccid` / ACS "Unified" driver).
- The daemon binds `127.0.0.1:8770`. The dashboard runs on xenon, so the operator's
  browser on hydrogen reaches `https://xenon:9443` for the API **and**
  `http://127.0.0.1:8770` for the local reader — both are same-origin-safe because
  the reader daemon CORS-allows exactly the dashboard origin.
- Mixed-content caveat: the dashboard is HTTPS and the reader is plain HTTP on
  loopback. Browsers treat `http://127.0.0.1` as a *potentially trustworthy* /
  secure context, so `fetch()` to it from an HTTPS page is allowed (loopback is
  exempt from mixed-content blocking in current Chrome/Safari/Firefox). If a future
  browser tightens this, front the daemon with a localhost TLS cert.

---

## C. Linux reader host (if you add a USB reader to xenon or an outpost)

```bash
sudo apt-get install -y pcscd pcsc-tools libpcsclite-dev python3-pyscard
sudo systemctl enable --now pcscd
pcsc_scan                        # tap a card -> shows ATR + card type
python3 nfc-reader.py --backend pcsc --selftest
```

### Permissions / udev

- `pcscd` mediates reader access, so the daemon user does **not** need raw USB
  perms — it talks to `pcscd` over `/run/pcscd/pcscd.comm`.
- If you instead use `libnfc` direct-to-USB (no pcscd), add a udev rule so a
  non-root daemon can open the device, e.g. for an ACR122U (VID 072f):

  ```
  # /etc/udev/rules.d/99-solari-nfc.rules
  SUBSYSTEM=="usb", ATTR{idVendor}=="072f", MODE="0660", GROUP="plugdev", TAG+="uaccess"
  ```
  then `sudo udevadm control --reload && sudo udevadm trigger`, and put the daemon
  user in `plugdev`.
- For a **PN532 over I2C** (SBC only): the daemon needs read/write on `/dev/i2c-N`
  — add the user to the `i2c` group (`sudo usermod -aG i2c <user>`) and ensure a
  udev rule sets `GROUP="i2c", MODE="0660"` on `/dev/i2c-N`. (On xenon today
  `/dev/i2c-5` is root-only, which is why the probe needed sudo.)

### Note on my xenon changes

I installed the packages above and briefly started `pcscd`, added a throwaway
`/etc/nfc/devices.d/pn532_i2c.conf` and toggled `allow_intrusive_scan` to run the
PN532 probe, then **removed the throwaway config and reverted `libnfc.conf`**. The
installed packages were left in place (harmless; `pcscd` finds no reader). Nothing
in the SolariNet repo or the live dashboard was modified.

---

## D. Go-live checklist

1. Identify a reader on at least one operator host (§A/§B/§C). Realistically:
   **hydrogen via `pcsc`.**
2. `pip install pyscard`; run `nfc-reader.py --backend pcsc` on that host.
3. Copy the scaffold files and apply the patches in `dashboard/INTEGRATION.md`.
4. Set `nfc2fa.enabled: true` in `solari-auth.json` (start `enforce: "enrolled"`).
5. As admin, enrol your own card first (EnrollCard UI → tap). Verify the login tap
   flow works. **Only then** consider `enforce: "all"`.
6. (Later) DESFire card + Mode B; MariaDB `nfcCredential` via solariCtl; optional
   reader HMAC; Keycloak-native authenticator (DESIGN.md §6b).
