#!/usr/bin/env python3
"""Build static, public-only SolariNet CA trust enrollment artifacts.

The input is a CA root certificate (PEM or DER). No CA private key is read.
The output directory contains a PEM root, an Apple configuration profile,
checksums, and a self-contained HTML page. These files are build artifacts and
are not served or deployed by this program.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import html
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path


PROFILE_NAMESPACE = uuid.UUID("4f487254-b903-4e02-9ef9-3a691e742685")
PORTAL_FILES = (
    "root_ca.pem",
    "akoria-root.mobileconfig",
    "firefox-policies.json",
    "SHA256SUMS",
    "index.html",
)


class TrustPortalError(Exception):
    pass


def openssl(*args: str, input_data: bytes | None = None) -> bytes:
    try:
        result = subprocess.run(
            ["openssl", *args], check=True, capture_output=True, input=input_data, timeout=15
        )
    except FileNotFoundError as exc:
        raise TrustPortalError("openssl is required to validate the CA certificate") from exc
    except subprocess.TimeoutExpired as exc:
        raise TrustPortalError("openssl timed out while validating the CA certificate") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", "replace").strip()
        raise TrustPortalError(f"openssl command failed while processing the CA certificate: {detail}") from exc
    return result.stdout


def certificate_details(cert_path: Path) -> dict[str, str | bytes]:
    try:
        if cert_path.stat().st_size > 1024 * 1024:
            raise TrustPortalError("CA certificate exceeds the 1 MiB input limit")
        certificate_bytes = cert_path.read_bytes()
    except OSError as exc:
        raise TrustPortalError(f"cannot read CA certificate: {exc}") from exc
    return certificate_details_from_bytes(certificate_bytes)


def certificate_details_from_bytes(certificate_bytes: bytes) -> dict[str, str | bytes]:
    if len(certificate_bytes) > 1024 * 1024:
        raise TrustPortalError("CA certificate exceeds the 1 MiB input limit")
    prefix = certificate_bytes[:64].lstrip()
    inform = "PEM" if prefix.startswith(b"-----BEGIN CERTIFICATE-----") else "DER"
    if inform == "PEM":
        if certificate_bytes.count(b"-----BEGIN CERTIFICATE-----") != 1 or certificate_bytes.count(b"-----END CERTIFICATE-----") != 1:
            raise TrustPortalError("input must contain exactly one PEM certificate")
        cert_end = certificate_bytes.find(b"-----END CERTIFICATE-----") + len(b"-----END CERTIFICATE-----")
        if certificate_bytes[cert_end:].strip():
            raise TrustPortalError("unexpected data follows the PEM certificate")
    x509 = ("x509", "-inform", inform)
    pem = openssl(*x509, "-outform", "PEM", input_data=certificate_bytes)
    der = openssl(*x509, "-outform", "DER", input_data=certificate_bytes)
    if inform == "DER" and der != certificate_bytes:
        raise TrustPortalError("input must contain exactly one DER certificate with no trailing data")
    basic_constraints = openssl(
        *x509, "-noout", "-ext", "basicConstraints", input_data=certificate_bytes
    ).decode("utf-8", "replace")
    if not re.search(r"CA\s*:\s*TRUE", basic_constraints, flags=re.IGNORECASE):
        raise TrustPortalError("input certificate is not a CA (basicConstraints CA:TRUE missing)")
    certificate_text = openssl(*x509, "-noout", "-text", input_data=certificate_bytes).decode("utf-8", "replace")
    certificate_lines = certificate_text.splitlines()
    key_usage = ""
    for index, line in enumerate(certificate_lines):
        if "X509v3 Key Usage:" in line:
            key_usage = line.split(":", 1)[1] + (
                certificate_lines[index + 1] if index + 1 < len(certificate_lines) else ""
            )
            break
    if key_usage and not re.search(r"Certificate Sign", key_usage, flags=re.IGNORECASE):
        raise TrustPortalError("CA certificate keyUsage does not permit certificate signing")

    name_options = ("-nameopt", "RFC2253,utf8,-esc_msb")
    subject = openssl(*x509, "-noout", "-subject", *name_options, input_data=certificate_bytes)
    issuer = openssl(*x509, "-noout", "-issuer", *name_options, input_data=certificate_bytes)
    subject_text = subject.decode("utf-8", "replace").strip().removeprefix("subject=")
    issuer_text = issuer.decode("utf-8", "replace").strip().removeprefix("issuer=")
    if subject_text != issuer_text:
        raise TrustPortalError("input certificate is an intermediate, not a self-issued root")
    try:
        with tempfile.TemporaryDirectory(prefix="solarinet-root-verify-") as verify_dir:
            verify_cert = Path(verify_dir) / "root.pem"
            verify_cert.write_bytes(pem)
            openssl(
                "verify", "-check_ss_sig", "-no_check_time", "-no-CApath", "-no-CAstore",
                "-CAfile", str(verify_cert), str(verify_cert),
            )
    except TrustPortalError as exc:
        raise TrustPortalError(f"could not verify root self-signature: {exc}") from exc
    except OSError as exc:
        raise TrustPortalError(f"cannot prepare temporary root verification: {exc}") from exc

    dates = openssl(*x509, "-noout", "-startdate", "-enddate", input_data=certificate_bytes)
    fingerprint = openssl(*x509, "-noout", "-fingerprint", "-sha256", input_data=certificate_bytes)
    date_lines = dict(line.split("=", 1) for line in dates.decode().splitlines() if "=" in line)
    try:
        not_before_dt = datetime.strptime(date_lines["notBefore"], "%b %d %H:%M:%S %Y GMT").replace(tzinfo=timezone.utc)
        not_after_dt = datetime.strptime(date_lines["notAfter"], "%b %d %H:%M:%S %Y GMT").replace(tzinfo=timezone.utc)
    except (KeyError, ValueError) as exc:
        raise TrustPortalError("could not parse CA certificate validity dates") from exc
    fingerprint_text = fingerprint.decode("ascii").strip().split("=", 1)[-1].upper()
    fingerprint_text = re.sub(r"[^0-9A-F]", "", fingerprint_text)
    if not re.fullmatch(r"[0-9A-F]{64}", fingerprint_text):
        raise TrustPortalError("could not parse a SHA-256 fingerprint from openssl")
    fingerprint_colon = ":".join(fingerprint_text[i:i+2] for i in range(0, 64, 2))
    now = datetime.now(timezone.utc)
    identity = f"subject {subject_text}; SHA-256 {fingerprint_colon}"
    if not_before_dt > now:
        raise TrustPortalError(
            f"CA certificate is not valid until {date_lines['notBefore']} ({identity})"
        )
    if not_after_dt <= now:
        raise TrustPortalError(
            f"CA certificate expired at {date_lines['notAfter']} ({identity})"
        )
    return {
        "pem": pem,
        "der": der,
        "subject": subject_text,
        "issuer": issuer_text,
        "not_before": date_lines.get("notBefore", "unknown"),
        "not_after": date_lines.get("notAfter", "unknown"),
        "fingerprint_hex": fingerprint_text,
        "fingerprint_colon": fingerprint_colon,
    }


def build_profile(details: dict[str, str | bytes]) -> bytes:
    fingerprint = str(details["fingerprint_hex"]).lower()
    profile_uuid = str(uuid.uuid5(PROFILE_NAMESPACE, f"solarinet-root:{fingerprint}")).upper()
    cert_uuid = str(uuid.uuid5(PROFILE_NAMESPACE, f"solarinet-root-cert:{fingerprint}")).upper()
    cert_payload = {
        "PayloadType": "com.apple.security.root",
        "PayloadVersion": 1,
        "PayloadIdentifier": f"net.akoria.pki.root.{fingerprint}",
        "PayloadUUID": cert_uuid,
        "PayloadDisplayName": "Akoria Internal Root CA",
        "PayloadDescription": "Public root certificate for SolariNet internal HTTPS services.",
        "PayloadContent": details["der"],
    }
    profile = {
        "PayloadType": "Configuration",
        "PayloadVersion": 1,
        "PayloadIdentifier": f"net.akoria.pki.trust.{fingerprint}",
        "PayloadUUID": profile_uuid,
        "PayloadDisplayName": "Akoria Internal Root CA",
        "PayloadDescription": "Adds the Akoria internal root certificate. Review the fingerprint before installation and explicitly enable full trust in Settings.",
        "PayloadOrganization": "Akoria / SolariNet",
        "PayloadRemovalDisallowed": False,
        "PayloadContent": [cert_payload],
    }
    return plistlib.dumps(profile, fmt=plistlib.FMT_XML, sort_keys=False)


def build_firefox_policy() -> bytes:
    policy = {
        "policies": {
            "Certificates": {
                "Install": ["/etc/ssl/solarinet/akoria-root.pem"]
            }
        }
    }
    return (json.dumps(policy, indent=2) + "\n").encode("utf-8")


def build_page(details: dict[str, str | bytes]) -> bytes:
    fp = html.escape(str(details["fingerprint_colon"]))
    subject = html.escape(str(details["subject"]))
    issuer = html.escape(str(details["issuer"]))
    not_after = html.escape(str(details["not_after"]))
    page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; img-src data:; base-uri 'none'; form-action 'none'">
<meta name="description" content="Install the Akoria internal CA root for SolariNet HTTPS services.">
<title>SolariNet device trust</title>
<style>
:root{{color-scheme:light dark;--bg:#101820;--panel:#1c2a34;--ink:#f2f6f8;--muted:#b6c4cc;--line:#3a4c58;--accent:#65d1c4;--warn:#ffd17a}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--ink);font:16px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif}}
main{{max-width:780px;margin:auto;padding:28px 18px 48px}}header{{display:flex;align-items:center;gap:14px;border-bottom:1px solid var(--line);padding-bottom:20px}}
header svg{{width:50px;height:50px;flex:none}}h1{{font-size:1.55rem;line-height:1.2;margin:0}}h2{{font-size:1.08rem;margin:0 0 8px}}p{{margin:8px 0}}.muted{{color:var(--muted)}}
.box{{margin:18px 0;padding:17px;border:1px solid var(--line);border-radius:12px;background:var(--panel)}}.warning{{border-left:5px solid var(--warn)}}
.fingerprint{{font:600 .91rem/1.6 ui-monospace,SFMono-Regular,Consolas,monospace;overflow-wrap:anywhere;color:var(--accent)}}
.links{{display:flex;flex-wrap:wrap;gap:10px;margin-top:12px}}a.button{{display:inline-block;padding:10px 14px;border-radius:7px;background:var(--accent);color:#071916;font-weight:700;text-decoration:none}}
a{{color:var(--accent)}}a:focus-visible{{outline:3px solid var(--warn);outline-offset:3px}}ol{{padding-left:24px}}li{{margin:8px 0}}code,pre{{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;overflow-wrap:anywhere}}pre{{padding:12px;background:#0b1116;border-radius:7px;white-space:pre-wrap}}
@media(prefers-reduced-motion:reduce){{*{{scroll-behavior:auto!important}}}}
</style>
</head>
<body><main>
<header><svg viewBox="0 0 64 64" role="img" aria-label="Certificate shield"><path d="M32 4 54 12v17c0 14-9 24-22 31C19 53 10 43 10 29V12z" fill="#1c2a34" stroke="#65d1c4" stroke-width="3"/><path d="m21 32 7 7 16-17" fill="none" stroke="#65d1c4" stroke-width="4" stroke-linecap="round" stroke-linejoin="round"/></svg><div><h1>SolariNet device trust</h1><div class="muted">Akoria Internal Root CA</div></div></header>
<section class="box warning" aria-labelledby="first-use"><h2 id="first-use">First-time setup needs a separate trusted step</h2><p>This page cannot establish its own TLS trust. Before installing the profile, verify the root fingerprint below on a trusted computer, then transfer the profile to iPhone/iPad by AirDrop or cable. After installing it, open <b>Settings → General → About → Certificate Trust Settings</b> and enable full trust for this root.</p><p>Do not install a profile merely because this page displayed it. The fingerprint is the identity check.</p></section>
<section class="box" aria-label="Profile signing note"><p>This self-service profile is unsigned. The device may label the profile as unverified; that is expected. Profile signing does not replace checking the CA root fingerprint.</p></section>
<section class="box" aria-labelledby="identity"><h2 id="identity">Root identity</h2><p><b>SHA-256 fingerprint</b></p><p class="fingerprint">{fp}</p><p><b>Subject:</b> {subject}<br><b>Issuer:</b> {issuer}<br><b>Expires:</b> {not_after}</p><p>Verify a downloaded PEM on a trusted computer with:<br><code>openssl x509 -noout -fingerprint -sha256 -in root_ca.pem</code></p><div class="links"><a class="button" href="akoria-root.mobileconfig" type="application/x-apple-aspen-config">Apple profile</a><a class="button" href="root_ca.pem" type="application/x-pem-file">Root PEM</a></div></section>
<section class="box" aria-labelledby="platform"><h2 id="platform">Install and verify</h2><ol><li><b>iPhone/iPad Safari and Firefox:</b> install the profile, then enable full trust in Certificate Trust Settings. Remove it from Settings → General → VPN &amp; Device Management.</li><li><b>macOS Safari:</b> install <code>root_ca.pem</code>, then run <code>sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain root_ca.pem</code>.</li><li><b>Zen / Firefox on macOS:</b> install the root in the System keychain, then make sure Firefox's “Allow Firefox to automatically trust third-party root certificates” setting is enabled. Restart and test each browser.</li><li><b>Firefox on Linux:</b> install the root at <code>/etc/ssl/solarinet/akoria-root.pem</code>. Firefox supports a system-wide policy at <code>/etc/firefox/policies/policies.json</code>; an installation-local policy can use <code>&lt;Firefox installation&gt;/distribution/policies.json</code>. Restart Firefox and confirm the policy is active in <code>about:policies</code>.</li><li><b>Firefox on Android:</b> install the root using Android's certificate settings, then verify with the installed Firefox version; some versions/settings may require an additional trust toggle.</li></ol><div class="links"><a class="button" href="firefox-policies.json" download="firefox-policies.json" type="application/json">Firefox Linux policy</a></div></section>
<footer class="muted"><p>Subject details: {subject}. Issuer: {issuer}. This package contains only the public CA root; it cannot issue certificates and contains no private key.</p></footer>
</main></body></html>
"""
    return page.encode("utf-8")


