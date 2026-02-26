from __future__ import annotations

from pathlib import Path

import pytest

from mcp_server.config import ConfigError, load_config


def test_load_default_config_without_file() -> None:
    config = load_config(None)
    assert config.ue.ws_url == "ws://127.0.0.1:19090"
    assert config.ue.connection_file == ""
    assert config.ue.project_root == ""
    assert config.request.default_timeout_ms == 30_000
    assert config.catalog.include_schemas is True
    assert config.catalog.required_tools == ()
    assert config.catalog.pin_schema_hash == ""
    assert config.catalog.fail_on_schema_change is False
    assert config.retry.transient_max_attempts == 2
    assert config.metrics.enabled is True


def test_load_yaml_config(tmp_path: Path) -> None:
    config_path = tmp_path / "config.yaml"
    config_path.write_text(
        """
server:
  log_level: DEBUG
  json_logs: true
ue:
  ws_url: ws://localhost:19091
  connection_file: Saved/UnrealMCP/connection.json
  project_root: ../TestMcp
  connect_timeout_s: 7
  ping_interval_s: 5
  reconnect:
    initial_delay_s: 1
    max_delay_s: 3
request:
  default_timeout_ms: 12345
catalog:
  include_schemas: false
  refresh_interval_s: 15
  required_tools:
    - system.health
    - asset.create
  pin_schema_hash: hash-abc
  fail_on_schema_change: true
retry:
  transient_max_attempts: 3
  backoff_initial_s: 0.3
  backoff_max_s: 2
metrics:
  enabled: false
  log_interval_s: 5
""".strip(),
        encoding="utf-8",
    )

    config = load_config(config_path)
    assert config.server.log_level == "DEBUG"
    assert config.server.json_logs is True
    assert config.ue.ws_url == "ws://localhost:19091"
    assert config.ue.connection_file == "Saved/UnrealMCP/connection.json"
    assert config.ue.project_root == "../TestMcp"
    assert config.ue.connect_timeout_s == 7
    assert config.ue.reconnect.initial_delay_s == 1
    assert config.request.default_timeout_ms == 12_345
    assert config.catalog.include_schemas is False
    assert config.catalog.refresh_interval_s == 15
    assert config.catalog.required_tools == ("system.health", "asset.create")
    assert config.catalog.pin_schema_hash == "hash-abc"
    assert config.catalog.fail_on_schema_change is True
    assert config.retry.transient_max_attempts == 3
    assert config.retry.backoff_initial_s == 0.3
    assert config.retry.backoff_max_s == 2
    assert config.metrics.enabled is False
    assert config.metrics.log_interval_s == 5


def test_invalid_ws_url_raises_config_error(tmp_path: Path) -> None:
    config_path = tmp_path / "config.yaml"
    config_path.write_text(
        """
ue:
  ws_url: http://localhost:19090
""".strip(),
        encoding="utf-8",
    )

    with pytest.raises(ConfigError):
        load_config(config_path)


def test_invalid_catalog_interval_raises_config_error(tmp_path: Path) -> None:
    config_path = tmp_path / "config.yaml"
    config_path.write_text(
        """
catalog:
  refresh_interval_s: -1
""".strip(),
        encoding="utf-8",
    )

    with pytest.raises(ConfigError):
        load_config(config_path)


def test_invalid_retry_attempts_raise_config_error(tmp_path: Path) -> None:
    config_path = tmp_path / "config.yaml"
    config_path.write_text(
        """
retry:
  transient_max_attempts: 0
""".strip(),
        encoding="utf-8",
    )

    with pytest.raises(ConfigError):
        load_config(config_path)


def test_invalid_required_tools_type_raises_config_error(tmp_path: Path) -> None:
    config_path = tmp_path / "config.yaml"
    config_path.write_text(
        """
catalog:
  required_tools: system.health
""".strip(),
        encoding="utf-8",
    )

    with pytest.raises(ConfigError):
        load_config(config_path)
