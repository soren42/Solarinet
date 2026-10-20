#!/usr/bin/env python3
"""Validate and atomically activate a complete managed BIND generation.

The root-owned configuration supplies trusted command arrays and filesystem /
container paths. Candidate directories contain data only. A single current
symlink is the activation point; a durable journal recovers interrupted work.
Initial include cutover and seeding require a separate reviewed installation.
"""
import argparse
import datetime
import fcntl
import hashlib
import ipaddress
import json
import os
import pathlib
import pwd
import re
import shutil
import stat
import subprocess
import tempfile
import time

SOA = re.compile(r"\bIN\s+SOA\s+\S+\s+\S+\s*\(\s*(\d+)", re.I)
ZONE = re.compile(r'\bzone\s+"([^"]+)"(?:\s+\w+)?\s*\{')


def syncDirectory(path):
    """Persist pathname changes before reporting a transition complete."""
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def writeJson(path, value):
    """Replace a private state file after syncing its complete contents."""
    fd, temporary = tempfile.mkstemp(prefix=".state-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(value, output, sort_keys=True)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        syncDirectory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def runCommand(config, key, arguments=()):
    """Run a trusted argv array; never log expanded BIND config or key material."""
    command = config[key]
    if not isinstance(command, list) or not command or any(not isinstance(x, str) for x in command):
        raise ValueError(f"invalid command array: {key}")
    result = subprocess.run(command + list(arguments), capture_output=True, text=True,
                            timeout=config.get("commandTimeout", 30))
    if result.returncode:
        raise RuntimeError(f"{key} failed with exit {result.returncode}")
    return result.stdout


def validName(name):
    """Constrain generated names before placing them in filenames or BIND text."""
    return len(name) <= 253 and all(re.fullmatch(r"[A-Za-z0-9_-]{1,63}", label)
                                   for label in name.split("."))


def zoneBlocks(text):
    """Locate zone declarations in named-checkconf's expanded canonical output.

    Quoted strings are skipped while counting nested braces. Views are refused
    because ownership would need a separate manifest for each view.
    """
    if re.search(r"(?m)^\s*view\s", text):
        raise ValueError("BIND views require an explicit per-view ownership design")
    blocks = []
    for match in ZONE.finditer(text):
        offset, depth, quoted, escaped = match.end(), 1, False, False
        while offset < len(text) and depth:
            char = text[offset]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
            offset += 1
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if depth or offset >= len(text) or text[offset] != ";":
            raise ValueError("invalid canonical zone declaration")
        blocks.append((match[1].rstrip(".").lower(), match.start(), offset + 1))
    if len({name for name, _, _ in blocks}) != len(blocks):
        raise ValueError("duplicate zone origins in canonical configuration")
    return blocks


def inspectZone(path, expectedUid=None):
    """Accept bounded regular zone data and extract a served-record check."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > 1024 * 1024:
            raise ValueError(f"candidate must be a regular zone file: {path.name}")
        if expectedUid is not None and info.st_uid != expectedUid:
            raise ValueError("candidate zone has an unexpected owner")
        content = source.read(1024 * 1024 + 1)
    if len(content) > 1024 * 1024:
        raise ValueError("candidate changed beyond the zone size limit")
    text = content.decode("utf-8")
    bare = path.name.startswith("db.bare-")
    origin = path.name[len("db.bare-") if bare else 3:].lower()
    if not validName(origin) or (bare and "." in origin):
        raise ValueError("invalid candidate zone filename")
    directives = re.findall(r"(?m)^\s*\$(\w+)", text)
    if any(value.upper() not in ("ORIGIN", "TTL") for value in directives):
        raise ValueError("candidate zone contains an unsupported directive")
    origins = re.findall(r"(?mi)^\s*\$ORIGIN\s+(\S+)", text)
    if len(origins) != 1 or origins[0].rstrip(".").lower() != origin:
        raise ValueError("zone filename and explicit origin disagree")
    soa = SOA.search(text)
    if soa is None or int(soa[1]) > 0xffffffff:
        raise ValueError("invalid candidate SOA serial")
    record = re.search(r"(?m)^(\S+)\s+IN\s+(A|PTR)\s+(\S+)\s*$", text)
    if record is None:
        raise ValueError("candidate needs an A or PTR record to verify after reload")
    owner, kind, value = record.groups()
    checkName = origin if owner == "@" else (owner.rstrip(".") if owner.endswith(".") else owner + "." + origin)
    if not validName(checkName):
        raise ValueError("invalid check record owner")
    if kind == "A":
        value = str(ipaddress.IPv4Address(value))
    elif not validName(value.rstrip(".")):
        raise ValueError("invalid PTR record")
    return {"name": origin, "file": path.name, "bare": bare,
            "serial": int(soa[1]), "sha256": hashlib.sha256(content).hexdigest(),
            "checkName": checkName, "checkType": kind, "checkValue": value}, content


def loadManifest(directory):
    """Reject modified immutable generations before activation or rollback."""
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("version") != 1 or not re.fullmatch(r"[0-9a-f]{64}", directory.name):
        raise ValueError("invalid generation manifest")
    identity = {key: value for key, value in manifest.items() if key != "generation"}
    digest = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    if digest != directory.name or manifest.get("generation") != digest:
        raise ValueError("generation manifest checksum mismatch")
    for zone in manifest["zones"]:
        if pathlib.Path(zone["file"]).name != zone["file"]:
            raise ValueError("invalid manifest filename")
        info, _ = inspectZone(directory / "zones" / zone["file"])
        if info != zone:
            raise ValueError("immutable zone checksum mismatch")
    if (directory / "named.conf.managed").read_text() != configurationText(manifest):
        raise ValueError("immutable managed configuration mismatch")
    return manifest


def configurationText(manifest):
    """Generate declarations from validated data, never execute candidate config."""
    lines = []
    for zone in manifest["zones"]:
        file = f'{manifest["runtimeRoot"]}/{manifest["generation"]}/zones/{zone["file"]}'
        if zone["bare"]:
            policy = "allow-transfer { none; }; notify no;"
        else:
            addresses = "; ".join(manifest["transfer"])
            policy = (f"allow-transfer {{ {addresses}; }}; also-notify {{ {addresses}; }}; notify yes;"
                      if addresses else "allow-transfer { none; }; notify no;")
        lines.append(f'zone "{zone["name"]}" {{ type primary; file "{file}"; {policy} }};')
    return "\n".join(lines) + "\n"


def stageGeneration(config, candidate, trusted=False):
    """Copy complete candidate data into a content-addressed immutable release."""
    root = pathlib.Path(config["releaseRoot"])
    root.mkdir(parents=True, exist_ok=True)
    paths = sorted(pathlib.Path(candidate).glob("db.*"))
    if not paths or len(paths) > 2048:
        raise ValueError("candidate has an invalid zone count")
    inspected = [inspectZone(path, None if trusted else config.get("_candidateUid")) for path in paths]
    zones = [item[0] for item in inspected]
    if len({zone["name"] for zone in zones}) != len(zones):
        raise ValueError("candidate contains duplicate origins")
    runtimeRoot = config.get("runtimeRoot", str(root))
    if not re.fullmatch(r"/[A-Za-z0-9_./-]+", runtimeRoot):
        raise ValueError("invalid runtime release path")
    transfer = [str(ipaddress.ip_address(address)) for address in config.get("transfer", [])]
    manifest = {"version": 1, "runtimeRoot": runtimeRoot, "transfer": transfer, "zones": zones}
    generation = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    manifest["generation"] = generation
    destination = root / generation
    if destination.exists():
        if loadManifest(destination) != manifest or (destination / "named.conf.managed").read_text() != configurationText(manifest):
            raise ValueError("existing generation has been modified")
        return destination, manifest
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".stage-", dir=root))
    try:
        (temporary / "zones").mkdir(mode=0o755)
        for zone, content in inspected:
            path = temporary / "zones" / zone["file"]
            with path.open("wb") as output:
                output.write(content)
                output.flush()
                os.fsync(output.fileno())
            path.chmod(0o644)
        (temporary / "named.conf.managed").write_text(configurationText(manifest), encoding="utf-8")
        writeJson(temporary / "manifest.json", manifest)
        (temporary / "manifest.json").chmod(0o644)
        (temporary / "named.conf.managed").chmod(0o644)
        for path in (temporary / "named.conf.managed", temporary / "manifest.json"):
            with path.open("rb") as source:
                os.fsync(source.fileno())
        syncDirectory(temporary / "zones")
        syncDirectory(temporary)
        temporary.chmod(0o755)
        os.replace(temporary, destination)
        syncDirectory(root)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return destination, manifest


def newerSerial(candidate, previous):
    """RFC 1982 ordering, including the 32-bit wrap boundary."""
    return 0 < ((candidate - previous) & 0xffffffff) < 0x80000000


def normalizedGeneration(config, candidate, referencePath, reference, state, trusted=False):
    """Allocate from durable high-water marks, including removed/re-added zones.

    The reference supplies the currently intended content. Unchanged data keeps
    its serial; changed/reintroduced data must be newer than every attempted
    activation, including one a secondary may have transferred before failure.
    """
    root = pathlib.Path(config["releaseRoot"])
    highWater = dict(state.get("serials", {}))
    prior = {zone["name"]: zone for zone in reference["zones"]}
    for zone in reference["zones"]:
        old = highWater.get(zone["name"])
        if old is None or newerSerial(zone["serial"], old):
            highWater[zone["name"]] = zone["serial"]
    with tempfile.TemporaryDirectory(prefix=".normalize-", dir=root) as folder:
        folder = pathlib.Path(folder)
        paths = sorted(pathlib.Path(candidate).glob("db.*"))
        if not paths or len(paths) > 2048:
            raise ValueError("candidate has an invalid zone count")
        for path in paths:
            zone, content = inspectZone(path, None if trusted else config.get("_candidateUid"))
            text = content.decode("utf-8")
            old = prior.get(zone["name"])
            identical = False
            if old:
                oldText = (referencePath / "zones" / old["file"]).read_text()
                canonical = lambda value: SOA.sub(lambda m: m[0][:-len(m[1])] + "SERIAL", value, count=1)
                identical = canonical(text) == canonical(oldText)
            ceiling = highWater.get(zone["name"])
            if ceiling is not None and (not isinstance(ceiling, int) or not 0 <= ceiling <= 0xffffffff):
                raise ValueError("invalid serial high-water state")
            serial = old["serial"] if identical else zone["serial"]
            if ceiling is not None and serial != ceiling and not newerSerial(serial, ceiling):
                serial = (ceiling + 1) & 0xffffffff
            elif not identical and ceiling is not None and serial == ceiling:
                serial = (ceiling + 1) & 0xffffffff
            text = SOA.sub(lambda m: m[0][:-len(m[1])] + str(serial), text, count=1)
            (folder / path.name).write_text(text, encoding="utf-8")
        destination, manifest = stageGeneration(config, folder, trusted=True)
    return destination, manifest


def rememberSerials(state, manifest):
    """Record every serial that could be served before changing the pointer."""
    serials = dict(state.get("serials", {}))
    for zone in manifest["zones"]:
        previous = serials.get(zone["name"])
        if previous is None or newerSerial(zone["serial"], previous):
            serials[zone["name"]] = zone["serial"]
    state["serials"] = serials


def rollbackGeneration(config, state):
    """Restore previous records with forward-only serials for AXFR secondaries."""
    root = pathlib.Path(config["releaseRoot"])
    for key in ("previous", "desired"):
        if not re.fullmatch(r"[0-9a-f]{64}", state.get(key, "")):
            raise ValueError("invalid rollback journal generation")
    if state.get("rollback"):
        if not re.fullmatch(r"[0-9a-f]{64}", state["rollback"]):
            raise ValueError("invalid rollback generation")
        directory = root / state["rollback"]
        return directory, loadManifest(directory)
    previousPath = root / state["previous"]
    desiredPath = root / state["desired"]
    rememberSerials(state, loadManifest(previousPath))
    desired = loadManifest(desiredPath)
    directory, manifest = normalizedGeneration(config, previousPath / "zones",
                                               desiredPath, desired, state, trusted=True)
    state["rollback"] = directory.name
    rememberSerials(state, manifest)
    writeJson(root / "state.json", state)
    return directory, manifest


def currentGeneration(config):
    """Require a seeded current link confined to the configured release root."""
    link = pathlib.Path(config["currentLink"])
    if not link.is_symlink():
        raise ValueError("current link must be seeded before include cutover and activation")
    current = link.resolve(strict=True)
    if current.parent != pathlib.Path(config["releaseRoot"]).resolve():
        raise ValueError("current generation escapes release root")
    return current, loadManifest(current)


def switchLink(config, destination):
    """Atomically replace the one activation pointer and sync its parent."""
    link = pathlib.Path(config["currentLink"])
    temporary = link.parent / ("." + link.name + "-" + os.urandom(8).hex())
    try:
        temporary.symlink_to(destination)
        os.replace(temporary, link)
        syncDirectory(link.parent)
    finally:
        if temporary.is_symlink():
            temporary.unlink()


def validateGeneration(config, candidate, previous):
    """Check each zone and the complete desired config before changing current."""
    for zone in candidate["zones"]:
        file = f'{candidate["runtimeRoot"]}/{candidate["generation"]}/zones/{zone["file"]}'
        runCommand(config, "checkZoneCommand", [zone["name"], file])
    expanded = runCommand(config, "expandCommand")
    oldNames = {zone["name"] for zone in previous["zones"]}
    desiredNames = {zone["name"] for zone in candidate["zones"]}
    blocks = zoneBlocks(expanded)
    present = {name for name, _, _ in blocks}
    if not oldNames <= present or desiredNames & (present - oldNames):
        raise ValueError("managed ownership is missing or collides with an unmanaged zone")
    for name, start, end in reversed(blocks):
        if name in oldNames:
            expanded = expanded[:start] + expanded[end:]
    root = pathlib.Path(config["releaseRoot"])
    fd, temporary = tempfile.mkstemp(prefix=".validation-", suffix=".conf", dir=root)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            output.write(expanded + "\n" + configurationText(candidate))
        runtime = candidate["runtimeRoot"] + "/" + pathlib.Path(temporary).name
        runCommand(config, "checkConfigCommand", [runtime])
    finally:
        os.unlink(temporary)                    # may contain expanded TSIG keys


def verifyServed(config, manifest):
    """Require authoritative SOAs and one expected A/PTR record per zone."""
    deadline = time.monotonic() + config.get("healthTimeout", 30)
    command = config.get("digCommand", ["dig"])
    common = ["@" + config.get("queryAddress", "127.0.0.1"), "-p",
              str(config.get("queryPort", 53)), "+norecurse", "+noall", "+comments",
              "+answer", "+time=1", "+tries=1"]
    for zone in manifest["zones"]:
        for name, kind, expected in ((zone["name"], "SOA", str(zone["serial"])),
                                     (zone["checkName"], zone["checkType"], zone["checkValue"])):
            while True:
                result = subprocess.run(command + common + [name, kind], capture_output=True,
                                        text=True, timeout=3)
                authoritative = re.search(r"flags:[^;]*\baa\b", result.stdout)
                answers = [line.split() for line in result.stdout.splitlines()
                           if line and not line.startswith(";")]
                values = [row[6] if kind == "SOA" else row[4]
                          for row in answers if len(row) >= (7 if kind == "SOA" else 5)
                          and row[3] == kind]
                if result.returncode == 0 and authoritative and expected in values:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"served-data verification failed: {name} {kind}")
                time.sleep(0.2)


def recoverPending(config, health=verifyServed):
    """Restore the previous generation after a crash between switch and proof."""
    root = pathlib.Path(config["releaseRoot"])
    path = root / "state.json"
    state = json.loads(path.read_text()) if path.exists() else {}
    if state.get("phase") not in ("pending", "rollbackFailed"):
        return state
    if not re.fullmatch(r"[0-9a-f]{64}", state.get("previous", "")):
        raise ValueError("invalid recovery generation")
    previous, manifest = rollbackGeneration(config, state)
    _, observed = currentGeneration(config)
    validateGeneration(config, manifest, observed)
    switchLink(config, previous)
    runCommand(config, "reloadCommand")
    health(config, manifest)
    state.update(phase="recovered", applied=previous.name, error="interrupted activation rolled back")
    writeJson(path, state)
    return state


def activate(config, candidate, health=verifyServed):
    """Serialize, prevalidate, switch, reload, prove serving, or restore previous."""
    root = pathlib.Path(config["releaseRoot"])
    root.mkdir(parents=True, exist_ok=True)
    with (root / ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        bootstrapPath = root / "bootstrap.json"
        if bootstrapPath.exists() and json.loads(bootstrapPath.read_text()).get("phase") != "applied":
            raise RuntimeError("legacy include cutover requires bootstrap recovery first")
        state = recoverPending(config, health)
        previousPath, previous = currentGeneration(config)
        destination, desired = normalizedGeneration(config, candidate, previousPath, previous, state)
        validateGeneration(config, desired, previous)
        rememberSerials(state, previous)
        rememberSerials(state, desired)
        state.pop("rollback", None)
        state.update(phase="pending", desired=destination.name, previous=previousPath.name)
        writeJson(root / "state.json", state)
        try:
            switchLink(config, destination)
            runCommand(config, "reloadCommand")
            health(config, desired)
        except Exception as failure:
            try:
                rollbackPath, rollbackManifest = rollbackGeneration(config, state)
                _, observed = currentGeneration(config)
                validateGeneration(config, rollbackManifest, observed)
                switchLink(config, rollbackPath)
                runCommand(config, "reloadCommand")
                health(config, rollbackManifest)
            except Exception as rollback:
                state.update(phase="rollbackFailed", error=str(rollback))
                writeJson(root / "state.json", state)
                raise RuntimeError("activation and rollback failed; inspect private target state") from failure
            state.update(phase="failed", applied=rollbackPath.name, error=str(failure))
            writeJson(root / "state.json", state)
            raise
        state.update(phase="applied", applied=destination.name, error=None,
                     lastSuccess=datetime.datetime.now(datetime.timezone.utc).isoformat())
        writeJson(root / "state.json", state)
        return state


def readRegular(path):
    """Read a bounded config/zone snapshot without following a final symlink."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > 4 * 1024 * 1024:
            raise ValueError("legacy input must be a bounded regular file")
        content = source.read(4 * 1024 * 1024 + 1)
    if len(content) > 4 * 1024 * 1024:
        raise ValueError("legacy input changed beyond its size limit")
    return content, info


def writeAtomicFile(path, content, mode=0o600, uid=None, gid=None):
    """Replace a config while preserving its recorded ownership and mode."""
    fd, temporary = tempfile.mkstemp(prefix=".cutover-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(content)
            output.flush()
            if uid is not None:
                os.fchown(output.fileno(), uid, gid)
            os.fchmod(output.fileno(), mode)
            os.fsync(output.fileno())
        os.replace(temporary, path)
        syncDirectory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def restoreBootstrap(config, journal, health=verifyServed):
    """Restore the exact legacy parent include after an interrupted cutover."""
    root = pathlib.Path(config["releaseRoot"])
    if not re.fullmatch(r"[0-9a-f]{64}", journal.get("generation", "")):
        raise ValueError("invalid bootstrap journal generation")
    backup, _ = readRegular(root / ".bootstrap-parent.backup")
    if hashlib.sha256(backup).hexdigest() != journal["beforeHash"]:
        raise ValueError("bootstrap backup checksum mismatch")
    writeAtomicFile(pathlib.Path(config["includeParent"]), backup,
                    journal["mode"], journal["uid"], journal["gid"])
    runCommand(config, "reloadCommand")
    manifest = loadManifest(root / journal["generation"])
    health(config, manifest)
    link = pathlib.Path(config["currentLink"])
    if link.is_symlink() and link.resolve() == root / journal["generation"]:
        link.unlink()
        syncDirectory(link.parent)
    elif link.exists() or link.is_symlink():
        raise ValueError("bootstrap recovery found an unrelated current pointer")
    journal.update(phase="recovered", error="legacy include restored")
    writeJson(root / "bootstrap.json", journal)
    return journal


def bootstrap(config, health=verifyServed):
    """Seed an equivalent baseline, then atomically convert the legacy includes.

    Stop the legacy copy-based writer first. Existing legacy files stay intact;
    only their parent include is replaced, with a private durable backup.
    """
    root = pathlib.Path(config["releaseRoot"])
    root.mkdir(parents=True, exist_ok=True)
    with (root / ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        journalPath = root / "bootstrap.json"
        journal = json.loads(journalPath.read_text()) if journalPath.exists() else {}
        if journal.get("phase") in ("pending", "rollbackFailed"):
            restoreBootstrap(config, journal, health)
        runtimeCurrent = config.get("runtimeCurrentLink", config["currentLink"])
        if not re.fullmatch(r"/[A-Za-z0-9_./-]+", runtimeCurrent):
            raise ValueError("invalid runtime current path")
        include = f'include "{runtimeCurrent}/named.conf.managed";\n'
        parent = pathlib.Path(config["includeParent"])
        parentBytes, parentInfo = readRegular(parent)
        parentText = parentBytes.decode("utf-8")
        if journal.get("phase") == "applied":
            if include.strip() not in parentText:
                raise ValueError("completed bootstrap include has drifted")
            _, current = currentGeneration(config)
            health(config, current)
            return journal
        link = pathlib.Path(config["currentLink"])
        if link.exists() or link.is_symlink():
            raise ValueError("bootstrap refuses an existing unmanaged current pointer")
        sourceRoot = pathlib.Path(config["legacyZoneRoot"])
        runtimeRoot = pathlib.PurePosixPath(config.get("runtimeLegacyZoneRoot", str(sourceRoot)))
        desiredParent = parentText
        snapshots = {parent: hashlib.sha256(parentBytes).hexdigest()}
        with tempfile.TemporaryDirectory(prefix=".bootstrap-", dir=root) as temporary:
            temporary = pathlib.Path(temporary)
            origins = set()
            for item in config["legacyIncludes"]:
                pattern = re.compile(r'(?m)^[ \t]*include\s+"' + re.escape(item["runtimePath"]) +
                                     r'"\s*;[ \t]*(?://[^\n]*)?(?:\n|$)')
                desiredParent, count = pattern.subn("", desiredParent)
                if count != 1:
                    raise ValueError("legacy include must occur exactly once in its parent")
                declarations, _ = readRegular(pathlib.Path(item["hostPath"]))
                snapshots[pathlib.Path(item["hostPath"])] = hashlib.sha256(declarations).hexdigest()
                text = declarations.decode("utf-8")
                for name, start, end in zoneBlocks(text):
                    if name in origins:
                        raise ValueError("legacy includes contain duplicate managed origins")
                    origins.add(name)
                    block = text[start:end]
                    if not re.search(r"\btype\s+(?:primary|master)\s*;", block):
                        raise ValueError("legacy managed include contains a non-primary zone")
                    files = re.findall(r'\bfile\s+"([^"]+)"\s*;', block)
                    if len(files) != 1:
                        raise ValueError("legacy zone must reference exactly one file")
                    relative = pathlib.PurePosixPath(files[0]).relative_to(runtimeRoot)
                    if len(relative.parts) != 1 or relative.name in (".", ".."):
                        raise ValueError("legacy zone file escapes its configured directory")
                    content, _ = readRegular(sourceRoot / relative.name)
                    snapshots[sourceRoot / relative.name] = hashlib.sha256(content).hexdigest()
                    zoneText = content.decode("utf-8")
                    if not re.search(r"(?mi)^\s*\$ORIGIN\s", zoneText):
                        zoneText = f"$ORIGIN {name}.\n" + zoneText
                    (temporary / relative.name).write_text(zoneText, encoding="utf-8")
            if not origins:
                raise ValueError("legacy include set contains no managed zones")
            destination, baseline = stageGeneration(config, temporary, trusted=True)
        if {zone["name"] for zone in baseline["zones"]} != origins:
            raise ValueError("legacy filenames do not match their declared origins")
        desiredParent = (desiredParent.rstrip() + "\n" + include).encode("utf-8")
        validateGeneration(config, baseline, baseline)
        health(config, baseline)                # prove the snapshot matches legacy serving
        for path, expected in snapshots.items():
            observed, _ = readRegular(path)
            if hashlib.sha256(observed).hexdigest() != expected:
                raise ValueError("legacy inputs changed during cutover preparation; stop the old writer")
        writeAtomicFile(root / ".bootstrap-parent.backup", parentBytes)
        journal = {"phase": "pending", "generation": destination.name,
                   "beforeHash": hashlib.sha256(parentBytes).hexdigest(),
                   "afterHash": hashlib.sha256(desiredParent).hexdigest(),
                   "mode": stat.S_IMODE(parentInfo.st_mode), "uid": parentInfo.st_uid,
                   "gid": parentInfo.st_gid}
        writeJson(journalPath, journal)
        try:
            switchLink(config, destination)
            writeAtomicFile(parent, desiredParent, journal["mode"], journal["uid"], journal["gid"])
            validateGeneration(config, baseline, baseline)
            runCommand(config, "reloadCommand")
            health(config, baseline)
        except Exception as failure:
            try:
                restoreBootstrap(config, journal, health)
            except Exception as rollback:
                journal.update(phase="rollbackFailed", error=str(rollback))
                writeJson(journalPath, journal)
                raise RuntimeError("bootstrap and legacy restore failed") from failure
            raise
        state = {"phase": "applied", "desired": destination.name, "applied": destination.name,
                 "lastSuccess": datetime.datetime.now(datetime.timezone.utc).isoformat(), "error": None}
        rememberSerials(state, baseline)
        writeJson(root / "state.json", state)
        journal.update(phase="applied", error=None)
        writeJson(journalPath, journal)
        return journal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", nargs="?")
    parser.add_argument("--bootstrap", action="store_true",
                        help="convert configured legacy includes after stopping the old writer")
    parser.add_argument("--config", default="/etc/solarinet/dns-release.json")
    args = parser.parse_args()
    fd = os.open(args.config, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "r", encoding="utf-8") as source:
        info = os.fstat(source.fileno())
        if os.geteuid() != 0 or info.st_uid != 0 or info.st_mode & 0o022:
            parser.error("release helper requires root and a root-owned non-writable configuration")
        config = json.load(source)
    for value in (config["releaseRoot"], str(pathlib.Path(config["currentLink"]).parent)):
        directory = pathlib.Path(value)
        directory.mkdir(parents=True, exist_ok=True)
        info = directory.stat()
        if directory.is_symlink() or info.st_uid != 0 or info.st_mode & 0o022:
            parser.error("release directories must be root-owned and not writable by other accounts")
    if args.bootstrap:
        if args.candidate:
            parser.error("bootstrap does not accept a candidate directory")
        _, info = readRegular(pathlib.Path(config["includeParent"]))
        if info.st_uid != 0 or info.st_mode & 0o022:
            parser.error("legacy include parent must be root-owned and not writable by other accounts")
        result = bootstrap(config)
    else:
        if not args.candidate:
            parser.error("activation requires a candidate directory")
        incoming = pathlib.Path(config["incomingRoot"]).resolve(strict=True)
        candidate = pathlib.Path(args.candidate).resolve(strict=True)
        if candidate == incoming or not candidate.is_relative_to(incoming):
            parser.error("candidate must be a private generation inside incomingRoot")
        config["_candidateUid"] = pwd.getpwnam(config["candidateUser"]).pw_uid
        info = candidate.stat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != config["_candidateUid"] or info.st_mode & 0o077:
            parser.error("candidate directory must be private and owned by candidateUser")
        result = activate(config, candidate)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
