#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


class ModeValidationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run and validate e2e_smoke_runner in cleanup/keep-assets modes."
    )
    parser.add_argument(
        "--config",
        type=str,
        default="configs/config.yaml",
        help="Config file path (relative to mcp_server root or absolute).",
    )
    parser.add_argument(
        "--mode",
        choices=("all", "cleanup", "keep"),
        default="all",
        help="Which mode to run. 'all' runs cleanup then keep.",
    )
    parser.add_argument(
        "--python-executable",
        type=str,
        default=sys.executable,
        help="Python executable used to invoke e2e_smoke_runner.",
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


def _parse_payload(stdout: str) -> dict[str, Any]:
    for line in reversed(stdout.splitlines()):
        text = line.strip()
        if not text or not text.startswith("{") or not text.endswith("}"):
            continue
        try:
            payload = json.loads(text)
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict) and isinstance(payload.get("scenarios"), list):
            return payload
    raise ModeValidationError("Could not find final JSON payload in e2e output.")


def _find_lifecycle_scenario(payload: dict[str, Any]) -> dict[str, Any]:
    scenarios = payload.get("scenarios")
    if not isinstance(scenarios, list):
        raise ModeValidationError("Payload does not contain scenarios list.")
    for scenario in scenarios:
        if isinstance(scenario, dict) and scenario.get("scenario") == "asset_lifecycle":
            return scenario
    raise ModeValidationError("asset_lifecycle scenario not found in payload.")


def _validate_mode(payload: dict[str, Any], mode: str) -> None:
    if payload.get("ok") is not True:
        raise ModeValidationError(f"payload.ok is not true in mode={mode}.")

    lifecycle = _find_lifecycle_scenario(payload)
    if lifecycle.get("status") != "ok":
        raise ModeValidationError(
            f"asset_lifecycle status is not ok in mode={mode}: {lifecycle.get('status')}"
        )

    summary = lifecycle.get("summary")
    if not isinstance(summary, dict):
        raise ModeValidationError(f"asset_lifecycle.summary missing in mode={mode}.")

    steps = lifecycle.get("steps")
    if not isinstance(steps, list):
        raise ModeValidationError(f"asset_lifecycle.steps missing in mode={mode}.")

    step_names = [
        step.get("step") for step in steps if isinstance(step, dict) and isinstance(step.get("step"), str)
    ]
    deleted_count = summary.get("deleted_count")
    kept_assets = summary.get("kept_assets")

    if mode == "cleanup":
        if deleted_count != 2:
            raise ModeValidationError(
                f"cleanup expected deleted_count=2, got {deleted_count!r}."
            )
        if kept_assets is True:
            raise ModeValidationError("cleanup expected kept_assets=false/absent.")
        if "delete.apply" not in step_names:
            raise ModeValidationError("cleanup expected delete.apply step.")
        return

    if mode == "keep":
        if deleted_count != 0:
            raise ModeValidationError(
                f"keep expected deleted_count=0, got {deleted_count!r}."
            )
        if kept_assets is not True:
            raise ModeValidationError("keep expected kept_assets=true.")
        if "delete.apply" in step_names:
            raise ModeValidationError("keep mode must not run delete.apply step.")
        if not any(name.startswith("verify.exists.") for name in step_names):
            raise ModeValidationError("keep mode expected verify.exists.* steps.")
        return

    raise ModeValidationError(f"Unknown mode: {mode}")


def _run_mode(
    *,
    project_root: Path,
    python_executable: str,
    config_path: Path,
    mode: str,
    extra_args: list[str],
) -> dict[str, Any]:
    command: list[str] = [
        python_executable,
        "examples/e2e_smoke_runner.py",
        "--config",
        str(config_path),
        "--stream-events",
        "--require-all",
    ]
    command.extend(extra_args)

    if mode == "keep":
        command.append("--asset-lifecycle-keep-assets")

    print(f"[RUN] mode={mode} command={' '.join(command)}", flush=True)
    completed = subprocess.run(
        command,
        cwd=project_root,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.stdout:
        print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="", file=sys.stderr)

    if completed.returncode != 0:
        raise ModeValidationError(
            f"e2e_smoke_runner failed in mode={mode} with exit={completed.returncode}."
        )

    payload = _parse_payload(completed.stdout)
    _validate_mode(payload, mode)

    lifecycle = _find_lifecycle_scenario(payload)
    summary = lifecycle.get("summary", {})
    print(
        f"[OK] mode={mode} deleted_count={summary.get('deleted_count')} kept_assets={summary.get('kept_assets')}",
        flush=True,
    )
    return payload


def main() -> int:
    args = parse_args()
    project_root = Path(__file__).resolve().parents[1]
    config_path = _resolve_config(project_root, args.config)

    if not config_path.exists():
        print(f"Config file not found: {config_path}", file=sys.stderr)
        return 2

    modes = ["cleanup", "keep"] if args.mode == "all" else [args.mode]
    try:
        for mode in modes:
            _run_mode(
                project_root=project_root,
                python_executable=args.python_executable,
                config_path=config_path,
                mode=mode,
                extra_args=list(args.extra_arg),
            )
    except ModeValidationError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 5

    print("[DONE] e2e mode matrix passed.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
