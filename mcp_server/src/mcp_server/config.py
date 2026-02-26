from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

import yaml


class ConfigError(ValueError):
    """설정 파일 검증 실패."""


@dataclass(frozen=True)
class ReconnectConfig:
    initial_delay_s: float = 0.5
    max_delay_s: float = 10.0


@dataclass(frozen=True)
class UeConfig:
    ws_url: str = "ws://127.0.0.1:19090"
    connection_file: str = ""
    project_root: str = ""
    connect_timeout_s: float = 10.0
    ping_interval_s: float = 10.0
    reconnect: ReconnectConfig = ReconnectConfig()


@dataclass(frozen=True)
class RequestConfig:
    default_timeout_ms: int = 30_000


@dataclass(frozen=True)
class ServerConfig:
    log_level: str = "INFO"
    json_logs: bool = False


@dataclass(frozen=True)
class CatalogConfig:
    include_schemas: bool = True
    refresh_interval_s: float = 60.0
    required_tools: tuple[str, ...] = ()
    pin_schema_hash: str = ""
    fail_on_schema_change: bool = False


@dataclass(frozen=True)
class RetryConfig:
    transient_max_attempts: int = 2
    backoff_initial_s: float = 0.2
    backoff_max_s: float = 1.0


@dataclass(frozen=True)
class MetricsConfig:
    enabled: bool = True
    log_interval_s: float = 30.0


@dataclass(frozen=True)
class AppConfig:
    server: ServerConfig = ServerConfig()
    ue: UeConfig = UeConfig()
    request: RequestConfig = RequestConfig()
    catalog: CatalogConfig = CatalogConfig()
    retry: RetryConfig = RetryConfig()
    metrics: MetricsConfig = MetricsConfig()


def load_config(path: str | Path | None) -> AppConfig:
    """YAML 설정 파일을 로드하고 검증한다."""
    if path is None:
        config = AppConfig()
        _validate_config(config)
        return config

    config_path = Path(path)
    if not config_path.exists():
        example_path = config_path.parent / "config.example.yaml"
        if example_path.exists():
            raise ConfigError(
                f"Config file not found: {config_path} (hint: copy {example_path} to {config_path})"
            )
        raise ConfigError(f"Config file not found: {config_path}")

    raw_data = yaml.safe_load(config_path.read_text(encoding="utf-8")) or {}
    if not isinstance(raw_data, dict):
        raise ConfigError("Config root must be a mapping object.")

    server_data = _as_dict(raw_data.get("server"), "server")
    ue_data = _as_dict(raw_data.get("ue"), "ue")
    request_data = _as_dict(raw_data.get("request"), "request")
    catalog_data = _as_dict(raw_data.get("catalog"), "catalog")
    retry_data = _as_dict(raw_data.get("retry"), "retry")
    metrics_data = _as_dict(raw_data.get("metrics"), "metrics")
    reconnect_data = _as_dict(ue_data.get("reconnect"), "ue.reconnect")

    config = AppConfig(
        server=ServerConfig(
            log_level=str(server_data.get("log_level", ServerConfig.log_level)),
            json_logs=bool(server_data.get("json_logs", ServerConfig.json_logs)),
        ),
        ue=UeConfig(
            ws_url=str(ue_data.get("ws_url", UeConfig.ws_url)),
            connection_file=str(ue_data.get("connection_file", UeConfig.connection_file)),
            project_root=str(ue_data.get("project_root", UeConfig.project_root)),
            connect_timeout_s=float(ue_data.get("connect_timeout_s", UeConfig.connect_timeout_s)),
            ping_interval_s=float(ue_data.get("ping_interval_s", UeConfig.ping_interval_s)),
            reconnect=ReconnectConfig(
                initial_delay_s=float(
                    reconnect_data.get("initial_delay_s", ReconnectConfig.initial_delay_s)
                ),
                max_delay_s=float(reconnect_data.get("max_delay_s", ReconnectConfig.max_delay_s)),
            ),
        ),
        request=RequestConfig(
            default_timeout_ms=int(
                request_data.get("default_timeout_ms", RequestConfig.default_timeout_ms)
            ),
        ),
        catalog=CatalogConfig(
            include_schemas=bool(
                catalog_data.get("include_schemas", CatalogConfig.include_schemas)
            ),
            refresh_interval_s=float(
                catalog_data.get("refresh_interval_s", CatalogConfig.refresh_interval_s)
            ),
            required_tools=_parse_required_tools(catalog_data.get("required_tools")),
            pin_schema_hash=str(
                catalog_data.get("pin_schema_hash", CatalogConfig.pin_schema_hash)
            ).strip(),
            fail_on_schema_change=bool(
                catalog_data.get(
                    "fail_on_schema_change",
                    CatalogConfig.fail_on_schema_change,
                )
            ),
        ),
        retry=RetryConfig(
            transient_max_attempts=int(
                retry_data.get(
                    "transient_max_attempts", RetryConfig.transient_max_attempts
                )
            ),
            backoff_initial_s=float(
                retry_data.get("backoff_initial_s", RetryConfig.backoff_initial_s)
            ),
            backoff_max_s=float(
                retry_data.get("backoff_max_s", RetryConfig.backoff_max_s)
            ),
        ),
        metrics=MetricsConfig(
            enabled=bool(metrics_data.get("enabled", MetricsConfig.enabled)),
            log_interval_s=float(
                metrics_data.get("log_interval_s", MetricsConfig.log_interval_s)
            ),
        ),
    )

    _validate_config(config)
    return config


