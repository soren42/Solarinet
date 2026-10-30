#!/usr/bin/env python3
"""Run the ACME lifecycle against a disposable, isolated step-ca and Apache.

This is an opt-in CI integration test. It starts no host service, exposes no
container port, and attaches test containers only to an internal Docker network.
"""
from __future__ import annotations

import json
import os
import pathlib
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timedelta, timezone
from typing import Sequence


STEP_CA_IMAGE = "smallstep/step-ca@sha256:a2b17872915c193259b75a5474c398326f41bd199f0842093e52cf4182bc8270"
STEP_CLI_IMAGE = "smallstep/step-cli@sha256:78c413a4e21cab323b7aa3b57fb060a41f517cb80bd0f1f2217ba1e6c8831c17"
ALLOWED_NAME = "acme-solarinet-ci.invalid"
DENIED_NAME = "unapproved-solarinet-ci.invalid"
CA_NAME = "ca.solarinet.test"
LINEAGE = "acme-solarinet-ci.invalid"
SERVICE_ID = "acme_web"
RUN_ID = os.environ.get("SOLARINET_ACME_RUN_ID") or secrets.token_hex(8)
if not all(ch.isalnum() or ch in "_.-" for ch in RUN_ID):
    raise SystemExit("SOLARINET_ACME_RUN_ID contains unsafe characters")
RESOURCE_LABEL = f"solarinet.acme-ci={RUN_ID}"
CERTBOT_IMAGE = f"solarinet-acme-certbot-apache:{RUN_ID}"
NETWORK_NAME = f"solarinet-acme-internal-{RUN_ID}"
CLIENT_NAME = f"solarinet-acme-client-{RUN_ID}"
CA_CONTAINER_NAME = f"solarinet-acme-ca-{RUN_ID}"
ROOT = pathlib.Path("/etc/ssl/solarinet")
ROOT_CERT = ROOT / "test-root.pem"
WEBROOT = pathlib.Path("/var/lib/solarinet/acme-webroot")
REGISTRY = pathlib.Path("/etc/solarinet/pki/acme-services.json")
TARGET = ROOT / "acme-web"
BOOTSTRAP_RELEASE = TARGET / "releases" / ("0" * 64)
HELPER = pathlib.Path("/usr/local/libexec/solarinet-pki/acme_certificates.py")


def run(args: Sequence[str], *, timeout: int = 120, check: bool = True,
        quiet: bool = False) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(args, check=False, text=True, capture_output=True,
                            stdin=subprocess.DEVNULL, timeout=timeout)
    if check and result.returncode:
        details = (result.stderr or result.stdout).strip().splitlines()[-12:]
        raise RuntimeError(f"{pathlib.Path(args[0]).name} failed ({result.returncode}): " +
                           "\n".join(details))
    if not quiet and result.stdout.strip():
        print(result.stdout.strip())
    return result


def docker(*args: str, timeout: int = 120, check: bool = True,
           quiet: bool = False) -> subprocess.CompletedProcess[str]:
    return run(["docker", *args], timeout=timeout, check=check, quiet=quiet)


