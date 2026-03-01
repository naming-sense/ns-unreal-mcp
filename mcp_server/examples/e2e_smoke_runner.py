#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass, replace
import json
from pathlib import Path
import time
from typing import Any, Callable
import uuid

from mcp_server.config import AppConfig, ConfigError, load_config
from mcp_server.event_router import EventRouter
from mcp_server.logging_setup import configure_logging
from mcp_server.mcp_facade import MCPFacade, ToolCallResult
from mcp_server.metrics import RuntimeMetrics
from mcp_server.request_broker import RequestBroker
from mcp_server.tool_catalog import ToolCatalog
from mcp_server.tool_passthrough import MCPPassThroughService
from mcp_server.ue_transport import UeWsTransport
from mcp_server.ws_endpoint import resolve_ws_endpoint


@dataclass(frozen=True)
class ScenarioSpec:
    key: str
    tool: str
    path_arg: str | None
    discover_class_paths: tuple[str, ...]
    discover_name_glob: str | None
    params_builder: Callable[[str], dict[str, Any]]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="E2E smoke runner for material/niagara/umg/sequencer(+extended) + asset lifecycle tools"
    )
    parser.add_argument("--config", type=Path, default=None, help="Path to YAML config")
    parser.add_argument("--log-level", type=str, default=None, help="Override log level")
    parser.add_argument("--path-glob", type=str, default="/Game/**", help="Asset search path glob")
    parser.add_argument("--material-path", type=str, default=None, help="Material instance object path")
    parser.add_argument("--niagara-path", type=str, default=None, help="Niagara system object path")
    parser.add_argument("--umg-path", type=str, default=None, help="Widget blueprint object path")
    parser.add_argument("--sequencer-path", type=str, default=None, help="Level sequence object path")
    parser.add_argument(
        "--skip-asset-lifecycle",
        action="store_true",
        help="Skip asset lifecycle scenario (duplicate/rename/create/delete)",
    )
    parser.add_argument(
        "--asset-lifecycle-root-path",
        type=str,
        default="/Game/MCPRuntimeE2E",
        help="Root path for temporary lifecycle assets",
    )
    parser.add_argument(
        "--asset-lifecycle-source",
        type=str,
        default="/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial",
        help="Source object path used by asset.duplicate",
    )
    parser.add_argument(
        "--asset-lifecycle-class-path",
        type=str,
        default="/Script/Engine.MaterialInstanceConstant",
        help="Class path used by asset.create",
    )
    parser.add_argument(
        "--asset-lifecycle-keep-assets",
        action="store_true",
        help="Keep created lifecycle assets instead of deleting them",
    )
    parser.add_argument(
        "--no-auto-discover",
        action="store_true",
        help="Disable asset.find fallback when object paths are omitted",
    )
    parser.add_argument(
        "--require-all",
        action="store_true",
        help="Fail when any scenario is skipped (missing asset path)",
    )
    parser.add_argument("--timeout-ms", type=int, default=None, help="Per-tool timeout override")
    parser.add_argument(
        "--stream-events",
        action="store_true",
        help="Collect event stream while each tool call is running",
    )
    parser.add_argument(
        "--print-event-lines",
        action="store_true",
        help="Print every streamed event line (with --stream-events)",
    )
    parser.add_argument(
        "--include-result",
        action="store_true",
        help="Include tool result payload in final JSON (may be large)",
    )
    return parser.parse_args()


def _scenario_specs() -> tuple[ScenarioSpec, ...]:
    return (
        ScenarioSpec(
            key="material",
            tool="mat.instance.params.get",
            path_arg="material_path",
            discover_class_paths=(
                "/Script/Engine.MaterialInstanceConstant",
                "/Script/Engine.MaterialInstance",
            ),
            discover_name_glob="MI_*",
            params_builder=lambda path: {
                "object_path": path,
                "include_inherited": True,
            },
        ),
        ScenarioSpec(
            key="niagara",
            tool="niagara.stack.list",
            path_arg="niagara_path",
            discover_class_paths=("/Script/Niagara.NiagaraSystem",),
            discover_name_glob="NS_*",
            params_builder=lambda path: {
                "object_path": path,
            },
        ),
        ScenarioSpec(
            key="umg",
            tool="umg.tree.get",
            path_arg="umg_path",
            discover_class_paths=(
                "/Script/UMGEditor.WidgetBlueprint",
                "/Script/UMG.WidgetBlueprint",
            ),
            discover_name_glob="WBP_*",
            params_builder=lambda path: {
                "object_path": path,
                "depth": 3,
            },
        ),
        ScenarioSpec(
            key="sequencer",
            tool="seq.inspect",
            path_arg="sequencer_path",
            discover_class_paths=("/Script/LevelSequence.LevelSequence",),
            discover_name_glob="LS_*",
            params_builder=lambda path: {
                "object_path": path,
            },
        ),
    )


