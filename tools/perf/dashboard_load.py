#!/usr/bin/env python3
"""Measure dashboard panel readiness and request waterfalls with Playwright.

Install Playwright into an isolated venv, then run with --storage-state pointing
at an existing authenticated browser state. The state file is read, never copied
or printed. Without one, the script records the unauthenticated shell/login gate.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from playwright.async_api import async_playwright

PANELS = [
    "Fleet Overview", "Reachability", "Topology Map", "Panel", "Alerts",
    "Analysis", "Systems", "Inventory", "Tags & Scan", "RackWire",
    "Discovery", "Provisioning", "Images & Stacks", "Maintenance", "Git",
    "Certificates", "DNS", "Config & Rules",
]


async def wait_ready(page, timeout_ms: int) -> dict[str, Any]:
    """Wait for app boot, quiet API traffic, and no visible loading cues."""
    await page.wait_for_function(
        "() => document.querySelector('.app') || document.querySelector('form')",
        timeout=timeout_ms,
    )
    app_visible = await page.locator(".app").count() > 0
    if not app_visible:
        return {"state": "auth_gate", "ready_ms": None}

    start = time.monotonic()
    await page.wait_for_function(
        """() => {
          const pending = window.__solariPerfPending || 0;
          const quietSince = window.__solariPerfQuietSince || 0;
          const quiet = quietSince && performance.now() - quietSince >= 500;
          const content = document.querySelector('.content');
          const loading = content && [...content.querySelectorAll('[role=status], [aria-live]')]
            .some(el => el.getClientRects().length && /loading|checking|fetching|working/i.test(el.textContent || ''));
          return pending === 0 && quiet && !loading;
        }""",
        timeout=timeout_ms,
    )
    return {"state": "rendered", "ready_ms": round((time.monotonic() - start) * 1000, 1)}


async def instrument(page):
    await page.add_init_script("""
      (() => {
        window.__solariPerfPending = 0;
        window.__solariPerfQuietSince = performance.now();
        const active = new Set();
        const begin = (key) => { active.add(key); window.__solariPerfPending = active.size; };
        const end = (key) => {
          active.delete(key);
          window.__solariPerfPending = active.size;
          if (!active.size) window.__solariPerfQuietSince = performance.now();
        };
        const originalFetch = window.fetch;
        window.fetch = function(input, init) {
          const url = typeof input === 'string' ? input : input.url;
          if (new URL(url, location.href).pathname.startsWith('/api/stream')) return originalFetch.apply(this, arguments);
          const key = 'fetch:' + Math.random(); begin(key);
          return originalFetch.apply(this, arguments).finally(() => end(key));
        };
        const open = XMLHttpRequest.prototype.open;
        const send = XMLHttpRequest.prototype.send;
        XMLHttpRequest.prototype.open = function(method, url) { this.__perfUrl = url; return open.apply(this, arguments); };
        XMLHttpRequest.prototype.send = function() {
          if (!new URL(this.__perfUrl, location.href).pathname.startsWith('/api/stream')) {
            const key = 'xhr:' + Math.random(); begin(key);
            this.addEventListener('loadend', () => end(key), { once: true });
          }
          return send.apply(this, arguments);
        };
      })();
    """)


async def measure_page(page, url: str, phase: str, timeout_ms: int, waterfall: list[dict[str, Any]]):
    current_panel = "shell"
    pending: dict[int, dict[str, Any]] = {}

    def on_request(request):
        nonlocal current_panel
        if request.url.startswith("data:"):
            return
        key = id(request)
        pending[key] = {
            "phase": phase,
            "panel": current_panel,
            "method": request.method,
            "url": request.url,
            "started_ms": round(time.monotonic() * 1000, 1),
        }

    async def on_request_done(request):
        record = pending.pop(id(request), None)
        if record is None:
            return
        record["duration_ms"] = round(time.monotonic() * 1000 - record["started_ms"], 1)
        record.pop("started_ms", None)
        response = await request.response()
        record["status"] = response.status if response else None
        waterfall.append(record)

    page.on("request", on_request)
    page.on("requestfinished", on_request_done)
    page.on("requestfailed", on_request_done)
    await instrument(page)
    nav_start = time.monotonic()
    response = await page.goto(url, wait_until="domcontentloaded") if phase == "cold" else await page.reload(wait_until="domcontentloaded")
    boot = await wait_ready(page, timeout_ms)
    result: dict[str, Any] = {
        "phase": phase,
        "http_status": response.status if response else None,
        "boot_state": boot["state"],
        "boot_ready_ms": round((time.monotonic() - nav_start) * 1000, 1) if boot["state"] == "rendered" else None,
        "panels": [],
    }
    if boot["state"] != "rendered":
        return result

    nav = page.locator(".nav-item")
    for name in PANELS:
        item = nav.filter(has_text=name).first
        if not await item.count():
            continue
        current_panel = name
        start = time.monotonic()
        try:
            # The sidebar is off-canvas at iPad portrait width; a DOM click
            # navigates without depending on the drawer being open.
            await item.evaluate("el => el.click()")
            await page.evaluate("window.__solariPerfQuietSince = performance.now()")
            readiness = await wait_ready(page, timeout_ms)
            result["panels"].append({
                "name": name,
                "state": readiness["state"],
                "ready_ms": round((time.monotonic() - start) * 1000, 1) if readiness["state"] == "rendered" else None,
            })
        except Exception as exc:  # record panel-specific timeout or selector failure
            result["panels"].append({"name": name, "state": "timeout_or_error", "error": str(exc)[:180]})
    return result


async def run(args):
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    results: dict[str, Any] = {"url": args.url, "viewport": {"width": 834, "height": 1194}, "cpu_slowdown": 4, "runs": [], "waterfall": []}
    state_path = Path(args.storage_state) if args.storage_state else None
    if state_path and not state_path.is_file():
        raise SystemExit("storage-state file does not exist")

    async with async_playwright() as p:
        browser_type = getattr(p, args.browser)
        browser = await browser_type.launch(headless=True)
        context_args = {
            "viewport": {"width": 834, "height": 1194},
            "device_scale_factor": 2,
            "is_mobile": True,
            "has_touch": True,
            "service_workers": "allow",
            "ignore_https_errors": args.insecure,
        }
        if state_path:
            context_args["storage_state"] = str(state_path)
        context = await browser.new_context(**context_args)
        page = await context.new_page()
        if args.browser == "chromium":
            cdp = await context.new_cdp_session(page)
            await cdp.send("Emulation.setCPUThrottlingRate", {"rate": 4})
        for phase in ("cold", "warm"):
            result = await measure_page(page, args.url, phase, args.timeout_ms, results["waterfall"])
            results["runs"].append(result)
            if result["boot_state"] == "auth_gate":
                break
        await context.close()
        await browser.close()

    results["measurement_limit"] = (
        "No authenticated storage state supplied; panel timings are unavailable."
        if not state_path else None
    )
    output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "run_states": [r["boot_state"] for r in results["runs"]], "waterfall_requests": len(results["waterfall"])}))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="https://dashboard.akoria.net/")
    parser.add_argument("--storage-state", help="path to authenticated Playwright storage-state JSON")
    parser.add_argument("--output", default="/tmp/solarinet-dashboard-perf.json")
    parser.add_argument("--browser", choices=["webkit", "chromium"], default="chromium")
    parser.add_argument("--timeout-ms", type=int, default=90000)
    parser.add_argument("--insecure", action="store_true", help="accept the self-signed dashboard certificate")
    asyncio.run(run(parser.parse_args()))


if __name__ == "__main__":
    main()
