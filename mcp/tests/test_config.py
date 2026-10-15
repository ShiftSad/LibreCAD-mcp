# SPDX-License-Identifier: GPL-2.0-or-later
import json

import pytest

from librecad_mcp.config import DEFAULT_URL, load_config
from librecad_mcp.discovery import default_paths
from librecad_mcp.errors import ConfigError


def put_default_discovery(data: dict):
    p = default_paths()[0]
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data), encoding="utf-8")
    return p


def test_environment_wins(monkeypatch):
    put_default_discovery({"url": "http://127.0.0.1:1", "token": "from-file"})
    monkeypatch.setenv("LIBRECAD_API_URL", "http://127.0.0.1:2/")
    monkeypatch.setenv("LIBRECAD_API_TOKEN", " from-env ")
    cfg = load_config()
    assert cfg.base_url == "http://127.0.0.1:2" and cfg.token == "from-env"
    assert cfg.token_source == "LIBRECAD_API_TOKEN"


def test_env_token_takes_the_port_from_the_discovery_file(monkeypatch):
    put_default_discovery({"url": "http://127.0.0.1:13999", "token": "from-file"})
    monkeypatch.setenv("LIBRECAD_API_TOKEN", "from-env")
    cfg = load_config()
    assert cfg.base_url == "http://127.0.0.1:13999" and cfg.token == "from-env"


def test_env_token_without_discovery_uses_the_default_url(monkeypatch):
    monkeypatch.setenv("LIBRECAD_API_TOKEN", "from-env")
    assert load_config().base_url == DEFAULT_URL


def test_os_default_discovery_file_needs_no_configuration():
    p = put_default_discovery({"url": "http://127.0.0.1:13140", "token": "abc", "pid": 1})
    cfg = load_config()
    assert cfg.token == "abc" and cfg.base_url == "http://127.0.0.1:13140"
    assert cfg.token_source == "discovery file" and cfg.discovery_path == str(p)


def test_explicit_discovery_file_beats_the_default(monkeypatch, tmp_path):
    put_default_discovery({"url": "http://127.0.0.1:1", "token": "default"})
    other = tmp_path / "other.json"
    other.write_text(json.dumps({"url": "http://127.0.0.1:2", "token": "explicit"}), encoding="utf-8")
    monkeypatch.setenv("LIBRECAD_API_DISCOVERY", str(other))
    assert load_config().token == "explicit"


def test_unsubstituted_placeholders_are_ignored(monkeypatch):
    put_default_discovery({"url": "http://127.0.0.1:13140", "token": "abc"})
    monkeypatch.setenv("LIBRECAD_API_TOKEN", "${user_config.api_token}")
    monkeypatch.setenv("LIBRECAD_API_URL", "${user_config.api_url}")
    monkeypatch.setenv("LIBRECAD_API_DISCOVERY", "${user_config.discovery}")
    cfg = load_config()
    assert cfg.token == "abc" and cfg.token_source == "discovery file"


def test_missing_discovery_file_is_a_helpful_not_reachable_error():
    with pytest.raises(ConfigError) as exc:
        load_config()
    msg = str(exc.value)
    assert "not reachable" in msg and str(default_paths()[0]) in msg and "Remote API (MCP)" in msg


def test_missing_explicit_discovery_file_names_it(monkeypatch, tmp_path):
    monkeypatch.setenv("LIBRECAD_API_DISCOVERY", str(tmp_path / "gone.json"))
    with pytest.raises(ConfigError, match="gone.json"):
        load_config()


def test_broken_discovery_file_is_reported(monkeypatch, tmp_path):
    p = tmp_path / "remote-api.json"
    p.write_text("{", encoding="utf-8")
    monkeypatch.setenv("LIBRECAD_API_DISCOVERY", str(p))
    with pytest.raises(ConfigError, match="not valid JSON"):
        load_config()


@pytest.mark.parametrize("value", ["abc", "0", "-1", "9999"])
def test_bad_timeout(monkeypatch, value):
    monkeypatch.setenv("LIBRECAD_API_TOKEN", "t")
    monkeypatch.setenv("LIBRECAD_API_TIMEOUT", value)
    with pytest.raises(ConfigError, match="LIBRECAD_API_TIMEOUT"):
        load_config()


def test_timeout_and_api_suffix(monkeypatch):
    monkeypatch.setenv("LIBRECAD_API_TOKEN", "t")
    monkeypatch.setenv("LIBRECAD_API_URL", "http://localhost:13140/api/v1/")
    monkeypatch.setenv("LIBRECAD_API_TIMEOUT", "12.5")
    cfg = load_config()
    assert cfg.base_url == "http://localhost:13140" and cfg.timeout == 12.5


def test_bad_url(monkeypatch):
    monkeypatch.setenv("LIBRECAD_API_TOKEN", "t")
    monkeypatch.setenv("LIBRECAD_API_URL", "127.0.0.1:13140")
    with pytest.raises(ConfigError, match="LIBRECAD_API_URL"):
        load_config()


def test_token_with_unsendable_characters_is_refused_without_echo(monkeypatch):
    monkeypatch.setenv("LIBRECAD_API_TOKEN", "abc“def")
    with pytest.raises(ConfigError) as exc:
        load_config()
    assert "abc" not in str(exc.value)


def test_the_token_is_not_in_repr_or_describe(monkeypatch):
    monkeypatch.setenv("LIBRECAD_API_TOKEN", "very-secret-token")
    cfg = load_config()
    assert "very-secret-token" not in repr(cfg)
    assert "very-secret-token" not in json.dumps(cfg.describe())