async def _discover_object_path(
    pass_through: MCPPassThroughService,
    *,
    path_glob: str,
    class_paths: tuple[str, ...],
    name_glob: str | None,
    timeout_ms: int | None,
) -> str | None:
    if class_paths:
        discovery_result = await pass_through.call_tool(
            tool="asset.find",
            params={
                "path_glob": path_glob,
                "class_path_in": list(class_paths),
                "limit": 1,
            },
            timeout_ms=timeout_ms,
        )
        found = _extract_first_asset_path(discovery_result)
        if found:
            return found

    if name_glob:
        discovery_result = await pass_through.call_tool(
            tool="asset.find",
            params={
                "path_glob": path_glob,
                "name_glob": name_glob,
                "limit": 1,
            },
            timeout_ms=timeout_ms,
        )
        found = _extract_first_asset_path(discovery_result)
        if found:
            return found

    return None


def _extract_first_asset_path(result: ToolCallResult) -> str | None:
    assets = result.result.get("assets")
    if not isinstance(assets, list):
        return None
    for asset in assets:
        if not isinstance(asset, dict):
            continue
        object_path = asset.get("object_path")
        if isinstance(object_path, str) and object_path:
            return object_path
    return None


def _build_success_summary(spec: ScenarioSpec, result: ToolCallResult) -> dict[str, Any]:
    summary: dict[str, Any] = {}
    if spec.key == "material":
        params = result.result.get("params")
        summary["param_count"] = len(params) if isinstance(params, list) else 0
    elif spec.key == "niagara":
        modules = result.result.get("modules")
        summary["module_count"] = len(modules) if isinstance(modules, list) else 0
    elif spec.key == "umg":
        nodes = result.result.get("nodes")
        summary["node_count"] = len(nodes) if isinstance(nodes, list) else 0
    elif spec.key == "sequencer":
        summary["binding_count"] = int(result.result.get("binding_count", 0))
        summary["master_track_count"] = int(result.result.get("master_track_count", 0))
    return summary


def _tool_is_available(pass_through: MCPPassThroughService, tool: str) -> bool:
    return any(defn.name == tool and defn.enabled for defn in pass_through.list_tools())


def _find_umg_event_candidate(nodes: list[dict[str, Any]]) -> tuple[dict[str, Any], str] | None:
    event_map: tuple[tuple[str, str], ...] = (
        ("/Script/UMG.Button", "OnClicked"),
        ("/Script/UMG.CheckBox", "OnCheckStateChanged"),
        ("/Script/UMG.EditableText", "OnTextChanged"),
        ("/Script/UMG.EditableTextBox", "OnTextChanged"),
        ("/Script/UMG.ComboBoxString", "OnSelectionChanged"),
        ("/Script/UMG.Slider", "OnValueChanged"),
        ("/Script/UMG.SpinBox", "OnValueChanged"),
    )
    for class_path, event_name in event_map:
        for node in nodes:
            if node.get("class_path") == class_path and isinstance(node.get("name"), str) and node["name"]:
                return node, event_name
    return None


