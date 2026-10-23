from solarinet_mcp import server
from solarinet_mcp.config import Config, DbConfig, RestConfig


def _config(transport, port=8765):
    return Config(
        rest=RestConfig(base_url="", token=None, timeout=30, verify_tls=True),
        db=DbConfig(host="127.0.0.1", port=3306, name="solarinet", user=None,
                    password=None, timeout=10),
        transport=transport,
        port=port,
    )


def test_streamable_http_uses_sdk_transport_name_and_configured_port(monkeypatch):
    calls = []
    monkeypatch.setattr(server.mcp, "run", lambda **kwargs: calls.append(kwargs))
    monkeypatch.setattr(server.mcp.settings, "port", server.mcp.settings.port)

    server.run(_config("streamable_http", 9876))

    assert server.mcp.settings.port == 9876
    assert calls == [{"transport": "streamable-http"}]


def test_stdio_remains_the_default_transport(monkeypatch):
    calls = []
    monkeypatch.setattr(server.mcp, "run", lambda **kwargs: calls.append(kwargs))

    server.run(_config("stdio"))

    assert calls == [{}]
