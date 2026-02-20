from __future__ import annotations

import json
from pathlib import Path

from mcp_server.config import AppConfig, UeConfig
from mcp_server.ws_endpoint import resolve_ws_endpoint


def test_resolve_ws_endpoint_prefers_env_override(tmp_path: Path) -> None:
    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:19090"))

    resolved = resolve_ws_endpoint(
        config,
        env={"UE_MCP_WS_URL": "ws://10.0.0.10:19190"},
        cwd=tmp_path,
    )

    assert resolved.ws_url == "ws://10.0.0.10:19190"
    assert resolved.source == "env:UE_MCP_WS_URL"
    assert resolved.connection_file is None


def test_resolve_ws_endpoint_from_connection_file_transport_fields(tmp_path: Path) -> None:
    connection_file = tmp_path / "Saved" / "UnrealMCP" / "connection.json"
    connection_file.parent.mkdir(parents=True, exist_ok=True)
    connection_file.write_text(
        json.dumps(
            {
                "version": 1,
                "transport": {
                    "bind_address": "0.0.0.0",
                    "port": 19090,
                },
            }
        ),
        encoding="utf-8",
    )

    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:19090"))
    resolved = resolve_ws_endpoint(
        config,
        env={"UE_MCP_CONNECTION_FILE": str(connection_file)},
        cwd=tmp_path,
    )

    assert resolved.ws_url == "ws://127.0.0.1:19090"
    assert resolved.source == "connection_file"
    assert resolved.connection_file == str(connection_file)


def test_resolve_ws_endpoint_from_project_root_connection_file(tmp_path: Path) -> None:
    project_root = tmp_path / "DemoProject"
    project_root.mkdir(parents=True, exist_ok=True)
    (project_root / "DemoProject.uproject").write_text("{}", encoding="utf-8")

    connection_file = project_root / "Saved" / "UnrealMCP" / "connection.json"
    connection_file.parent.mkdir(parents=True, exist_ok=True)
    connection_file.write_text(
        json.dumps(
            {
                "ws_url": "ws://172.16.1.20:19095",
            }
        ),
        encoding="utf-8",
    )

    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:19090"))
    resolved = resolve_ws_endpoint(
        config,
        env={"UE_MCP_PROJECT_ROOT": str(project_root)},
        cwd=tmp_path,
    )

    assert resolved.ws_url == "ws://172.16.1.20:19095"
    assert resolved.source == "connection_file"
    assert resolved.connection_file == str(connection_file)


def test_resolve_ws_endpoint_falls_back_to_config_ws_url(tmp_path: Path) -> None:
    config = AppConfig(ue=UeConfig(ws_url="ws://127.0.0.1:19090"))

    resolved = resolve_ws_endpoint(
        config,
        env={"UE_MCP_WS_URL": "not-a-ws-url"},
        cwd=tmp_path,
    )

    assert resolved.ws_url == "ws://127.0.0.1:19090"
    assert resolved.source == "config:ue.ws_url"
    assert resolved.connection_file is None