async def _run_umg_extended_scenario(
    *,
    pass_through: MCPPassThroughService,
    object_path: str,
    timeout_ms: int | None,
    stream_events: bool,
    print_event_lines: bool,
    include_result: bool,
) -> dict[str, Any]:
    required_tools = (
        "umg.tree.get",
        "umg.widget.inspect",
        "umg.binding.list",
        "umg.animation.list",
        "umg.graph.summary",
    )
    missing_tools = [tool for tool in required_tools if not _tool_is_available(pass_through, tool)]
    if missing_tools:
        return {
            "scenario": "umg_extended",
            "status": "skipped",
            "reason": f"required tools missing: {', '.join(missing_tools)}",
            "missing_tools": missing_tools,
        }

    scenario_started = time.perf_counter()
    scenario_events: list[dict[str, Any]] = []
    steps: list[dict[str, Any]] = []

    async def _invoke(step_name: str, tool: str, params: dict[str, Any]) -> ToolCallResult:
        started = time.perf_counter()
        step_events: list[dict[str, Any]] = []
        try:
            if stream_events:

                def _on_event(event: dict[str, Any]) -> None:
                    scenario_events.append(event)
                    step_events.append(event)
                    if print_event_lines:
                        print(
                            json.dumps(
                                {
                                    "type": "event",
                                    "scenario": "umg_extended",
                                    "step": step_name,
                                    "event": event,
                                },
                                ensure_ascii=False,
                            ),
                            flush=True,
                        )

                result = await pass_through.call_tool_stream(
                    tool=tool,
                    params=params,
                    timeout_ms=timeout_ms,
                    on_event=_on_event,
                )
            else:
                result = await pass_through.call_tool(
                    tool=tool,
                    params=params,
                    timeout_ms=timeout_ms,
                )
        except Exception as exc:
            duration_ms = int((time.perf_counter() - started) * 1000)
            steps.append(
                {
                    "step": step_name,
                    "tool": tool,
                    "status": "error",
                    "duration_ms": duration_ms,
                    "event_count": len(step_events),
                    "error": {"type": exc.__class__.__name__, "message": str(exc)},
                }
            )
            raise

        duration_ms = int((time.perf_counter() - started) * 1000)
        step_payload: dict[str, Any] = {
            "step": step_name,
            "tool": tool,
            "request_id": result.request_id,
            "status": result.status,
            "duration_ms": duration_ms,
            "event_count": len(step_events),
            "diagnostics": result.diagnostics,
        }
        if include_result:
            step_payload["result"] = result.result
        steps.append(step_payload)
        return result

    try:
        tree_result = await _invoke(
            "tree.get",
            "umg.tree.get",
            {
                "object_path": object_path,
                "depth": 5,
                "include": {
                    "slot_summary": True,
                    "layout_summary": True,
                },
            },
        )
        if not tree_result.ok or tree_result.status == "error":
            raise RuntimeError("umg.tree.get failed in extended scenario")

        nodes_raw = tree_result.result.get("nodes")
        nodes: list[dict[str, Any]] = []
        if isinstance(nodes_raw, list):
            for node in nodes_raw:
                if isinstance(node, dict):
                    nodes.append(node)
        if len(nodes) == 0:
            raise RuntimeError("umg.tree.get returned no nodes")

        target_node = nodes[0]
        target_name = str(target_node.get("name", ""))
        if not target_name:
            raise RuntimeError("umg.tree.get node does not contain widget name")

        inspect_result = await _invoke(
            "widget.inspect",
            "umg.widget.inspect",
            {
                "object_path": object_path,
                "widget_ref": {"name": target_name},
                "depth": 2,
                "include": {
                    "properties": True,
                    "metadata": True,
                    "style": True,
                },
            },
        )
        if not inspect_result.ok or inspect_result.status == "error":
            raise RuntimeError("umg.widget.inspect failed in extended scenario")

        slot_target = next(
            (node for node in nodes if isinstance(node.get("slot_type"), str) and node["slot_type"]),
            None,
        )
        if slot_target is not None and _tool_is_available(pass_through, "umg.slot.inspect"):
            slot_result = await _invoke(
                "slot.inspect",
                "umg.slot.inspect",
                {
                    "object_path": object_path,
                    "widget_ref": {"name": slot_target["name"]},
                    "include": {
                        "layout": True,
                        "anchors": True,
                        "padding": True,
                        "alignment": True,
                        "zorder": True,
                    },
                },
            )
            if not slot_result.ok or slot_result.status == "error":
                raise RuntimeError("umg.slot.inspect failed in extended scenario")
        else:
            steps.append(
                {
                    "step": "slot.inspect",
                    "tool": "umg.slot.inspect",
                    "status": "skipped",
                    "reason": "slot target not found or tool unavailable",
                }
            )

        binding_list_result = await _invoke(
            "binding.list",
            "umg.binding.list",
            {"object_path": object_path},
        )
        if not binding_list_result.ok or binding_list_result.status == "error":
            raise RuntimeError("umg.binding.list failed in extended scenario")

        animation_list_result = await _invoke(
            "animation.list",
            "umg.animation.list",
            {"object_path": object_path},
        )
        if not animation_list_result.ok or animation_list_result.status == "error":
            raise RuntimeError("umg.animation.list failed in extended scenario")

        graph_summary_result = await _invoke(
            "graph.summary",
            "umg.graph.summary",
            {"object_path": object_path, "include_names": False},
        )
        if not graph_summary_result.ok or graph_summary_result.status == "error":
            raise RuntimeError("umg.graph.summary failed in extended scenario")

        if _tool_is_available(pass_through, "umg.widget.event.bind") and _tool_is_available(
            pass_through, "umg.widget.event.unbind"
        ):
            candidate = _find_umg_event_candidate(nodes)
            if candidate is not None:
                widget_node, event_name = candidate
                function_name = f"HandleMcpE2E_{widget_node['name']}_{event_name}"
                bind_result = await _invoke(
                    "widget.event.bind",
                    "umg.widget.event.bind",
                    {
                        "object_path": object_path,
                        "widget_ref": {"name": widget_node["name"]},
                        "event_name": event_name,
                        "function_name": function_name,
                        "compile_on_success": False,
                    },
                )
                if not bind_result.ok or bind_result.status == "error":
                    raise RuntimeError("umg.widget.event.bind failed in extended scenario")

                post_bind_summary = await _invoke(
                    "graph.summary.post_bind",
                    "umg.graph.summary",
                    {"object_path": object_path, "include_names": False},
                )
                if not post_bind_summary.ok or post_bind_summary.status == "error":
                    raise RuntimeError("umg.graph.summary post_bind failed in extended scenario")

                unbind_result = await _invoke(
                    "widget.event.unbind",
                    "umg.widget.event.unbind",
                    {
                        "object_path": object_path,
                        "widget_ref": {"name": widget_node["name"]},
                        "event_name": event_name,
                        "function_name": function_name,
                        "compile_on_success": False,
                    },
                )
                if not unbind_result.ok or unbind_result.status == "error":
                    raise RuntimeError("umg.widget.event.unbind failed in extended scenario")
            else:
                steps.append(
                    {
                        "step": "widget.event.bind",
                        "tool": "umg.widget.event.bind",
                        "status": "skipped",
                        "reason": "no compatible delegate widget found in tree",
                    }
                )
        else:
            steps.append(
                {
                    "step": "widget.event.bind",
                    "tool": "umg.widget.event.bind",
                    "status": "skipped",
                    "reason": "event bind tools are unavailable",
                }
            )

    except Exception as exc:
        return {
            "scenario": "umg_extended",
            "tool": "umg.workflow",
            "object_path": object_path,
            "status": "error",
            "duration_ms": int((time.perf_counter() - scenario_started) * 1000),
            "event_count": len(scenario_events),
            "error": {"type": exc.__class__.__name__, "message": str(exc)},
            "steps": steps,
        }

    return {
        "scenario": "umg_extended",
        "tool": "umg.workflow",
        "object_path": object_path,
        "status": "ok",
        "duration_ms": int((time.perf_counter() - scenario_started) * 1000),
        "event_count": len(scenario_events),
        "summary": {
            "step_count": len(steps),
        },
        "steps": steps,
    }


