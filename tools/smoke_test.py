#!/usr/bin/env python3
"""Run bounded native-runtime checks and preserve each launch's actual evidence.

These clear-only presentation timings are diagnostic observations, not gameplay
benchmarks. This harness never creates settings files or substitutes software
rendering after a failed hardware case.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
from typing import Any


FRAMES = 60
TIMEOUT_SECONDS = 30


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def text_output(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def read_lines(file: Path) -> list[str]:
    if not file.exists():
        return []
    return file.read_text(encoding="utf-8").splitlines()


def metrics_from(message: str) -> dict[str, int | float | str]:
    parts = message.split(",")
    if parts[0] != "clear-only":
        raise ValueError("frame summary is not identified as clear-only")
    raw: dict[str, str] = {}
    for part in parts[1:]:
        key, separator, value = part.partition("=")
        if not separator or key in raw:
            raise ValueError("malformed or duplicate frame metric")
        raw[key] = value
    metrics: dict[str, int | float | str] = {"workload": "clear-only"}
    for key in ("frames", "samples", "scheduler_ticks"):
        value = int(raw[key])
        if value < 0:
            raise ValueError(f"negative {key}")
        metrics[key] = value
    for key in ("mean_ms", "p95_ms", "p99_ms"):
        value = float(raw[key])
        if not math.isfinite(value) or value < 0:
            raise ValueError(f"non-finite or negative {key}")
        metrics[key] = value
    if metrics["frames"] < FRAMES or metrics["samples"] < FRAMES:
        raise ValueError(f"fewer than {FRAMES} frames/samples were recorded")
    if metrics["p95_ms"] > metrics["p99_ms"]:
        raise ValueError("p95 exceeds p99")
    return metrics


def read_settings(file: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or key in values:
            raise ValueError("malformed persisted settings")
        values[key.strip()] = value.strip()
    return values


def has_subsequence(sequence: list[str], expected: list[str]) -> bool:
    index = 0
    for value in sequence:
        if index < len(expected) and value == expected[index]:
            index += 1
    return index == len(expected)


def run_case(executable: Path, root: Path, case: dict[str, Any]) -> dict[str, Any]:
    data_dir = root / case.get("data_name", case["name"])
    repeat = bool(case.get("repeat", False))
    if not repeat:
        data_dir.mkdir()
    elif not data_dir.is_dir():
        raise ValueError("repeat case has no prior data directory")
    log_path = data_dir / "logs" / "runtime.jsonl"
    previous_lines = read_lines(log_path)
    settings_path = data_dir / "config" / "settings.ini"
    settings_before = settings_path.read_bytes() if settings_path.is_file() else None
    command = [str(executable), "--no-dialogs", "--smoke-frames", str(FRAMES),
               "--data-dir", str(data_dir)] + case["arguments"]
    result: dict[str, Any] = {
        "name": case["name"],
        "command": command,
        "data_dir": str(data_dir),
        "started_at": utc_now(),
        "expected_exit_code": case.get("expected_exit", 0),
        "requested_resolution": case.get("resolution"),
        "prior_log_lines": len(previous_lines),
        "timeout_seconds": TIMEOUT_SECONDS,
        "errors": [],
    }
    errors = result["errors"]
    started = time.monotonic()
    try:
        completed = subprocess.run(
            command, cwd=executable.parent, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            encoding="utf-8", errors="replace", timeout=TIMEOUT_SECONDS,
            check=False,
        )
        result["exit_code"] = completed.returncode
        result["stdout"] = completed.stdout
        result["stderr"] = completed.stderr
        if completed.returncode != result["expected_exit_code"]:
            errors.append(f"exit code {completed.returncode}; expected {result['expected_exit_code']}")
    except subprocess.TimeoutExpired as error:
        result["exit_code"] = None
        result["stdout"] = text_output(error.stdout)
        result["stderr"] = text_output(error.stderr)
        errors.append(f"process exceeded {TIMEOUT_SECONDS}s and was terminated")
    except OSError as error:
        result["exit_code"] = None
        result["launch_error"] = str(error)
        errors.append(f"launch failed: {error}")
    result["wall_seconds"] = round(time.monotonic() - started, 6)
    result["finished_at"] = utc_now()

    events: list[dict[str, Any]] = []
    try:
        after_lines = read_lines(log_path)
        result["total_log_lines"] = len(after_lines)
        if after_lines[:len(previous_lines)] != previous_lines:
            errors.append("prior log evidence was changed or truncated")
        for offset, line in enumerate(after_lines[len(previous_lines):], start=len(previous_lines) + 1):
            event = json.loads(line)
            if not isinstance(event, dict) or any(
                not isinstance(event.get(key), str) for key in ("time", "level", "event", "message")
            ):
                raise ValueError(f"invalid JSONL event schema at line {offset}")
            events.append(event)
    except (OSError, UnicodeError, ValueError) as error:
        errors.append(f"log validation failed: {error}")
    result["session_events"] = events
    names = [event["event"] for event in events]
    messages: dict[str, list[str]] = {}
    for event in events:
        messages.setdefault(event["event"], []).append(event["message"])
    result["adapter"] = messages.get("graphics_ready", [])
    result["audio"] = [event for event in events if event["event"] in
                       ("audio_ready", "audio_unavailable", "audio_disabled")]
    result["resize_events"] = messages.get("resize", [])
    result["reported_initial_dimensions"] = messages.get("graphics_dimensions", [])

    if result["expected_exit_code"] == 0:
        for required in ("startup", "graphics_ready", "frame_summary", "shutdown"):
            if names.count(required) != 1:
                errors.append(f"expected one {required} event in this launch")
        if "fatal" in names:
            errors.append("successful case emitted a fatal event")
        if any(event["level"] == "error" for event in events):
            errors.append("successful case emitted an error-level event")
        if not has_subsequence(names, ["startup", "graphics_ready", "frame_summary", "shutdown"]):
            errors.append("successful launch events are missing or out of order")
        for message in messages.get("frame_summary", []):
            try:
                result["frame_metrics"] = metrics_from(message)
            except (KeyError, ValueError) as error:
                errors.append(f"invalid frame summary: {error}")
        if case.get("audio_enabled"):
            if names.count("audio_ready") + names.count("audio_unavailable") != 1:
                errors.append("audio initialization produced no unique status")
        elif names.count("audio_disabled") != 1:
            errors.append("no unique audio_disabled event for --no-audio")
        if case.get("warp"):
            if not any("WARP software diagnostics" in value for value in result["adapter"]):
                errors.append("WARP case did not identify its explicit software adapter")
        elif any("WARP software diagnostics" in value for value in result["adapter"]):
            errors.append("hardware case unexpectedly used WARP")
        if case.get("resize") and not has_subsequence(
            result["resize_events"], ["800x600", "1920x1080", "1280x720"]
        ):
            errors.append("resize case did not record 800x600, 1920x1080, 1280x720 in order")
        if case.get("resolution"):
            width, height = case["resolution"]
            if result["reported_initial_dimensions"] != [f"{width}x{height}"]:
                errors.append("initial graphics dimensions differ from requested dimensions or are not uniquely logged")
        if case.get("borderless"):
            if "borderless" not in messages.get("display_mode", []):
                errors.append("borderless case did not log the initial display transition")
            if not result["resize_events"]:
                errors.append("borderless case did not log the resulting swap-chain resize")
        try:
            settings = read_settings(settings_path)
            result["persisted_settings"] = settings
            if settings.get("vsync") != "1":
                errors.append("default vsync was not persisted as enabled")
            if not case.get("borderless") and case.get("resolution"):
                width, height = case["resolution"]
                if settings.get("width") != str(width) or settings.get("height") != str(height):
                    errors.append("persisted dimensions differ from requested dimensions")
        except (OSError, UnicodeError, ValueError) as error:
            errors.append(f"settings persistence check failed: {error}")
        if repeat:
            if not previous_lines:
                errors.append("repeat case had no prior session log")
            if names.count("settings_loaded") != 1 or "settings_default" in names:
                errors.append("repeat launch did not load existing validated settings")
            if settings_before is None:
                errors.append("repeat case had no existing settings")
            elif settings_path.is_file() and settings_path.read_bytes() != settings_before:
                errors.append("repeat launch unexpectedly changed persisted settings")
        elif names.count("settings_default") != 1:
            errors.append("fresh launch did not use missing-file defaults")
    elif case.get("expect_fatal"):
        if names.count("fatal") != 1:
            errors.append("injected failure did not emit exactly one fatal event")
        if "graphics_ready" in names or "shutdown" in names:
            errors.append("injected initialization failure reached graphics or clean shutdown")
        if not any("Requested initialization failure" in value for value in messages.get("fatal", [])):
            errors.append("fatal event does not identify the injected failure")
    elif case.get("invalid_arguments"):
        # Argument validation occurs before the runtime logger is constructed.
        if events:
            errors.append("invalid arguments unexpectedly started a runtime session")
        result["expected_logging_behavior"] = "No runtime log: argument parsing fails before logger construction."
    result["passed"] = not errors
    return result


def cases() -> list[dict[str, Any]]:
    return [
        {"name": "hardware_720p_audio", "resolution": [1280, 720], "audio_enabled": True,
         "arguments": ["--width", "1280", "--height", "720"]},
        {"name": "hardware_720p_repeat", "data_name": "hardware_720p_audio", "repeat": True,
         "resolution": [1280, 720], "audio_enabled": True, "arguments": []},
        {"name": "hardware_1080p", "resolution": [1920, 1080],
         "arguments": ["--width", "1920", "--height", "1080", "--no-audio"]},
        {"name": "hardware_1440p", "resolution": [2560, 1440],
         "arguments": ["--width", "2560", "--height", "1440", "--no-audio"]},
        {"name": "hardware_4k", "resolution": [3840, 2160],
         "arguments": ["--width", "3840", "--height", "2160", "--no-audio"]},
        {"name": "hardware_resize", "resolution": [1280, 720], "resize": True,
         "arguments": ["--width", "1280", "--height", "720", "--no-audio", "--resize-smoke"]},
        {"name": "hardware_borderless", "resolution": [1280, 720], "borderless": True,
         "arguments": ["--width", "1280", "--height", "720", "--no-audio", "--borderless"]},
        {"name": "warp_720p", "resolution": [1280, 720], "warp": True,
         "arguments": ["--width", "1280", "--height", "720", "--no-audio", "--warp"]},
        {"name": "injected_initialization_failure", "expected_exit": 1, "expect_fatal": True,
         "arguments": ["--fail-init", "--no-audio"]},
        {"name": "invalid_width", "expected_exit": 1, "invalid_arguments": True,
         "arguments": ["--width", "0", "--no-audio"]},
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path, help="Native JStarsReborn.exe to test")
    parser.add_argument("--output-dir", required=True, type=Path, help="New evidence directory; must not exist")
    args = parser.parse_args()
    executable = args.exe.resolve()
    root = args.output_dir.resolve()
    if not executable.is_file():
        parser.error(f"executable is not a file: {executable}")
    try:
        root.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"cannot create fresh output directory: {error}")
    report: dict[str, Any] = {
        "schema_version": 1,
        "started_at": utc_now(),
        "executable": str(executable),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "logical_cpu_count": os.cpu_count(),
        "python": platform.python_version(),
        "scope": "Native foundation clear-only presentation and lifecycle smoke tests; no gameplay benchmark.",
        "limitations": [
            "No original assets, combat, animation, multiplayer, or gameplay performance is exercised.",
            "No physical controller, audible output, HDR, GPU-hang recovery, or visual correctness is verified.",
            "audio_unavailable is a recorded graceful-degradation outcome, not verified audio initialization.",
            "Each timing sample measures the clear/present call section with default vsync, not total game frame time.",
        ],
        "cases": [],
    }
    for case in cases():
        try:
            result = run_case(executable, root, case)
        except (OSError, UnicodeError, ValueError) as error:
            result = {"name": case["name"], "passed": False,
                      "errors": [f"harness could not collect case evidence: {error}"]}
        report["cases"].append(result)
        print(f"{'PASS' if result['passed'] else 'FAIL'} {case['name']}", flush=True)
        for error in result["errors"]:
            print(f"  {error}", flush=True)
    report["finished_at"] = utc_now()
    report["passed"] = all(result["passed"] for result in report["cases"])
    report["passed_cases"] = sum(result["passed"] for result in report["cases"])
    report["total_cases"] = len(report["cases"])
    (root / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{report['passed_cases']}/{report['total_cases']} passed; report: {root / 'report.json'}", flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
