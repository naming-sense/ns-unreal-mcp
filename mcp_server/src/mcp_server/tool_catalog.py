from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from mcp_server.mcp_facade import MCPFacade


@dataclass(frozen=True)
class ToolDefinition:
    name: str
    domain: str
    version: str
    enabled: bool
    write: bool
    params_schema: dict[str, Any] | None
    result_schema: dict[str, Any] | None


class ToolCatalog:
    def __init__(self) -> None:
        self._tools_by_name: dict[str, ToolDefinition] = {}
        self._schema_hash: str = ""
        self._protocol_version: str = "unreal-mcp/1.0"
        self._capabilities: tuple[str, ...] = ()

    @property
    def schema_hash(self) -> str:
        return self._schema_hash

    @property
    def protocol_version(self) -> str:
        return self._protocol_version

    @property
    def tools(self) -> list[ToolDefinition]:
        return sorted(self._tools_by_name.values(), key=lambda x: x.name)

    @property
    def capabilities(self) -> tuple[str, ...]:
        return self._capabilities

    def get_tool(self, name: str) -> ToolDefinition | None:
        return self._tools_by_name.get(name)

    async def refresh(self, facade: MCPFacade, *, include_schemas: bool = True) -> None:
        response = await facade.call_tool(
            tool="tools.list",
            params={"include_schemas": include_schemas},
        )

        self._protocol_version = str(response.result.get("protocol_version", "unreal-mcp/1.0"))
        self._schema_hash = str(response.result.get("schema_hash", ""))
        self._capabilities = _normalize_capabilities(response.result.get("capabilities"))
        self._tools_by_name.clear()

        for tool_value in response.result.get("tools", []):
            if not isinstance(tool_value, dict):
                continue

            name = str(tool_value.get("name", ""))
            if not name:
                continue

            self._tools_by_name[name] = ToolDefinition(
                name=name,
                domain=str(tool_value.get("domain", "")),
                version=str(tool_value.get("version", "1.0.0")),
                enabled=bool(tool_value.get("enabled", True)),
                write=bool(tool_value.get("write", False)),
                params_schema=tool_value.get("params_schema")
                if isinstance(tool_value.get("params_schema"), dict)
                else None,
                result_schema=tool_value.get("result_schema")
                if isinstance(tool_value.get("result_schema"), dict)
                else None,
            )


def _normalize_capabilities(raw_value: Any) -> tuple[str, ...]:
    if not isinstance(raw_value, list):
        return ()

    normalized: list[str] = []
    seen: set[str] = set()
    for entry in raw_value:
        if not isinstance(entry, str):
            continue
        capability = entry.strip()
        if not capability or capability in seen:
            continue
        seen.add(capability)
        normalized.append(capability)
    return tuple(normalized)
