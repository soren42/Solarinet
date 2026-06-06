"""
Async REST client for the SolariNet dashboard API (section 11.2).

All endpoints return the stable envelope:
    { "ok": true,  "data": <...>, "ts": "ISO-8601" }
    { "ok": false, "error": { "code": <str>, "message": <str> } }

This module unwraps that envelope, maps transport/HTTP/application errors to a
single SolariApiError, and is injectable with a custom httpx transport so the
tools can be unit-tested without a live server.
"""
from __future__ import annotations

from typing import Any, Mapping

import httpx

from .config import RestConfig


class SolariApiError(Exception):
    """An error from the dashboard API or its transport.

    Attributes:
        message: human-readable description.
        code: short machine code where known (HTTP status or envelope code).
    """

    def __init__(self, message: str, code: str | None = None) -> None:
        super().__init__(message)
        self.message = message
        self.code = code


class RestClient:
    """Thin async wrapper over the section 11.2 API."""

    def __init__(self, cfg: RestConfig, transport: httpx.AsyncBaseTransport | None = None) -> None:
        self._cfg = cfg
        self._transport = transport  # injected in tests; None => real network

    def _headers(self) -> dict[str, str]:
        headers = {"Accept": "application/json"}
        if self._cfg.token:
            headers["Authorization"] = f"Bearer {self._cfg.token}"
        return headers

    async def request(
        self,
        method: str,
        path: str,
        *,
        params: Mapping[str, Any] | None = None,
        json_body: Mapping[str, Any] | None = None,
    ) -> Any:
        """Call the API and return the unwrapped `data` field.

        Raises SolariApiError on transport failure, non-2xx status, malformed
        envelope, or an envelope with ok=false.
        """
        if not self._cfg.configured:
            raise SolariApiError(
                "REST API not configured. Set SOLARINET_API_BASE (and "
                "SOLARINET_API_TOKEN for control actions).",
                code="not_configured",
            )
        url = f"{self._cfg.base_url}{path}"
        # Drop params whose value is None so we don't send empty query keys.
        clean_params = {k: v for k, v in (params or {}).items() if v is not None}
        try:
            async with httpx.AsyncClient(
                transport=self._transport,
                timeout=self._cfg.timeout,
                verify=self._cfg.verify_tls,
            ) as client:
                resp = await client.request(
                    method,
                    url,
                    params=clean_params or None,
                    json=dict(json_body) if json_body is not None else None,
                    headers=self._headers(),
                )
        except httpx.TimeoutException as exc:
            raise SolariApiError(f"Request to {path} timed out", code="timeout") from exc
        except httpx.TransportError as exc:
            raise SolariApiError(
                f"Could not reach the dashboard API at {self._cfg.base_url}: {exc}",
                code="unreachable",
            ) from exc

        return self._parse(resp, path)

    @staticmethod
    def _parse(resp: httpx.Response, path: str) -> Any:
        # Try to read the envelope even on error status (the API uses it for errors).
        body: Any
        try:
            body = resp.json()
        except ValueError:
            body = None

        if isinstance(body, Mapping) and body.get("ok") is False:
            err = body.get("error") or {}
            raise SolariApiError(
                err.get("message", "API returned ok=false"),
                code=str(err.get("code", resp.status_code)),
            )

        if resp.status_code == 401:
            raise SolariApiError(
                "Unauthorized. Check SOLARINET_API_TOKEN.", code="401"
            )
        if resp.status_code == 403:
            raise SolariApiError(
                "Forbidden. This action needs the operator role.", code="403"
            )
        if resp.status_code == 404:
            raise SolariApiError(f"Not found: {path}", code="404")
        if resp.status_code >= 400:
            raise SolariApiError(
                f"API request to {path} failed with HTTP {resp.status_code}",
                code=str(resp.status_code),
            )

        if not isinstance(body, Mapping) or "data" not in body:
            raise SolariApiError(
                f"Malformed response from {path}: missing envelope 'data'",
                code="bad_envelope",
            )
        return body["data"]
