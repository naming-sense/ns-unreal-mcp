from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from mcp_server.mcp_facade import ToolCallResult
from mcp_server.tool_passthrough import MCPPassThroughService, UnknownToolError


@dataclass(frozen=True)
class _ResolvedAction:
    requested_kind: str
    delegated_tool: str
    params: dict[str, Any]
    fallback_note: str


class UMGOrchestrationService:
    VIRTUAL_TOOL_NAME = "umg.workflow.compose"
    K2_EVENT_CAPABILITY = "umg_widget_event_k2_v1"

    _COMPILE_AWARE_TOOLS = {
        "umg.widget.add",
        "umg.widget.remove",
        "umg.widget.reparent",
        "umg.widget.patch",
        "umg.widget.patch.v2",
        "umg.slot.patch",
        "umg.slot.patch.v2",
        "umg.binding.set",
        "umg.binding.clear",
        "umg.widget.event.bind",
        "umg.widget.event.unbind",
        "umg.blueprint.patch",
    }

    _SAVE_AWARE_TOOLS = {
        "umg.widget.add",
        "umg.widget.remove",
        "umg.widget.reparent",
        "umg.widget.patch",
        "umg.widget.patch.v2",
        "umg.slot.patch",
        "umg.slot.patch.v2",
        "umg.binding.set",
        "umg.binding.clear",
        "umg.widget.event.bind",
        "umg.widget.event.unbind",
        "umg.blueprint.patch",
    }

    def __init__(self, pass_through: MCPPassThroughService) -> None:
        self._pass_through = pass_through

    def list_virtual_tools(self) -> list[dict[str, Any]]:
        return [
            {
                "name": self.VIRTUAL_TOOL_NAME,
                "description": (
                    "[umg] server-side orchestration helper. "
                    "Composes multiple UMG operations with capability-aware fallback."
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
                                            "widget.add",
                                            "widget.remove",
                                            "widget.reparent",
                                            "widget.patch",
                                            "slot.patch",
                                            "widget.event.bind",
                                            "widget.event.unbind",
                                            "binding.set",
                                            "binding.clear",
                                            "blueprint.patch",
                                        ],
                                    },
                                    "args": {"type": "object"},
                                },
                                "required": ["kind", "args"],
                                "additionalProperties": False,
                            },
                        },
                        "prefer_v2": {"type": "boolean", "default": True},
                        "compile_on_finish": {"type": "boolean", "default": True},
                        "auto_save": {"type": "boolean", "default": False},
                        "continue_on_error": {"type": "boolean", "default": False},
                    },
                    "required": ["object_path", "actions"],
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
        object_path = arguments.get("object_path")
        if not isinstance(object_path, str) or not object_path.strip():
            raise ValueError("umg.workflow.compose requires non-empty string 'object_path'.")
        object_path = object_path.strip()

        actions = arguments.get("actions")
        if not isinstance(actions, list) or len(actions) == 0:
            raise ValueError("umg.workflow.compose requires non-empty array 'actions'.")

        prefer_v2 = bool(arguments.get("prefer_v2", True))
        compile_on_finish = bool(arguments.get("compile_on_finish", True))
        auto_save = bool(arguments.get("auto_save", False))
        continue_on_error = bool(arguments.get("continue_on_error", False))

        resolved_actions: list[_ResolvedAction] = []
        for action in actions:
            resolved_actions.append(
                self._resolve_action(
                    object_path=object_path,
                    action=action,
                    prefer_v2=prefer_v2,
                )
            )

        steps: list[dict[str, Any]] = []
        touched_packages: list[str] = []
        errors: list[dict[str, Any]] = []

        for index, resolved in enumerate(resolved_actions):
            params = dict(resolved.params)
            is_last_action = index == len(resolved_actions) - 1

            if resolved.delegated_tool in self._COMPILE_AWARE_TOOLS:
                params["compile_on_success"] = compile_on_finish and is_last_action

            if auto_save and resolved.delegated_tool in self._SAVE_AWARE_TOOLS:
                save_object = params.get("save")
                if not isinstance(save_object, dict):
                    save_object = {}
                save_object["auto_save"] = True
                params["save"] = save_object

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
            step_result_packages = call_result.result.get("touched_packages")
            if isinstance(step_result_packages, list):
                for package_name in step_result_packages:
                    if isinstance(package_name, str) and package_name and package_name not in touched_packages:
                        touched_packages.append(package_name)
            steps.append(step_payload)

            if not step_payload["ok"]:
                errors.append(
                    {
                        "code": "MCP.SERVER.UMG_WORKFLOW_STEP_FAILED",
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
            "object_path": object_path,
            "step_count": len(steps),
            "failed_count": len(errors),
            "steps": steps,
            "touched_packages": touched_packages,
            "strategy": {
                "prefer_v2": prefer_v2,
                "compile_on_finish": compile_on_finish,
                "auto_save": auto_save,
                "continue_on_error": continue_on_error,
                "k2_event_capability": self._pass_through.has_capability(self.K2_EVENT_CAPABILITY),
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
            request_id=request_id or "umg-workflow",
            result=workflow_result,
            diagnostics=diagnostics,
            raw_envelope={
                "status": status,
                "result": workflow_result,
                "diagnostics": diagnostics,
            },
        )

    def _resolve_action(
        self,
        *,
        object_path: str,
        action: Any,
        prefer_v2: bool,
    ) -> _ResolvedAction:
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
        params.setdefault("object_path", object_path)

        if requested_kind == "widget.patch":
            delegated_tool = "umg.widget.patch.v2" if prefer_v2 and self._has_tool("umg.widget.patch.v2") else "umg.widget.patch"
            self._require_tool(delegated_tool)
            return _ResolvedAction(requested_kind, delegated_tool, params, "")

        if requested_kind == "slot.patch":
            delegated_tool = "umg.slot.patch.v2" if prefer_v2 and self._has_tool("umg.slot.patch.v2") else "umg.slot.patch"
            self._require_tool(delegated_tool)
            return _ResolvedAction(requested_kind, delegated_tool, params, "")

        if requested_kind == "widget.event.bind":
            has_k2_event = self._pass_through.has_capability(self.K2_EVENT_CAPABILITY)
            if has_k2_event and self._has_tool("umg.widget.event.bind"):
                return _ResolvedAction(requested_kind, "umg.widget.event.bind", params, "")
            if self._has_tool("umg.binding.set"):
                translated_params = self._translate_event_bind_to_binding(params)
                return _ResolvedAction(
                    requested_kind,
                    "umg.binding.set",
                    translated_params,
                    "fallback: umg.widget.event.bind -> umg.binding.set",
                )
            self._require_tool("umg.widget.event.bind")
            return _ResolvedAction(
                requested_kind,
                "umg.widget.event.bind",
                params,
                "capability missing: using legacy umg.widget.event.bind",
            )

        if requested_kind == "widget.event.unbind":
            has_k2_event = self._pass_through.has_capability(self.K2_EVENT_CAPABILITY)
            if has_k2_event and self._has_tool("umg.widget.event.unbind"):
                return _ResolvedAction(requested_kind, "umg.widget.event.unbind", params, "")
            if self._has_tool("umg.binding.clear"):
                translated_params = self._translate_event_unbind_to_binding(params)
                return _ResolvedAction(
                    requested_kind,
                    "umg.binding.clear",
                    translated_params,
                    "fallback: umg.widget.event.unbind -> umg.binding.clear",
                )
            self._require_tool("umg.widget.event.unbind")
            return _ResolvedAction(
                requested_kind,
                "umg.widget.event.unbind",
                params,
                "capability missing: using legacy umg.widget.event.unbind",
            )

        if requested_kind == "widget.add":
            self._require_tool("umg.widget.add")
            return _ResolvedAction(requested_kind, "umg.widget.add", params, "")
        if requested_kind == "widget.remove":
            self._require_tool("umg.widget.remove")
            return _ResolvedAction(requested_kind, "umg.widget.remove", params, "")
        if requested_kind == "widget.reparent":
            self._require_tool("umg.widget.reparent")
            return _ResolvedAction(requested_kind, "umg.widget.reparent", params, "")
        if requested_kind == "binding.set":
            self._require_tool("umg.binding.set")
            return _ResolvedAction(requested_kind, "umg.binding.set", params, "")
        if requested_kind == "binding.clear":
            self._require_tool("umg.binding.clear")
            return _ResolvedAction(requested_kind, "umg.binding.clear", params, "")
        if requested_kind == "blueprint.patch":
            self._require_tool("umg.blueprint.patch")
            return _ResolvedAction(requested_kind, "umg.blueprint.patch", params, "")

        raise ValueError(f"Unsupported action kind: {requested_kind}")

    def _translate_event_bind_to_binding(self, params: dict[str, Any]) -> dict[str, Any]:
        event_name = params.get("event_name")
        function_name = params.get("function_name")
        if not isinstance(event_name, str) or not event_name:
            raise ValueError("widget.event.bind action requires non-empty event_name.")
        if not isinstance(function_name, str) or not function_name:
            raise ValueError("widget.event.bind action requires non-empty function_name.")

        translated = dict(params)
        translated["property_name"] = event_name
        translated["function_name"] = function_name
        translated.pop("event_name", None)
        return translated

    def _translate_event_unbind_to_binding(self, params: dict[str, Any]) -> dict[str, Any]:
        translated = dict(params)
        event_name = translated.pop("event_name", None)
        if isinstance(event_name, str) and event_name:
            translated["property_name"] = event_name
        return translated

    def _has_tool(self, tool_name: str) -> bool:
        return any(
            tool_def.name == tool_name and tool_def.enabled
            for tool_def in self._pass_through.list_tools()
        )

    def _require_tool(self, tool_name: str) -> None:
        if not self._has_tool(tool_name):
            raise UnknownToolError(f"Workflow requires tool not available: {tool_name}")