def exec_client(*args: str, timeout: int = 120, check: bool = True,
                quiet: bool = False,
                env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    options = [part for key, value in (env or {}).items() for part in ("--env", f"{key}={value}")]
    return docker("exec", *options, CLIENT_NAME, *args, timeout=timeout,
                  check=check, quiet=quiet)


def copy_to_client(source: pathlib.Path, destination: pathlib.Path,
                   mode: str = "0600") -> None:
    docker("cp", str(source), f"{CLIENT_NAME}:{destination}")
    exec_client("chown", "root:root", str(destination))
    exec_client("chmod", mode, str(destination))


def wait_for_ca() -> None:
    for _ in range(60):
        result = exec_client("curl", "--fail", "--silent", "--show-error",
                             "--cacert", str(ROOT_CERT),
                             f"https://{CA_NAME}:9443/health", check=False,
                             quiet=True)
        if result.returncode == 0:
            print("Disposable step-ca health endpoint is trusted and reachable.")
            return
        time.sleep(1)
    raise RuntimeError("disposable step-ca did not become ready")


def cert_serial() -> str:
    result = exec_client("openssl", "x509", "-in", str(TARGET / "current/fullchain.pem"),
                         "-noout", "-serial")
    return result.stdout.strip()


def replace_current_pointer(release: str) -> None:
    exec_client(
        "/usr/local/bin/python3", "-c",
        "import os,pathlib,sys; current=pathlib.Path(sys.argv[1]); "
        "temp=current.with_name('.current-integration-test'); "
        "temp.unlink(missing_ok=True); temp.symlink_to(sys.argv[2]); os.replace(temp,current)",
        str(TARGET / "current"), release,
    )


def assert_https() -> None:
    result = exec_client("curl", "--fail", "--silent", "--show-error",
                         "--cacert", str(ROOT_CERT), f"https://{ALLOWED_NAME}/")
    if result.stdout.strip() != "solarinet-acme-integration-ok":
        raise RuntimeError("Apache did not serve the expected HTTPS response")


def write_ca_config(ca_home: pathlib.Path) -> None:
    path = ca_home / "config/ca.json"
    config = json.loads(path.read_text())
    config["address"] = ":9443"
    config["dnsNames"] = [CA_NAME]
    config.pop("password", None)
    provisioners = config["authority"]["provisioners"]
    acme = [item for item in provisioners if item.get("type") == "ACME"]
    if len(acme) != 1:
        raise RuntimeError("step ca init did not create exactly one ACME provisioner")
    acme[0]["challenges"] = ["http-01"]
    acme[0]["claims"] = {
        "minTLSCertDuration": "168h",
        "defaultTLSCertDuration": "168h",
        "maxTLSCertDuration": "720h",
    }
    config["authority"]["provisioners"] = acme
    config["authority"]["policy"] = {
        "x509": {"allow": {"dns": [ALLOWED_NAME]}}
    }
    path.write_text(json.dumps(config, indent=2) + "\n")
    written = json.loads(path.read_text())
    if written["authority"]["policy"]["x509"] != {"allow": {"dns": [ALLOWED_NAME]}}:
        raise RuntimeError("web CA policy must allow only the exact configured DNS name")
    written_provisioners = written["authority"]["provisioners"]
    if (len(written_provisioners) != 1 or written_provisioners[0].get("type") != "ACME"
            or written_provisioners[0].get("challenges") != ["http-01"]):
        raise RuntimeError("web CA must expose only ACME HTTP-01 issuance")


def step_cli(ca_home: pathlib.Path, *args: str) -> None:
    docker("run", "--rm", "--label", RESOURCE_LABEL, "--network", "none", "--user", "1000:1000",
           "--env", "STEPPATH=/home/step", "--volume", f"{ca_home}:/home/step",
           STEP_CLI_IMAGE, "step", *args, timeout=120, quiet=True)


def install_constrained_web_intermediate(ca_home: pathlib.Path) -> None:
    template = ca_home / "config/web-intermediate.tpl"
    template.write_text(
        "{\n"
        '  "subject": {{ toJson .Subject }},\n'
        '  "keyUsage": ["certSign", "crlSign"],\n'
        '  "basicConstraints": {"isCA": true, "maxPathLen": 0},\n'
        '  "nameConstraints": {\n'
        '    "critical": true,\n'
        f'    "permittedDNSDomains": ["{ALLOWED_NAME}", "{CA_NAME}"]\n'
        "  }\n"
        "}\n"
    )
    template.chmod(0o640)
    os.chown(template, 1000, 1000)
    try:
        step_cli(
            ca_home, "certificate", "create", "--force",
            "--template", "/home/step/config/web-intermediate.tpl",
            "--ca", "/home/step/certs/root_ca.crt",
            "--ca-key", "/home/step/secrets/root_ca_key",
            "--ca-password-file", "/home/step/secrets/password",
            "--password-file", "/home/step/secrets/password",
            "--not-after", "8760h",
            "SolariNet Web Intermediate",
            "/home/step/certs/intermediate_ca.crt",
            "/home/step/secrets/intermediate_ca_key",
        )
    finally:
        template.unlink(missing_ok=True)


def verify_constrained_web_intermediate(ca_home: pathlib.Path) -> None:
    root_cert = ca_home / "certs/root_ca.crt"
    intermediate_cert = ca_home / "certs/intermediate_ca.crt"
    root_text = run(
        ["openssl", "x509", "-in", str(root_cert), "-noout", "-text"], quiet=True
    ).stdout
    intermediate_text = run(
        ["openssl", "x509", "-in", str(intermediate_cert), "-noout", "-text"], quiet=True
    ).stdout
    if not re.search(r"CA:TRUE,\s*pathlen:\s*1\b", root_text):
        raise RuntimeError("disposable root must have pathLen=1 for a sibling web intermediate")
    if not re.search(r"CA:TRUE,\s*pathlen:\s*0\b", intermediate_text):
        raise RuntimeError("web intermediate must have pathLen=0")
    validity_text = run([
        "openssl", "x509", "-in", str(intermediate_cert), "-noout", "-startdate", "-enddate",
    ], quiet=True).stdout
    not_before = re.search(r"notBefore=(.+)", validity_text)
    not_after = re.search(r"notAfter=(.+)", validity_text)
    if not_before is None or not_after is None:
        raise RuntimeError("could not read the web intermediate validity interval")
    date_format = "%b %d %H:%M:%S %Y %Z"
    validity = datetime.strptime(not_after.group(1), date_format).replace(tzinfo=timezone.utc) - \
        datetime.strptime(not_before.group(1), date_format).replace(tzinfo=timezone.utc)
    if validity < timedelta(days=360) or validity > timedelta(days=366):
        raise RuntimeError("disposable web intermediate must have an explicit one-year validity")
    if not re.search(r"X509v3 Name Constraints: critical", intermediate_text):
        raise RuntimeError("web intermediate is missing critical X.509 name constraints")
    permitted_names = (ALLOWED_NAME, CA_NAME)
    if any(DENIED_NAME == name or DENIED_NAME.endswith("." + name) for name in permitted_names):
        raise RuntimeError("denied test DNS name overlaps a permitted name-constraint subtree")
    constraint_text = intermediate_text.split("X509v3 Name Constraints: critical", 1)[1]
    permitted_block = re.search(
        r"\bPermitted:\s*(.*?)(?=\n\s*(?:Excluded:|X509v3 |Signature Algorithm)|\Z)",
        constraint_text, re.DOTALL,
    )
    if permitted_block is None:
        raise RuntimeError("web intermediate has no permitted name-constraint block")
    encoded_permitted_names = set(re.findall(r"DNS:([^\s,]+)", permitted_block.group(1)))
    if encoded_permitted_names != set(permitted_names) or re.search(r"\bExcluded:", constraint_text):
        raise RuntimeError("web intermediate name constraints do not exactly match the two required DNS subtrees")

    test_dir = ca_home / "test-certs"
    test_dir.mkdir(mode=0o750)
    os.chown(test_dir, 1000, 1000)
    try:
        def create_leaf(name: str) -> None:
            step_cli(
                ca_home, "certificate", "create", "--force",
                "--ca", "/home/step/certs/intermediate_ca.crt",
                "--ca-key", "/home/step/secrets/intermediate_ca_key",
                "--ca-password-file", "/home/step/secrets/password",
                "--password-file", "/home/step/secrets/password",
                "--san", name, name,
                f"/home/step/test-certs/{name}.crt",
                f"/home/step/test-certs/{name}.key",
            )

        def verify_leaf(name: str, issuer_chain: pathlib.Path) -> subprocess.CompletedProcess[str]:
            return run([
                "openssl", "verify", "-CAfile", str(root_cert),
                "-untrusted", str(issuer_chain), "-verify_hostname", name,
                str(test_dir / f"{name}.crt"),
            ], check=False, quiet=True)

        for permitted_name in (ALLOWED_NAME, f"host.{ALLOWED_NAME}"):
            create_leaf(permitted_name)
            allowed = verify_leaf(permitted_name, intermediate_cert)
            if allowed.returncode:
                raise RuntimeError("name-constrained chain rejected a DNS name under its permitted subtree: " +
                                   (allowed.stderr or allowed.stdout).strip())

        try:
            create_leaf(DENIED_NAME)
        except RuntimeError as exc:
            raise RuntimeError("step-cli refused to create the disallowed-DNS leaf, so client enforcement was not tested") from exc
        denied = verify_leaf(DENIED_NAME, intermediate_cert)
        denied_output = (denied.stdout + denied.stderr).lower()
        if denied.returncode == 0 or "permitted subtree violation" not in denied_output:
            raise RuntimeError("name-constrained chain did not specifically reject an unpermitted DNS name: " +
                               denied_output.strip())

        nested_ca_cert = test_dir / "nested-intermediate.crt"
        try:
            step_cli(
                ca_home, "certificate", "create", "--force", "--profile", "intermediate-ca",
                "--ca", "/home/step/certs/intermediate_ca.crt",
                "--ca-key", "/home/step/secrets/intermediate_ca_key",
                "--ca-password-file", "/home/step/secrets/password",
                "--password-file", "/home/step/secrets/password",
                "Nested Web CA", "/home/step/test-certs/nested-intermediate.crt",
                "/home/step/test-certs/nested-intermediate.key",
            )
        except RuntimeError as exc:
            raise RuntimeError("step-cli refused the nested-CA probe, so path-length validation was not tested") from exc
        step_cli(
            ca_home, "certificate", "create", "--force",
            "--ca", "/home/step/test-certs/nested-intermediate.crt",
            "--ca-key", "/home/step/test-certs/nested-intermediate.key",
            "--ca-password-file", "/home/step/secrets/password",
            "--password-file", "/home/step/secrets/password",
            "--san", ALLOWED_NAME, "nested-test-leaf",
            "/home/step/test-certs/nested-test-leaf.crt",
            "/home/step/test-certs/nested-test-leaf.key",
        )
        nested_chain = test_dir / "nested-chain.pem"
        nested_chain.write_text(nested_ca_cert.read_text() + intermediate_cert.read_text())
        nested = run([
            "openssl", "verify", "-CAfile", str(root_cert), "-untrusted", str(nested_chain),
            "-verify_hostname", ALLOWED_NAME, str(test_dir / "nested-test-leaf.crt"),
        ], check=False, quiet=True)
        nested_output = (nested.stdout + nested.stderr).lower()
        if nested.returncode == 0 or "path length constraint exceeded" not in nested_output:
            raise RuntimeError("pathLen=0 intermediate did not specifically reject a nested CA: " +
                               nested_output.strip())
    finally:
        shutil.rmtree(test_dir)
        if test_dir.exists():
            raise RuntimeError("disposable certificate-test keys remain in the CA directory")

    print("Disposable web intermediate verified: its critical DNS name constraints reject an out-of-scope leaf, and the chain's path-length limits reject a nested CA.")


def remove_disposable_root_key(ca_home: pathlib.Path) -> None:
    root_key = ca_home / "secrets/root_ca_key"
    root_key.unlink()
    if root_key.exists():
        raise RuntimeError("disposable root signing key remains in the online CA directory")


def registry(*, fail_check: bool = False) -> dict[str, object]:
    return {
        "schema_version": 1,
        "acme": {
            "directory_url": f"https://{CA_NAME}:9443/acme/acme/directory",
            "root_ca": str(ROOT_CERT),
            "webroot": str(WEBROOT),
            "certbot_root": "/etc/letsencrypt-solarinet",
            "target_root": str(ROOT),
        },
        "services": [{
            "id": SERVICE_ID,
            "lineage": LINEAGE,
            "domains": [ALLOWED_NAME],
            "target": str(TARGET),
            "check_command": ["/bin/false"] if fail_check else ["/usr/sbin/httpd", "-t"],
            "reload_command": ["/usr/sbin/httpd", "-k", "graceful"],
            "group": None,
        }],
    }


def put_registry(payload: dict[str, object], registry_file: pathlib.Path) -> None:
    registry_file.write_text(json.dumps(payload, indent=2) + "\n")
    registry_file.chmod(0o600)
    copy_to_client(registry_file, REGISTRY)


def apache_config() -> str:
    return f"""ServerName {ALLOWED_NAME}
<VirtualHost *:80>
    ServerName {ALLOWED_NAME}
    DocumentRoot /var/www/localhost/htdocs
    Alias /.well-known/acme-challenge/ {WEBROOT}/.well-known/acme-challenge/
    <Directory {WEBROOT}/.well-known/acme-challenge/>
        AllowOverride None
        Options None
        Require all granted
    </Directory>
</VirtualHost>
<VirtualHost *:443>
    ServerName {ALLOWED_NAME}
    DocumentRoot /var/www/localhost/htdocs
    SSLEngine on
    SSLCertificateFile {TARGET}/current/fullchain.pem
    SSLCertificateKeyFile {TARGET}/current/privkey.pem
    <Directory /var/www/localhost/htdocs>
        AllowOverride None
        Options None
        Require all granted
    </Directory>
</VirtualHost>
"""


def prepare_client(registry_file: pathlib.Path, root_cert: pathlib.Path,
                   helper_source: pathlib.Path, work: pathlib.Path) -> None:
    docker("run", "--detach", "--label", RESOURCE_LABEL, "--name", CLIENT_NAME,
           "--network", NETWORK_NAME,
           "--network-alias", ALLOWED_NAME, "--network-alias", DENIED_NAME,
           "--entrypoint", "/bin/sh", CERTBOT_IMAGE, "-c", "sleep 36000", quiet=True)
    exec_client("mkdir", "-p", str(WEBROOT / ".well-known/acme-challenge"),
                str(ROOT), str(BOOTSTRAP_RELEASE),
                "/etc/solarinet/pki", "/usr/local/libexec/solarinet-pki",
                "/var/www/localhost/htdocs")
    copy_to_client(root_cert, ROOT_CERT)
    copy_to_client(helper_source, HELPER, "0755")
    copy_to_client(registry_file, REGISTRY)
    conf = work / "apache.conf"
    conf.write_text(apache_config())
    copy_to_client(conf, pathlib.Path("/etc/apache2/conf.d/solarinet-acme-ci.conf"))
    conf.unlink(missing_ok=True)
    exec_client("sh", "-c", (
        "set -eu; "
        "chmod 0755 /var/lib/solarinet /var/lib/solarinet/acme-webroot "
        "/var/lib/solarinet/acme-webroot/.well-known "
        "/var/lib/solarinet/acme-webroot/.well-known/acme-challenge; "
        "printf 'solarinet-acme-integration-ok\\n' > /var/www/localhost/htdocs/index.html; "
        "printf 'probe-ok\\n' > /var/lib/solarinet/acme-webroot/.well-known/acme-challenge/probe; "
        "openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj '/CN=acme-solarinet-ci.invalid' "
        f"-addext 'subjectAltName=DNS:{ALLOWED_NAME}' -addext 'extendedKeyUsage=serverAuth' "
        f"-keyout {BOOTSTRAP_RELEASE}/privkey.pem "
        f"-out {BOOTSTRAP_RELEASE}/fullchain.pem >/dev/null 2>&1; "
        f"chmod 0600 {BOOTSTRAP_RELEASE}/privkey.pem; "
        f"ln -s releases/{BOOTSTRAP_RELEASE.name} {TARGET}/current; "
        "httpd -t; httpd -k start"
    ), timeout=30)
    probe = exec_client("curl", "--fail", "--silent", "--show-error",
                        f"http://{ALLOWED_NAME}/.well-known/acme-challenge/probe")
    if probe.stdout.strip() != "probe-ok":
        raise RuntimeError("Apache HTTP-01 webroot is not reachable")
    print("Disposable Apache HTTP-01 webroot is reachable.")


def issue_and_test(registry_file: pathlib.Path) -> None:
    exec_client("/usr/local/bin/python3", str(HELPER), "--registry", str(REGISTRY),
                "--certbot", "/usr/local/bin/certbot", "--issue-service", SERVICE_ID,
                timeout=240)
    assert_https()
    initial_serial = cert_serial()
    initial_pointer = exec_client("readlink", str(TARGET / "current")).stdout.strip()
    print("Initial HTTP-01 issuance, atomic activation, Apache reload, and HTTPS verification passed.")

    denied = exec_client(
        "/usr/local/bin/certbot", "certonly", "--non-interactive", "--verbose", "--pre-hook", "",
        "--post-hook", "", "--no-directory-hooks", "--agree-tos",
        "--register-unsafely-without-email", "--config-dir", "/tmp/solarinet-certbot-policy-case/config",
        "--work-dir", "/tmp/solarinet-certbot-policy-case/work", "--logs-dir",
        "/tmp/solarinet-certbot-policy-case/logs", "--server",
        f"https://{CA_NAME}:9443/acme/acme/directory", "--authenticator", "webroot",
        "--preferred-challenges", "http-01", "--webroot-path", str(WEBROOT),
        "--cert-name", "policy-case", "--domain", DENIED_NAME,
        "--deploy-hook", "/bin/true", timeout=180, check=False,
        env={"REQUESTS_CA_BUNDLE": str(ROOT_CERT)})
    if denied.returncode == 0:
        raise RuntimeError("disposable CA issued an unapproved DNS name")
    message = (denied.stdout + "\n" + denied.stderr).casefold()
    # python-acme's localized message for ACME rejectedIdentifier is this
    # exact string; unlike loose words, it cannot match a config/log path.
    if "the server will not issue certificates for the identifier" not in message:
        ca_log = docker("logs", CA_CONTAINER_NAME, check=False, quiet=True).stdout
        detail = "\n".join((denied.stdout + "\n" + denied.stderr).splitlines()[-10:])
        ca_detail = "\n".join(ca_log.splitlines()[-10:])
        raise RuntimeError("unapproved-name request did not return rejectedIdentifier; client:\n" +
                           detail + "\nstep-ca:\n" + ca_detail)
    registered = exec_client("test", "-d", "/tmp/solarinet-certbot-policy-case/config/accounts",
                             check=False, quiet=True)
    if registered.returncode != 0:
        raise RuntimeError("unapproved-name request failed before ACME account registration")
    print("Disposable step-ca rejected the challengeable but unapproved DNS name.")

    exec_client("/usr/local/bin/python3", str(HELPER), "--registry", str(REGISTRY),
                "--certbot", "/usr/local/bin/certbot", "--renew", "--force-renewal",
                timeout=300)
    assert_https()
    renewed_serial = cert_serial()
    if renewed_serial == initial_serial:
        raise RuntimeError("forced renewal did not replace the serving certificate")
    print("Forced Certbot renewal, revalidation, atomic activation, and Apache reload passed.")

    renewed_pointer = exec_client("readlink", str(TARGET / "current")).stdout.strip()
    if renewed_pointer == initial_pointer:
        raise RuntimeError("forced renewal did not create a distinct release directory")
    replace_current_pointer(initial_pointer)
    put_registry(registry(fail_check=True), registry_file)
    failed = exec_client("/usr/local/bin/python3", str(HELPER), "--registry", str(REGISTRY),
                         "--deploy-service", SERVICE_ID, timeout=60, check=False, quiet=True)
    if failed.returncode == 0:
        raise RuntimeError("injected Apache config failure unexpectedly activated")
    restored = exec_client("readlink", str(TARGET / "current")).stdout.strip()
    if restored != initial_pointer:
        raise RuntimeError("failed config validation did not restore the previous certificate pointer")
    assert_https()
    print("Injected service-check failure restored the prior pointer and kept HTTPS healthy.")
    put_registry(registry(), registry_file)
    exec_client("/usr/local/bin/python3", str(HELPER), "--registry", str(REGISTRY),
                "--deploy-service", SERVICE_ID, timeout=60)
    if exec_client("readlink", str(TARGET / "current")).stdout.strip() != renewed_pointer:
        raise RuntimeError("successful deployment did not restore the latest issued release")
    assert_https()
    print("Retry after restoring the valid service check activated the latest release.")


def main() -> int:
    if os.environ.get("SOLARINET_ACME_INTEGRATION") != "1":
        raise SystemExit("Set SOLARINET_ACME_INTEGRATION=1 to run this opt-in Docker integration test")
    if os.geteuid() != 0:
        raise SystemExit("Run this integration test as root on an isolated CI runner")
    repo = pathlib.Path(__file__).resolve().parents[2]
    helper_source = repo / "deploy/pki/acme_certificates.py"
    if not helper_source.is_file():
        raise SystemExit("ACME helper source is missing")

    work: pathlib.Path | None = None
    try:
        tmp_parent = pathlib.Path(os.environ.get("RUNNER_TEMP", "/dev/shm"))
        tmp_parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        work = pathlib.Path(tempfile.mkdtemp(
            prefix=f"solarinet-acme-integration-{RUN_ID}-", dir=tmp_parent))
        ca_home = work / "step"
        (ca_home / "secrets").mkdir(parents=True, mode=0o700)
        password = ca_home / "secrets/password"
        password.write_text(secrets.token_urlsafe(24) + "\n")
        password.chmod(0o600)
        os.chown(ca_home, 1000, 1000)
        os.chown(ca_home / "secrets", 1000, 1000)
        os.chown(password, 1000, 1000)
        os.chmod(ca_home, 0o750)
        registry_file = work / "registry.json"
        registry_file.write_text(json.dumps(registry(), indent=2) + "\n")
        registry_file.chmod(0o600)
        image_context = work / "client-image"
        image_context.mkdir(mode=0o700)
        image_dockerfile = image_context / "Dockerfile"
        shutil.copyfile(repo / "deploy/pki/acme_test.Dockerfile", image_dockerfile)
        docker("build", "--file", str(image_dockerfile), "--tag", CERTBOT_IMAGE,
               str(image_context), timeout=600, quiet=True)
        docker("network", "create", "--internal", "--label", RESOURCE_LABEL,
               NETWORK_NAME, quiet=True)
        docker("run", "--rm", "--label", RESOURCE_LABEL, "--network", "none", "--user", "1000:1000",
               "--env", "STEPPATH=/home/step", "--volume", f"{ca_home}:/home/step",
               STEP_CLI_IMAGE, "step", "ca", "init", "--name", "SolariNet disposable CI CA",
               "--dns", CA_NAME, "--address", ":9443", "--provisioner", "ci@example.invalid",
               "--password-file", "/home/step/secrets/password",
               "--provisioner-password-file", "/home/step/secrets/password", "--acme",
               timeout=120, quiet=True)
        install_constrained_web_intermediate(ca_home)
        verify_constrained_web_intermediate(ca_home)
        remove_disposable_root_key(ca_home)
        write_ca_config(ca_home)
        docker("run", "--detach", "--label", RESOURCE_LABEL, "--name", CA_CONTAINER_NAME, "--network",
               NETWORK_NAME, "--network-alias", CA_NAME,
               "--volume", f"{ca_home}:/home/step", STEP_CA_IMAGE, quiet=True)
        prepare_client(registry_file, ca_home / "certs/root_ca.crt", helper_source, work)
        wait_for_ca()
        issue_and_test(registry_file)
        print("ACME disposable CA integration test PASSED.")
        return 0
    finally:
        cleanup_errors: list[str] = []

        def cleanup_call(label: str, args: Sequence[str], *,
                         tolerate_nonzero: bool = False) -> subprocess.CompletedProcess[str] | None:
            try:
                result = docker(*args, check=False, quiet=True)
                if result.returncode and not tolerate_nonzero:
                    cleanup_errors.append(f"{label} exited {result.returncode}")
                return result
            except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
                cleanup_errors.append(f"{label}: {exc}")
                return None

        cleanup_call("remove ACME client", ["rm", "--force", CLIENT_NAME], tolerate_nonzero=True)
        cleanup_call("remove CA container", ["rm", "--force", CA_CONTAINER_NAME], tolerate_nonzero=True)
        listed = cleanup_call("list test containers", ["ps", "--all", "--quiet", "--filter",
                                                          f"label={RESOURCE_LABEL}"])
        leftovers = listed.stdout.split() if listed else []
        if leftovers:
            cleanup_call("remove test containers", ["rm", "--force", *leftovers],
                         tolerate_nonzero=True)
        listed = cleanup_call("verify test containers", ["ps", "--all", "--quiet", "--filter",
                                                           f"label={RESOURCE_LABEL}"])
        remaining_containers = listed.stdout.split() if listed else ["inspection failed"]
        if remaining_containers:
            cleanup_errors.append("test-labeled Docker containers remain after cleanup")
        listed = cleanup_call("list test networks", ["network", "ls", "--quiet", "--filter",
                                                       f"label={RESOURCE_LABEL}"])
        networks = listed.stdout.split() if listed else []
        if networks:
            cleanup_call("remove test networks", ["network", "rm", *networks],
                         tolerate_nonzero=True)
        listed = cleanup_call("verify test networks", ["network", "ls", "--quiet", "--filter",
                                                        f"label={RESOURCE_LABEL}"])
        remaining_networks = listed.stdout.split() if listed else ["inspection failed"]
        if remaining_networks:
            cleanup_errors.append("test-labeled Docker networks remain after cleanup")
        cleanup_call("remove run-specific Certbot image", ["image", "rm", "--force", CERTBOT_IMAGE],
                     tolerate_nonzero=True)
        remaining_image = cleanup_call("verify run-specific Certbot image removal", [
            "image", "ls", "--format", "{{.Repository}}:{{.Tag}}", "--filter",
            f"reference={CERTBOT_IMAGE}",
        ])
        if remaining_image is None or remaining_image.stdout.strip():
            cleanup_errors.append("run-specific Certbot test image remains or could not be inspected")
        if work is not None:
            try:
                shutil.rmtree(work)
            except OSError as exc:
                cleanup_errors.append(f"disposable CA scratch cleanup failed: {exc}")
            else:
                if work.exists():
                    cleanup_errors.append("disposable CA scratch directory remains after cleanup")
        if cleanup_errors:
            raise RuntimeError("test cleanup failed: " + "; ".join(cleanup_errors))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.SubprocessError, RuntimeError, ValueError) as exc:
        print(f"ACME disposable integration test failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
