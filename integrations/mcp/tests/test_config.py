import pytest

from solarinet_mcp.config import load_config


def test_streamable_http_port_defaults_and_accepts_boundaries(monkeypatch):
    monkeypatch.delenv("SOLARINET_MCP_PORT", raising=False)
    assert load_config().port == 8765

    for port in ("1", "65535"):
        monkeypatch.setenv("SOLARINET_MCP_PORT", port)
        assert load_config().port == int(port)


@pytest.mark.parametrize("port", ["0", "65536", "nope"])
def test_invalid_streamable_http_port_fails_configuration(port, monkeypatch):
    monkeypatch.setenv("SOLARINET_MCP_PORT", port)
    with pytest.raises(ValueError, match="SOLARINET_MCP_PORT"):
        load_config()
