"""Stable SOA serials derived from the last applied generation, never a draft."""
import datetime
import os
import pathlib
import re
import tempfile

SERIAL_TOKEN = "__SOLARI_SERIAL__"
SOA_RE = re.compile(r"(\bIN\s+SOA\s+\S+\s+\S+\s*\(\s*)(\d+|" + SERIAL_TOKEN + r")",
                    re.IGNORECASE)


def stampZone(template, previousPath, today=None):
    """Reuse an unchanged zone serial, or advance under RFC 1982 arithmetic."""
    current = SOA_RE.search(template)
    if current is None or current.group(2) != SERIAL_TOKEN or len(SOA_RE.findall(template)) != 1:
        raise ValueError("zone template must contain exactly one SOA serial token")
    previousPath = pathlib.Path(previousPath)
    previous = previousPath.read_text(encoding="utf-8") if previousPath.exists() else None
    oldSerial = None
    if previous is not None:
        match = SOA_RE.search(previous)
        if match is None or not match.group(2).isdigit():
            raise ValueError(f"invalid previous SOA serial: {previousPath}")
        oldSerial = int(match.group(2))
        if not 0 <= oldSerial <= 0xffffffff:
            raise ValueError(f"previous SOA serial out of range: {previousPath}")
        canonical = lambda text: SOA_RE.sub(lambda m: m.group(1) + SERIAL_TOKEN,
                                            text, count=1)
        if canonical(previous) == canonical(template):
            return SOA_RE.sub(lambda m: m.group(1) + str(oldSerial), template, count=1)
    date = today or datetime.datetime.now(datetime.timezone.utc).date()
    serial = int(date.strftime("%Y%m%d")) * 100 + 1
    if serial > 0xffffffff:
        raise ValueError("date-based SOA serial exceeds 32 bits")
    if oldSerial is not None:
        distance = (serial - oldSerial) & 0xffffffff
        if not 0 < distance < 0x80000000:
            serial = (oldSerial + 1) & 0xffffffff
    return SOA_RE.sub(lambda m: m.group(1) + str(serial), template, count=1)


def atomicWrite(path, text):
    """Write a complete generated file before replacing its previous pathname."""
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".zone-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            output.write(text)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