async def _run_sequencer_extended_scenario(
    *,
    pass_through: MCPPassThroughService,
    timeout_ms: int | None,
    stream_events: bool,
    print_event_lines: bool,
    include_result: bool,
    root_path: str,
    keep_assets: bool,
) -> dict[str, Any]:
    required_tools = (
        "seq.asset.create",
        "seq.inspect",
        "seq.binding.add",
        "seq.track.add",
        "seq.section.add",
        "seq.channel.list",
        "seq.key.set",
        "seq.playback.patch",
        "seq.save",
        "seq.validate",
        "asset.delete",
    )
    missing_tools = [tool for tool in required_tools if not _tool_is_available(pass_through, tool)]
    if missing_tools:
        return {
            "scenario": "sequencer_extended",
            "status": "skipped",
            "reason": f"required tools missing: {', '.join(missing_tools)}",
            "missing_tools": missing_tools,
        }

    scenario_started = time.perf_counter()
    scenario_events: list[dict[str, Any]] = []
    steps: list[dict[str, Any]] = []

    unique = uuid.uuid4().hex[:8].upper()
    package_path = f"{root_path.rstrip('/')}/LS_E2E_{unique}"
    asset_name = f"LS_E2E_{unique}"
    object_path = f"{package_path}.{asset_name}"

    async def _invoke(step_name: str, tool: str, params: dict[str, Any]) -> ToolCallResult:
        started = time.perf_counter()
        step_events: list[dict[str, Any]] = []
        try:
            if stream_events:

                def _on_event(event: dict[str, Any]) -> None:
                    scenario_events.append(event)
                    step_events.append(event)
                    if print_event_lines:
                        print(
                            json.dumps(
                                {
                                    "type": "event",
                                    "scenario": "sequencer_extended",
                                    "step": step_name,
                                    "event": event,
                                },
                                ensure_ascii=False,
                            ),
                            flush=True,
                        )

                result = await pass_through.call_tool_stream(
                    tool=tool,
                    params=params,
                    timeout_ms=timeout_ms,
                    on_event=_on_event,
                )
            else:
                result = await pass_through.call_tool(
                    tool=tool,
                    params=params,
                    timeout_ms=timeout_ms,
                )
        except Exception as exc:
            duration_ms = int((time.perf_counter() - started) * 1000)
            steps.append(
                {
                    "step": step_name,
                    "tool": tool,
                    "status": "error",
                    "duration_ms": duration_ms,
                    "event_count": len(step_events),
                    "error": {
                        "type": exc.__class__.__name__,
                        "message": str(exc),
                    },
                }
            )
            raise

        duration_ms = int((time.perf_counter() - started) * 1000)
        step_payload: dict[str, Any] = {
            "step": step_name,
            "tool": tool,
            "request_id": result.request_id,
            "status": result.status if result.ok and result.status != "error" else "error",
            "duration_ms": duration_ms,
            "event_count": len(step_events),
            "diagnostics": result.diagnostics,
        }
        if include_result:
            step_payload["result"] = result.result
        steps.append(step_payload)
        return result

    try:
        create_result = await _invoke(
            "create",
            "seq.asset.create",
            {
                "package_path": root_path,
                "asset_name": asset_name,
                "display_rate": {"numerator": 30, "denominator": 1},
                "tick_resolution": {"numerator": 24000, "denominator": 1},
            },
        )
        created_path = create_result.result.get("object_path")
        if isinstance(created_path, str) and created_path:
            object_path = created_path

        await _invoke("inspect", "seq.inspect", {"object_path": object_path})
        binding_result = await _invoke(
            "binding.add",
            "seq.binding.add",
            {
                "object_path": object_path,
                "mode": "possessable",
                "class_path": "/Script/Engine.Actor",
                "display_name": "E2EActor",
            },
        )
        binding_id = binding_result.result.get("binding_id")
        if not isinstance(binding_id, str) or not binding_id:
            raise RuntimeError("seq.binding.add did not return binding_id")

        track_result = await _invoke(
            "track.add",
            "seq.track.add",
            {
                "object_path": object_path,
                "binding_id": binding_id,
                "track_type": "float",
            },
        )
        track_id = track_result.result.get("track_id")
        if not isinstance(track_id, str) or not track_id:
            raise RuntimeError("seq.track.add did not return track_id")

        section_result = await _invoke(
            "section.add",
            "seq.section.add",
            {
                "track_id": track_id,
                "start_frame": 0,
                "end_frame": 120,
            },
        )
        section_id = section_result.result.get("section_id")
        if not isinstance(section_id, str) or not section_id:
            raise RuntimeError("seq.section.add did not return section_id")

        channel_result = await _invoke(
            "channel.list",
            "seq.channel.list",
            {
                "object_path": object_path,
                "section_id": section_id,
            },
        )
        channel_id = ""
        channels = channel_result.result.get("channels")
        if isinstance(channels, list) and channels:
            first_channel = channels[0]
            if isinstance(first_channel, dict):
                candidate = first_channel.get("channel_id")
                if isinstance(candidate, str):
                    channel_id = candidate

        if channel_id:
            await _invoke(
                "key.set.0",
                "seq.key.set",
                {
                    "channel_id": channel_id,
                    "frame": 0,
                    "value": 0.0,
                },
            )
            await _invoke(
                "key.set.120",
                "seq.key.set",
                {
                    "channel_id": channel_id,
                    "frame": 120,
                    "value": 1.0,
                    "interp": "linear",
                },
            )
        else:
            steps.append(
                {
                    "step": "key.set",
                    "tool": "seq.key.set",
                    "status": "skipped",
                    "reason": "channel.list returned no channel_id",
                }
            )

        await _invoke(
            "playback.patch",
            "seq.playback.patch",
            {
                "object_path": object_path,
                "start_frame": 0,
                "end_frame": 120,
            },
        )
        await _invoke("save", "seq.save", {"object_path": object_path})
        await _invoke("validate", "seq.validate", {"object_path": object_path})
    except Exception as exc:
        return {
            "scenario": "sequencer_extended",
            "status": "error",
            "duration_ms": int((time.perf_counter() - scenario_started) * 1000),
            "event_count": len(scenario_events),
            "error": {
                "type": exc.__class__.__name__,
                "message": str(exc),
            },
            "summary": {
                "object_path": object_path,
                "keep_assets": keep_assets,
            },
            "steps": steps,
        }
    finally:
        if not keep_assets:
            try:
                delete_preview = await _invoke(
                    "delete.preview",
                    "asset.delete",
                    {
                        "object_paths": [object_path],
                        "mode": "preview",
                        "fail_if_referenced": False,
                    },
                )
                confirm_token = delete_preview.result.get("confirm_token")
                if isinstance(confirm_token, str) and confirm_token:
                    await _invoke(
                        "delete.apply",
                        "asset.delete",
                        {
                            "object_paths": [object_path],
                            "mode": "apply",
                            "fail_if_referenced": False,
                            "confirm_token": confirm_token,
                        },
                    )
            except Exception:
                pass

    return {
        "scenario": "sequencer_extended",
        "status": "ok",
        "duration_ms": int((time.perf_counter() - scenario_started) * 1000),
        "event_count": len(scenario_events),
        "summary": {
            "object_path": object_path,
            "keep_assets": keep_assets,
            "step_count": len(steps),
        },
        "steps": steps,
    }