def _as_dict(value: Any, name: str) -> dict[str, Any]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ConfigError(f"'{name}' must be a mapping object.")
    return value


def _validate_config(config: AppConfig) -> None:
    parsed = urlparse(config.ue.ws_url)
    if parsed.scheme not in {"ws", "wss"}:
        raise ConfigError("ue.ws_url must start with ws:// or wss://")
    if not parsed.netloc:
        raise ConfigError("ue.ws_url must include host:port")

    if config.ue.connect_timeout_s <= 0:
        raise ConfigError("ue.connect_timeout_s must be > 0")
    if config.ue.ping_interval_s <= 0:
        raise ConfigError("ue.ping_interval_s must be > 0")

    if config.ue.reconnect.initial_delay_s <= 0:
        raise ConfigError("ue.reconnect.initial_delay_s must be > 0")
    if config.ue.reconnect.max_delay_s <= 0:
        raise ConfigError("ue.reconnect.max_delay_s must be > 0")
    if config.ue.reconnect.max_delay_s < config.ue.reconnect.initial_delay_s:
        raise ConfigError("ue.reconnect.max_delay_s must be >= initial_delay_s")

    if config.request.default_timeout_ms <= 0:
        raise ConfigError("request.default_timeout_ms must be > 0")
    if config.catalog.refresh_interval_s < 0:
        raise ConfigError("catalog.refresh_interval_s must be >= 0")
    for required_tool in config.catalog.required_tools:
        if not required_tool.strip():
            raise ConfigError("catalog.required_tools must not contain empty tool names")
    if config.retry.transient_max_attempts <= 0:
        raise ConfigError("retry.transient_max_attempts must be > 0")
    if config.retry.backoff_initial_s <= 0:
        raise ConfigError("retry.backoff_initial_s must be > 0")
    if config.retry.backoff_max_s <= 0:
        raise ConfigError("retry.backoff_max_s must be > 0")
    if config.retry.backoff_max_s < config.retry.backoff_initial_s:
        raise ConfigError("retry.backoff_max_s must be >= backoff_initial_s")
    if config.metrics.log_interval_s < 0:
        raise ConfigError("metrics.log_interval_s must be >= 0")


def _parse_required_tools(raw_value: Any) -> tuple[str, ...]:
    if raw_value is None:
        return CatalogConfig.required_tools
    if not isinstance(raw_value, list):
        raise ConfigError("'catalog.required_tools' must be an array of strings.")

    normalized: list[str] = []
    seen: set[str] = set()
    for item in raw_value:
        if not isinstance(item, str):
            raise ConfigError("'catalog.required_tools' must be an array of strings.")
        stripped = item.strip()
        if not stripped:
            continue
        if stripped in seen:
            continue
        seen.add(stripped)
        normalized.append(stripped)
    return tuple(normalized)