def artifacts_for_details(details: dict[str, str | bytes]) -> dict[str, bytes]:
    files = {
        "root_ca.pem": bytes(details["pem"]),
        "akoria-root.mobileconfig": build_profile(details),
        "firefox-policies.json": build_firefox_policy(),
        "index.html": build_page(details),
    }
    files["SHA256SUMS"] = "".join(
        f"{hashlib.sha256(payload).hexdigest()}  {name}\n"
        for name, payload in files.items()
    ).encode("ascii")
    return files


def write_artifacts(cert_path: Path, out_dir: Path) -> dict[str, str]:
    if not cert_path.is_file():
        raise TrustPortalError(f"CA certificate does not exist or is not a regular file: {cert_path}")
    details = certificate_details(cert_path)
    files = artifacts_for_details(details)

    try:
        if out_dir.is_symlink():
            raise TrustPortalError(f"refusing output directory symlink: {out_dir}")
        out_dir.parent.mkdir(parents=True, exist_ok=True)
        if out_dir.exists():
            raise TrustPortalError(f"refusing to overwrite existing trust portal output path: {out_dir}")
        staging = Path(tempfile.mkdtemp(prefix=f".{out_dir.name}.pki-stage-", dir=out_dir.parent))
    except OSError as exc:
        raise TrustPortalError(f"cannot prepare output directory: {exc}") from exc
    try:
        for name, payload in files.items():
            path = staging / name
            path.write_bytes(payload)
            path.chmod(0o644)
        staging.chmod(0o755)
        os.rename(staging, out_dir)
    except OSError as exc:
        raise TrustPortalError(f"cannot publish trust portal artifacts: {exc}") from exc
    finally:
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)
    return {
        "fingerprint": str(details["fingerprint_colon"]),
        "subject": str(details["subject"]),
        "issuer": str(details["issuer"]),
        "expires": str(details["not_after"]),
        "output": str(out_dir),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path, help="public CA root certificate in PEM or DER form")
    parser.add_argument("--output", required=True, type=Path, help="new directory for static artifacts; refuses to overwrite an existing path")
    args = parser.parse_args(argv)
    try:
        summary = write_artifacts(args.root, args.output)
    except TrustPortalError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"Built static trust artifacts in {summary['output']}")
    print(f"Subject: {summary['subject']}")
    print(f"Issuer: {summary['issuer']}")
    print(f"Expires: {summary['expires']}")
    print(f"SHA-256: {summary['fingerprint']}")
    print("No CA private key was read. Artifacts were not served or deployed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