async def _run_asset_lifecycle_scenario(
    *,
    pass_through: MCPPassThroughService,
    timeout_ms: int | None,
    stream_events: bool,
    print_event_lines: bool,
    include_result: bool,
    root_path: str,
    duplicate_source_path: str,
    create_class_path: str,
    keep_assets: bool,
) -> dict[str, Any]:
    required_tools = (
        "asset.duplicate",
        "asset.rename",
        "asset.create",
        "asset.delete",
        "asset.load",
    )
    missing_tools = [tool for tool in required_tools if not _tool_is_available(pass_through, tool)]
    if missing_tools:
        return {
            "scenario": "asset_lifecycle",
            "status": "skipped",
            "reason": f"required tools missing: {', '.join(missing_tools)}",
            "required_tools": list(required_tools),
            "missing_tools": missing_tools,
        }

    scenario_started = time.perf_counter()
    scenario_events: list[dict[str, Any]] = []
    steps: list[dict[str, Any]] = []

    suffix = uuid.uuid4().hex[:8].upper()
    duplicate_name = f"MCP_E2E_Duplicate_{suffix}"
    renamed_name = f"MCP_E2E_Renamed_{suffix}"
    created_name = f"MCP_E2E_Created_{suffix}"
    duplicate_path = f"{root_path}/{duplicate_name}.{duplicate_name}"
    renamed_path = f"{root_path}/{renamed_name}.{renamed_name}"
    created_path = f"{root_path}/{created_name}.{created_name}"

    async def _invoke(step_name: str, tool: str, params: dict[str, Any]) -> ToolCallResult:
        started = time.perf_counter()
        step_events: list[dict[str, Any]] = []
        try:
            if stream_events:

                def _on_event(event: dict[str, Any]) -> None:
                    scenario_events.append(event)
                    step_events.append(event)
                    if print_event_lines:
                        print(
                            json.dumps(
                                {
                                    "type": "event",
                                    "scenario": "asset_lifecycle",
                                    "step": step_name,
                                    "event": event,
                                },
                                ensure_ascii=False,
                            ),
                            flush=True,
                        )

                result = await pass_through.call_tool_stream(
                    tool=tool,
                    params=params,
                    timeout_ms=timeout_ms,
                    on_event=_on_event,
                )
            else:
                result = await pass_through.call_tool(
                    tool=tool,
                    params=params,
                    timeout_ms=timeout_ms,
                )
        except Exception as exc:
            duration_ms = int((time.perf_counter() - started) * 1000)
            steps.append(
                {
                    "step": step_name,
                    "tool": tool,
                    "status": "error",
                    "duration_ms": duration_ms,
                    "event_count": len(step_events),
                    "error": {"type": exc.__class__.__name__, "message": str(exc)},
                }
            )
            raise

        duration_ms = int((time.perf_counter() - started) * 1000)
        payload: dict[str, Any] = {
            "step": step_name,
            "tool": tool,
            "request_id": result.request_id,
            "status": result.status,
            "duration_ms": duration_ms,
            "event_count": len(step_events),
            "diagnostics": result.diagnostics,
        }
        if include_result:
            payload["result"] = result.result
        steps.append(payload)
        return result

    try:
        duplicate_result = await _invoke(
            "duplicate",
            "asset.duplicate",
            {
                "source_object_path": duplicate_source_path,
                "dest_package_path": root_path,
                "dest_asset_name": duplicate_name,
                "overwrite": False,
                "save": {"auto_save": keep_assets},
            },
        )
        if not duplicate_result.ok or duplicate_result.status == "error":
            raise RuntimeError("asset.duplicate failed")

        rename_result = await _invoke(
            "rename",
            "asset.rename",
            {
                "object_path": duplicate_path,
                "new_package_path": root_path,
                "new_asset_name": renamed_name,
                "fixup_redirectors": False,
                "save": {"auto_save": keep_assets},
            },
        )
        if not rename_result.ok or rename_result.status == "error":
            raise RuntimeError("asset.rename failed")

        create_result = await _invoke(
            "create",
            "asset.create",
            {
                "package_path": root_path,
                "asset_name": created_name,
                "asset_class_path": create_class_path,
                "overwrite": False,
                "save": {"auto_save": keep_assets},
            },
        )
        if not create_result.ok or create_result.status == "error":
            raise RuntimeError("asset.create failed")

        if keep_assets:
            for path in (renamed_path, created_path):
                load_existing = await _invoke(
                    f"verify.exists.{path.rsplit('/', 1)[-1]}",
                    "asset.load",
                    {"object_path": path},
                )
                if not load_existing.ok or load_existing.status == "error":
                    raise RuntimeError(f"asset.load should succeed for retained asset: {path}")
        else:
            preview_result = await _invoke(
                "delete.preview",
                "asset.delete",
                {
                    "object_paths": [renamed_path, created_path],
                    "mode": "preview",
                    "fail_if_referenced": True,
                },
            )
            if not preview_result.ok or preview_result.status == "error":
                raise RuntimeError("asset.delete preview failed")

            confirm_token = preview_result.result.get("confirm_token")
            if not isinstance(confirm_token, str) or not confirm_token:
                raise RuntimeError("asset.delete preview missing confirm_token")

            apply_result = await _invoke(
                "delete.apply",
                "asset.delete",
                {
                    "object_paths": [renamed_path, created_path],
                    "mode": "apply",
                    "fail_if_referenced": True,
                    "confirm_token": confirm_token,
                },
            )
            if not apply_result.ok or apply_result.status == "error":
                raise RuntimeError("asset.delete apply failed")

            for path in (renamed_path, created_path):
                load_after_delete = await _invoke(
                    f"verify.deleted.{path.rsplit('/', 1)[-1]}",
                    "asset.load",
                    {"object_path": path},
                )
                if load_after_delete.status != "error":
                    raise RuntimeError(f"asset.load should fail after delete: {path}")

    except Exception as exc:
        return {
            "scenario": "asset_lifecycle",
            "status": "error",
            "duration_ms": int((time.perf_counter() - scenario_started) * 1000),
            "event_count": len(scenario_events),
            "error": {"type": exc.__class__.__name__, "message": str(exc)},
            "summary": {
                "root_path": root_path,
                "duplicate_path": duplicate_path,
                "renamed_path": renamed_path,
                "created_path": created_path,
            },
            "steps": steps,
        }

    return {
        "scenario": "asset_lifecycle",
        "status": "ok",
        "duration_ms": int((time.perf_counter() - scenario_started) * 1000),
        "event_count": len(scenario_events),
        "summary": {
            "root_path": root_path,
            "duplicate_source": duplicate_source_path,
            "duplicate_path": duplicate_path,
            "renamed_path": renamed_path,
            "created_path": created_path,
            "kept_assets": keep_assets,
            "deleted_count": 0 if keep_assets else 2,
        },
        "steps": steps,
    }


