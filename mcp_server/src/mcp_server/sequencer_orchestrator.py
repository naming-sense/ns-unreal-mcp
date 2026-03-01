from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from mcp_server.mcp_facade import ToolCallResult
from mcp_server.tool_passthrough import MCPPassThroughService, UnknownToolError


@dataclass(frozen=True)
class _ResolvedSequencerAction:
    requested_kind: str
    delegated_tool: str
    params: dict[str, Any]
    fallback_note: str


class SequencerOrchestrationService:
    VIRTUAL_TOOL_NAME = "seq.workflow.compose"
    CORE_CAPABILITY = "sequencer_core_v1"
    KEYS_CAPABILITY = "sequencer_keys_v1"

    _SAVE_AWARE_TOOLS = {
        "seq.asset.create",
        "seq.binding.add",
        "seq.binding.remove",
        "seq.track.add",
        "seq.track.remove",
        "seq.section.add",
        "seq.section.patch",
        "seq.section.remove",
        "seq.key.set",
        "seq.key.remove",
        "seq.key.bulk_set",
        "seq.playback.patch",
        "seq.save",
    }

    def __init__(self, pass_through: MCPPassThroughService) -> None:
        self._pass_through = pass_through

    def list_virtual_tools(self) -> list[dict[str, Any]]:
        return [
            {
                "name": self.VIRTUAL_TOOL_NAME,
                "description": (
                    "[seq] server-side orchestration helper. "
                    "Composes sequencer lifecycle/structure/key operations with capability-aware fallback."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "object_path": {"type": "string"},
                        "actions": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "kind": {
                                        "type": "string",
                                        "enum": [
                                            "asset.create",
                                            "asset.load",
                                            "inspect",
                                            "binding.list",
                                            "track.list",
                                            "section.list",
                                            "channel.list",
                                            "binding.add",
                                            "binding.remove",
                                            "track.add",
                                            "track.remove",
                                            "section.add",
                                            "section.patch",
                                            "section.remove",
                                            "key.set",
                                            "key.remove",
                                            "key.bulk_set",
                                            "object.inspect",
                                            "object.patch.v2",
                                            "playback.patch",
                                            "save",
                                            "validate",
                                        ],
                                    },
                                    "args": {"type": "object"},
                                },
                                "required": ["kind", "args"],
                                "additionalProperties": False,
                            },
                        },
                        "auto_save": {"type": "boolean", "default": False},
                        "continue_on_error": {"type": "boolean", "default": False},
                    },
                    "required": ["actions"],
                    "additionalProperties": False,
                },
                "annotations": {
                    "readOnlyHint": False,
                },
            }
        ]

    def is_virtual_tool(self, tool_name: str) -> bool:
        return tool_name == self.VIRTUAL_TOOL_NAME

    async def call_virtual_tool(
        self,
        *,
        tool_name: str,
        arguments: dict[str, Any],
        request_id: str | None = None,
    ) -> ToolCallResult:
        if tool_name != self.VIRTUAL_TOOL_NAME:
            raise UnknownToolError(f"Unknown virtual tool: {tool_name}")
        return await self._call_workflow_compose(arguments=arguments, request_id=request_id)

    async def _call_workflow_compose(
        self,
        *,
        arguments: dict[str, Any],
        request_id: str | None,
    ) -> ToolCallResult:
        actions = arguments.get("actions")
        if not isinstance(actions, list) or len(actions) == 0:
            raise ValueError("seq.workflow.compose requires non-empty array 'actions'.")

        current_object_path = arguments.get("object_path")
        if current_object_path is not None and not isinstance(current_object_path, str):
            raise ValueError("seq.workflow.compose optional 'object_path' must be string.")
        current_object_path = current_object_path.strip() if isinstance(current_object_path, str) else ""

        auto_save = bool(arguments.get("auto_save", False))
        continue_on_error = bool(arguments.get("continue_on_error", False))

        steps: list[dict[str, Any]] = []
        errors: list[dict[str, Any]] = []
        touched_packages: list[str] = []

        for index, action in enumerate(actions):
            resolved = self._resolve_action(action)
            params = dict(resolved.params)
            if current_object_path and "object_path" not in params and resolved.requested_kind != "asset.create":
                params["object_path"] = current_object_path

            if auto_save and resolved.delegated_tool in self._SAVE_AWARE_TOOLS:
                save = params.get("save")
                if not isinstance(save, dict):
                    save = {}
                save["auto_save"] = True
                params["save"] = save

            step_request_id = (
                f"{request_id}-step{index + 1}"
                if isinstance(request_id, str) and request_id
                else None
            )
            call_result = await self._pass_through.call_tool(
                tool=resolved.delegated_tool,
                params=params,
                request_id=step_request_id,
            )

            step_payload = {
                "index": index + 1,
                "requested_kind": resolved.requested_kind,
                "delegated_tool": resolved.delegated_tool,
                "request_id": call_result.request_id,
                "status": call_result.status,
                "ok": call_result.ok and call_result.status != "error",
                "fallback": resolved.fallback_note,
                "diagnostics": call_result.diagnostics,
            }
            steps.append(step_payload)

            result_object = call_result.result
            if isinstance(result_object.get("object_path"), str) and result_object.get("object_path"):
                current_object_path = str(result_object["object_path"])
            result_packages = result_object.get("touched_packages")
            if isinstance(result_packages, list):
                for package in result_packages:
                    if isinstance(package, str) and package and package not in touched_packages:
                        touched_packages.append(package)

            if not step_payload["ok"]:
                errors.append(
                    {
                        "code": "MCP.SERVER.SEQ_WORKFLOW_STEP_FAILED",
                        "message": "Workflow step failed.",
                        "detail": (
                            f"index={index + 1}, kind={resolved.requested_kind}, "
                            f"tool={resolved.delegated_tool}, status={call_result.status}"
                        ),
                        "retriable": False,
                    }
                )
                if not continue_on_error:
                    break

        workflow_result = {
            "object_path": current_object_path,
            "step_count": len(steps),
            "failed_count": len(errors),
            "steps": steps,
            "touched_packages": touched_packages,
            "strategy": {
                "auto_save": auto_save,
                "continue_on_error": continue_on_error,
                "sequencer_core_capability": self._pass_through.has_capability(self.CORE_CAPABILITY),
                "sequencer_keys_capability": self._pass_through.has_capability(self.KEYS_CAPABILITY),
                "capabilities": list(self._pass_through.capabilities),
            },
        }

        status = "error" if errors else "ok"
        diagnostics: dict[str, Any] = {
            "errors": errors,
            "warnings": [],
            "infos": [],
        }
        return ToolCallResult(
            ok=not errors,
            status=status,
            request_id=request_id or "seq-workflow",
            result=workflow_result,
            diagnostics=diagnostics,
            raw_envelope={
                "status": status,
                "result": workflow_result,
                "diagnostics": diagnostics,
            },
        )

    def _resolve_action(self, action: Any) -> _ResolvedSequencerAction:
        if not isinstance(action, dict):
            raise ValueError("Each action must be an object with kind/args.")

        kind_value = action.get("kind")
        if not isinstance(kind_value, str) or not kind_value.strip():
            raise ValueError("Action kind must be a non-empty string.")
        requested_kind = kind_value.strip()

        args_value = action.get("args")
        if not isinstance(args_value, dict):
            raise ValueError(f"Action '{requested_kind}' requires object 'args'.")
        params = dict(args_value)

        direct_map = {
            "asset.create": "seq.asset.create",
            "asset.load": "seq.asset.load",
            "inspect": "seq.inspect",
            "binding.list": "seq.binding.list",
            "track.list": "seq.track.list",
            "section.list": "seq.section.list",
            "channel.list": "seq.channel.list",
            "binding.add": "seq.binding.add",
            "binding.remove": "seq.binding.remove",
            "track.add": "seq.track.add",
            "track.remove": "seq.track.remove",
            "section.add": "seq.section.add",
            "section.patch": "seq.section.patch",
            "section.remove": "seq.section.remove",
            "key.set": "seq.key.set",
            "key.remove": "seq.key.remove",
            "object.inspect": "seq.object.inspect",
            "object.patch.v2": "seq.object.patch.v2",
            "playback.patch": "seq.playback.patch",
            "save": "seq.save",
            "validate": "seq.validate",
        }

        if requested_kind == "key.bulk_set":
            if self._pass_through.has_capability(self.KEYS_CAPABILITY) and self._has_tool("seq.key.bulk_set"):
                return _ResolvedSequencerAction(requested_kind, "seq.key.bulk_set", params, "")
            self._require_tool("seq.key.set")
            return _ResolvedSequencerAction(
                requested_kind,
                "seq.key.set",
                self._translate_bulk_to_single_key(params),
                "fallback: seq.key.bulk_set -> seq.key.set(first key)",
            )

        delegated_tool = direct_map.get(requested_kind)
        if delegated_tool is None:
            raise ValueError(f"Unsupported action kind: {requested_kind}")

        self._require_tool(delegated_tool)
        return _ResolvedSequencerAction(requested_kind, delegated_tool, params, "")

    def _translate_bulk_to_single_key(self, params: dict[str, Any]) -> dict[str, Any]:
        keys = params.get("keys")
        if not isinstance(keys, list) or len(keys) == 0 or not isinstance(keys[0], dict):
            raise ValueError("key.bulk_set action requires non-empty keys array for fallback.")

        first_key = dict(keys[0])
        translated = dict(params)
        translated.pop("keys", None)
        translated.update(first_key)
        return translated

    def _has_tool(self, tool_name: str) -> bool:
        return any(tool.name == tool_name and tool.enabled for tool in self._pass_through.list_tools())

    def _require_tool(self, tool_name: str) -> None:
        if not self._has_tool(tool_name):
            raise UnknownToolError(f"Required tool is not available: {tool_name}")
