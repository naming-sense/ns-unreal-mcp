#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


class ControlRigE2EError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run one-click ControlRig E2E validation with strict(require-all) mode."
    )
    parser.add_argument(
        "--config",
        type=str,
        default="configs/config.yaml",
        help="Config file path (relative to mcp_server root or absolute).",
    )
    parser.add_argument(
        "--python-executable",
        type=str,
        default=sys.executable,
        help="Python executable used to run helper scripts.",
    )
    parser.add_argument(
        "--timeout-ms",
        type=int,
        default=120000,
        help="Per-tool timeout forwarded to e2e_smoke_runner.",
    )
    parser.add_argument(
        "--root-path",
        type=str,
        default="/Game/MCPRuntimeE2E",
        help="Temporary asset root path.",
    )
    parser.add_argument(
        "--sequencer-probe-name",
        type=str,
        default="LS_Probe_01",
        help="Sequencer asset name used when sequencer-path is not provided.",
    )
    parser.add_argument(
        "--sequencer-path",
        type=str,
        default="",
        help="Existing level sequence object path. If omitted, probe asset is auto-created/resolved.",
    )
    parser.add_argument(
        "--material-path",
        type=str,
        default="/Game/Characters/Mannequins/Materials/Manny/MI_Manny_01_New.MI_Manny_01_New",
    )
    parser.add_argument(
        "--niagara-path",
        type=str,
        default="/Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad.NS_JumpPad",
    )
    parser.add_argument(
        "--umg-path",
        type=str,
        default="/Game/Input/Touch/UI_Thumbstick.UI_Thumbstick",
    )
    parser.add_argument(
        "--controlrig-path",
        type=str,
        default="/Game/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body",
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Extra argument forwarded to e2e_smoke_runner (repeatable).",
    )
    return parser.parse_args()


def _resolve_config(project_root: Path, config_arg: str) -> Path:
    config_path = Path(config_arg)
    if not config_path.is_absolute():
        config_path = (project_root / config_path).resolve()
    return config_path


def _parse_json_payload(stdout: str) -> dict[str, Any]:
    for line in reversed(stdout.splitlines()):
        text = line.strip()
        if not text or not text.startswith("{") or not text.endswith("}"):
            continue
        try:
            payload = json.loads(text)
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict):
            return payload
    raise ControlRigE2EError("Could not parse JSON payload from command output.")


def _run_command(command: list[str], *, cwd: Path) -> dict[str, Any]:
    print(f"[RUN] {' '.join(command)}", flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.stdout:
        print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="", file=sys.stderr)

    if completed.returncode != 0:
        raise ControlRigE2EError(
            f"Command failed with exit={completed.returncode}: {' '.join(command)}"
        )
    return _parse_json_payload(completed.stdout)


def _ensure_sequencer_path(
    *,
    project_root: Path,
    python_executable: str,
    config_path: Path,
    timeout_ms: int,
    root_path: str,
    sequencer_probe_name: str,
    existing_path: str,
) -> str:
    if existing_path:
        return existing_path

    probe_path = f"{root_path.rstrip('/')}/{sequencer_probe_name}.{sequencer_probe_name}"
    load_payload = _run_command(
        [
            python_executable,
            "examples/agent_tool_client.py",
            "--config",
            str(config_path),
            "--tool",
            "asset.load",
            "--params-json",
            json.dumps({"object_path": probe_path}, ensure_ascii=False),
            "--timeout-ms",
            str(timeout_ms),
        ],
        cwd=project_root,
    )

    if load_payload.get("ok") is True:
        result = load_payload.get("result")
        if isinstance(result, dict) and result.get("loaded") is True:
            return probe_path

    create_payload = _run_command(
        [
            python_executable,
            "examples/agent_tool_client.py",
            "--config",
            str(config_path),
            "--tool",
            "seq.asset.create",
            "--params-json",
            json.dumps(
                {
                    "package_path": root_path,
                    "asset_name": sequencer_probe_name,
                    "overwrite": True,
                    "display_rate": {"numerator": 30, "denominator": 1},
                    "tick_resolution": {"numerator": 24000, "denominator": 1},
                },
                ensure_ascii=False,
            ),
            "--timeout-ms",
            str(timeout_ms),
        ],
        cwd=project_root,
    )
    if create_payload.get("ok") is not True:
        raise ControlRigE2EError("Failed to create sequencer probe asset.")

    create_result = create_payload.get("result")
    if isinstance(create_result, dict):
        object_path = create_result.get("object_path")
        if isinstance(object_path, str) and object_path:
            return object_path
    return probe_path


def _find_scenario(payload: dict[str, Any], scenario_name: str) -> dict[str, Any]:
    scenarios = payload.get("scenarios")
    if not isinstance(scenarios, list):
        raise ControlRigE2EError("Payload does not contain scenarios list.")
    for scenario in scenarios:
        if isinstance(scenario, dict) and scenario.get("scenario") == scenario_name:
            return scenario
    raise ControlRigE2EError(f"Scenario not found: {scenario_name}")


def _validate_e2e_payload(payload: dict[str, Any]) -> None:
    if payload.get("ok") is not True:
        raise ControlRigE2EError("e2e payload ok=false")

    for scenario_name in ("controlrig", "controlrig_extended", "sequencer_extended", "umg_extended"):
        scenario = _find_scenario(payload, scenario_name)
        if scenario.get("status") != "ok":
            raise ControlRigE2EError(
                f"{scenario_name} status is not ok: {scenario.get('status')}"
            )

    controlrig_ext = _find_scenario(payload, "controlrig_extended")
    summary = controlrig_ext.get("summary")
    if not isinstance(summary, dict):
        raise ControlRigE2EError("controlrig_extended.summary missing")
    if int(summary.get("step_count", 0)) <= 0:
        raise ControlRigE2EError("controlrig_extended.step_count is invalid")


def main() -> int:
    args = parse_args()
    project_root = Path(__file__).resolve().parents[1]
    config_path = _resolve_config(project_root, args.config)
    if not config_path.exists():
        print(f"Config file not found: {config_path}", file=sys.stderr)
        return 2

    try:
        sequencer_path = _ensure_sequencer_path(
            project_root=project_root,
            python_executable=args.python_executable,
            config_path=config_path,
            timeout_ms=args.timeout_ms,
            root_path=args.root_path,
            sequencer_probe_name=args.sequencer_probe_name,
            existing_path=args.sequencer_path.strip(),
        )

        command: list[str] = [
            args.python_executable,
            "examples/e2e_smoke_runner.py",
            "--config",
            str(config_path),
            "--no-auto-discover",
            "--material-path",
            args.material_path,
            "--niagara-path",
            args.niagara_path,
            "--umg-path",
            args.umg_path,
            "--sequencer-path",
            sequencer_path,
            "--controlrig-path",
            args.controlrig_path,
            "--skip-asset-lifecycle",
            "--timeout-ms",
            str(args.timeout_ms),
            "--require-all",
        ]
        command.extend(args.extra_arg)

        payload = _run_command(command, cwd=project_root)
        _validate_e2e_payload(payload)

        controlrig_ext = _find_scenario(payload, "controlrig_extended")
        controlrig_summary = controlrig_ext.get("summary", {})
        print(
            "[OK] ControlRig E2E passed "
            f"(step_count={controlrig_summary.get('step_count')}, "
            f"temp_sequence={controlrig_summary.get('temp_sequence_path')})",
            flush=True,
        )
        return 0
    except ControlRigE2EError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 5


if __name__ == "__main__":
    raise SystemExit(main())