async def run_e2e(
    config: AppConfig,
    *,
    material_path: str | None,
    niagara_path: str | None,
    umg_path: str | None,
    sequencer_path: str | None,
    path_glob: str,
    auto_discover: bool,
    require_all: bool,
    timeout_ms: int | None,
    stream_events: bool,
    print_event_lines: bool,
    include_result: bool,
    skip_asset_lifecycle: bool,
    asset_lifecycle_root_path: str,
    asset_lifecycle_source: str,
    asset_lifecycle_class_path: str,
    asset_lifecycle_keep_assets: bool,
) -> int:
    endpoint_resolution = resolve_ws_endpoint(config)
    config = replace(config, ue=replace(config.ue, ws_url=endpoint_resolution.ws_url))

    metrics = RuntimeMetrics() if config.metrics.enabled else None
    request_broker = RequestBroker(
        default_timeout_ms=config.request.default_timeout_ms,
        metrics=metrics,
    )
    event_router = EventRouter(metrics=metrics)
    transport = UeWsTransport(
        ws_url=config.ue.ws_url,
        request_broker=request_broker,
        event_router=event_router,
        connect_timeout_s=config.ue.connect_timeout_s,
        ping_interval_s=config.ue.ping_interval_s,
        reconnect_initial_delay_s=config.ue.reconnect.initial_delay_s,
        reconnect_max_delay_s=config.ue.reconnect.max_delay_s,
        metrics=metrics,
    )
    facade = MCPFacade(transport)
    catalog = ToolCatalog()
    pass_through = MCPPassThroughService(
        facade=facade,
        catalog=catalog,
        event_router=event_router,
        include_schemas=config.catalog.include_schemas,
        refresh_interval_s=config.catalog.refresh_interval_s,
        transient_max_attempts=config.retry.transient_max_attempts,
        retry_backoff_initial_s=config.retry.backoff_initial_s,
        retry_backoff_max_s=config.retry.backoff_max_s,
        metrics=metrics,
    )

    supplied_paths = {
        "material": material_path,
        "niagara": niagara_path,
        "umg": umg_path,
        "sequencer": sequencer_path,
    }
    resolved_paths: dict[str, str | None] = {
        "material": None,
        "niagara": None,
        "umg": None,
        "sequencer": None,
    }

    scenarios: list[dict[str, Any]] = []
    overall_ok = True

    await transport.start()
    try:
        await transport.wait_until_connected(timeout_s=config.ue.connect_timeout_s)
    except TimeoutError:
        print(
            json.dumps(
                {
                    "ok": False,
                    "error": {
                        "code": "MCP.SERVER.CONNECT_TIMEOUT",
                        "message": f"Failed to connect UE WS: {config.ue.ws_url}",
                        "retriable": True,
                    },
                    "scenarios": [],
                },
                ensure_ascii=False,
            )
        )
        await transport.stop()
        return 3

    try:
        await pass_through.start()

        for spec in _scenario_specs():
            object_path = supplied_paths[spec.key]
            discovery_used = False

            if not object_path and auto_discover:
                object_path = await _discover_object_path(
                    pass_through,
                    path_glob=path_glob,
                    class_paths=spec.discover_class_paths,
                    name_glob=spec.discover_name_glob,
                    timeout_ms=timeout_ms,
                )
                discovery_used = object_path is not None

            if not object_path:
                skipped = {
                    "scenario": spec.key,
                    "tool": spec.tool,
                    "status": "skipped",
                    "reason": "asset path not provided/found",
                    "auto_discover": auto_discover,
                }
                scenarios.append(skipped)
                if require_all:
                    overall_ok = False
                continue

            resolved_paths[spec.key] = object_path
            started = time.perf_counter()
            events: list[dict[str, Any]] = []

            try:
                if stream_events:

                    def _on_event(event: dict[str, Any]) -> None:
                        events.append(event)
                        if print_event_lines:
                            print(
                                json.dumps(
                                    {
                                        "type": "event",
                                        "scenario": spec.key,
                                        "event": event,
                                    },
                                    ensure_ascii=False,
                                ),
                                flush=True,
                            )

                    result = await pass_through.call_tool_stream(
                        tool=spec.tool,
                        params=spec.params_builder(object_path),
                        timeout_ms=timeout_ms,
                        on_event=_on_event,
                    )
                else:
                    result = await pass_through.call_tool(
                        tool=spec.tool,
                        params=spec.params_builder(object_path),
                        timeout_ms=timeout_ms,
                    )
            except Exception as exc:
                duration_ms = int((time.perf_counter() - started) * 1000)
                scenarios.append(
                    {
                        "scenario": spec.key,
                        "tool": spec.tool,
                        "object_path": object_path,
                        "status": "error",
                        "duration_ms": duration_ms,
                        "event_count": len(events),
                        "auto_discovered": discovery_used,
                        "error": {
                            "type": exc.__class__.__name__,
                            "message": str(exc),
                        },
                    }
                )
                overall_ok = False
                continue

            duration_ms = int((time.perf_counter() - started) * 1000)
            scenario_ok = result.ok and result.status != "error"
            if not scenario_ok:
                overall_ok = False

            scenario_payload: dict[str, Any] = {
                "scenario": spec.key,
                "tool": spec.tool,
                "request_id": result.request_id,
                "object_path": object_path,
                "status": result.status if scenario_ok else "error",
                "duration_ms": duration_ms,
                "event_count": len(events),
                "auto_discovered": discovery_used,
                "summary": _build_success_summary(spec, result),
                "diagnostics": result.diagnostics,
            }
            if include_result:
                scenario_payload["result"] = result.result
            scenarios.append(scenario_payload)

        resolved_umg_path = resolved_paths.get("umg")
        if isinstance(resolved_umg_path, str) and resolved_umg_path:
            umg_extended_result = await _run_umg_extended_scenario(
                pass_through=pass_through,
                object_path=resolved_umg_path,
                timeout_ms=timeout_ms,
                stream_events=stream_events,
                print_event_lines=print_event_lines,
                include_result=include_result,
            )
            scenarios.append(umg_extended_result)
            if umg_extended_result.get("status") == "error":
                overall_ok = False
            if require_all and umg_extended_result.get("status") == "skipped":
                overall_ok = False

        sequencer_extended_result = await _run_sequencer_extended_scenario(
            pass_through=pass_through,
            timeout_ms=timeout_ms,
            stream_events=stream_events,
            print_event_lines=print_event_lines,
            include_result=include_result,
            root_path=asset_lifecycle_root_path,
            keep_assets=asset_lifecycle_keep_assets,
        )
        scenarios.append(sequencer_extended_result)
        if sequencer_extended_result.get("status") == "error":
            overall_ok = False
        if require_all and sequencer_extended_result.get("status") == "skipped":
            overall_ok = False

        if not skip_asset_lifecycle:
            lifecycle_result = await _run_asset_lifecycle_scenario(
                pass_through=pass_through,
                timeout_ms=timeout_ms,
                stream_events=stream_events,
                print_event_lines=print_event_lines,
                include_result=include_result,
                root_path=asset_lifecycle_root_path,
                duplicate_source_path=asset_lifecycle_source,
                create_class_path=asset_lifecycle_class_path,
                keep_assets=asset_lifecycle_keep_assets,
            )
            scenarios.append(lifecycle_result)
            if lifecycle_result.get("status") == "error":
                overall_ok = False
            if require_all and lifecycle_result.get("status") == "skipped":
                overall_ok = False
    finally:
        await pass_through.stop()
        await transport.stop()

    payload: dict[str, Any] = {
        "ok": overall_ok,
        "auto_discover": auto_discover,
        "require_all": require_all,
        "path_glob": path_glob,
        "scenarios": scenarios,
    }
    if metrics is not None:
        payload["metrics"] = metrics.snapshot()
    print(json.dumps(payload, ensure_ascii=False))
    return 0 if overall_ok else 5


