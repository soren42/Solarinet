#!/usr/bin/env python3
"""Bounded serial capture with out-of-band job markers for the host supervisor."""

from __future__ import annotations

import errno
import os
import re
import select
import sys

LIMIT = 4 * 1024 * 1024
RETAIN = 2 * 1024 * 1024
EXIT_MARKER = re.compile(rb"(?:^|\n)SOLARINET_CI_EXIT=([0-9]{1,3})\r?\n")
START_MARKER = b"SOLARINET_CI_STARTED"


def touch(path: str, value: bytes = b"") -> None:
    temporary = f"{path}.{os.getpid()}.tmp"
    with open(temporary, "wb") as marker:
        marker.write(value)
        marker.flush()
        os.fsync(marker.fileno())
    os.replace(temporary, path)


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: serial-capture.py DEVICE LOG START_MARKER EXIT_MARKER", file=sys.stderr)
        return 2
    device, log_path, start_path, exit_path = sys.argv[1:]
    descriptor = os.open(device, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    rolling = bytearray()
    with open(log_path, "w+b") as log:
        try:
            while True:
                ready, _, _ = select.select([descriptor], [], [], 1)
                if not ready:
                    continue
                try:
                    chunk = os.read(descriptor, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:  # Linux PTY guest side closed.
                        break
                    raise
                if not chunk:
                    break
                rolling.extend(chunk)
                if START_MARKER in rolling and not os.path.exists(start_path):
                    touch(start_path)
                match = EXIT_MARKER.search(rolling)
                if match and not os.path.exists(exit_path):
                    touch(exit_path, match.group(1))
                if len(rolling) > 256:
                    del rolling[:-256]
                log.write(chunk)
                log.flush()
                if log.tell() > LIMIT:
                    log.seek(-RETAIN, os.SEEK_END)
                    tail = log.read(RETAIN)
                    log.seek(0)
                    log.write(tail)
                    log.truncate()
                    log.flush()
        finally:
            os.close(descriptor)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