def main() -> int:
    args = parse_args()
    if args.print_event_lines and not args.stream_events:
        print("Argument error: --print-event-lines requires --stream-events.")
        return 2

    try:
        config = load_config(args.config)
    except ConfigError as exc:
        print(f"Config error: {exc}")
        return 2

    if args.log_level:
        config = replace(config, server=replace(config.server, log_level=args.log_level))

    configure_logging(config.server.log_level, json_logs=config.server.json_logs)

    try:
        return asyncio.run(
            run_e2e(
                config,
                material_path=args.material_path,
                niagara_path=args.niagara_path,
                umg_path=args.umg_path,
                sequencer_path=args.sequencer_path,
                path_glob=args.path_glob,
                auto_discover=not args.no_auto_discover,
                require_all=args.require_all,
                timeout_ms=args.timeout_ms,
                stream_events=args.stream_events,
                print_event_lines=args.print_event_lines,
                include_result=args.include_result,
                skip_asset_lifecycle=args.skip_asset_lifecycle,
                asset_lifecycle_root_path=args.asset_lifecycle_root_path,
                asset_lifecycle_source=args.asset_lifecycle_source,
                asset_lifecycle_class_path=args.asset_lifecycle_class_path,
                asset_lifecycle_keep_assets=args.asset_lifecycle_keep_assets,
            )
        )
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
