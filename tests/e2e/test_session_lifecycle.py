#!/usr/bin/env python3
"""Exercise the persisted mock-provider lifecycle and legacy compatibility contract."""

from __future__ import annotations

import base64
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

import harness


SESSION_ID_RE = re.compile(r"session ([0-9a-f-]{36})")
LEGACY_ID = "00000000-0000-4000-8000-000000000004"
LEGACY_FILENAME = f"2026-01-01T00-00-00Z_{LEGACY_ID}.jsonl"
IMAGE_B64 = (
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
)


def run_oneshot(home: Path, workdir: Path, prompt: str, script: str,
                options: list[str] | None = None) -> subprocess.CompletedProcess[str]:
    env = harness.hermetic_env(home)
    env.update(
        {
            "HAX_MOCK_SCRIPT": str(harness.REPO_ROOT / "scripts" / "mock" / script),
            "HAX_NO_SESSION": "0",
            "HAX_PROVIDER": "mock",
            "XDG_STATE_HOME": str(home / "state"),
        }
    )
    binary = Path(
        os.environ.get("HAX_BIN", str(harness.REPO_ROOT / "build" / "hax"))
    ).resolve()
    command = [str(binary), *(options or []), "-p", prompt]
    return subprocess.run(
        command,
        cwd=workdir,
        env=env,
        capture_output=True,
        encoding="utf-8",
        timeout=30,
    )


def read_records(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text().splitlines()]


def session_id(result: subprocess.CompletedProcess[str]) -> str:
    match = SESSION_ID_RE.search(result.stderr)
    harness.expect(match is not None, "run reports a resumable session id", None)
    return match.group(1) if match else ""


def assert_usage(record: dict, expected: dict[str, int | float]) -> None:
    harness.expect(record["kind"] == "turn_usage", "usage record has the expected kind")
    harness.expect(record["provider"] == "mock", "usage keeps the wire provider")
    harness.expect(record["model"] == "mock-model", "usage keeps the wire model")
    usage = record["usage"]
    for key, value in expected.items():
        harness.expect(usage.get(key) == value, f"usage {key} is {value!r}")
    harness.expect(isinstance(usage.get("elapsed_ms"), int), "usage records elapsed time")
    harness.expect(usage["elapsed_ms"] >= 0, "usage elapsed time is non-negative")


def assert_no_glaze_diagnostics(result: subprocess.CompletedProcess[str]) -> None:
    diagnostics = result.stderr.lower()
    harness.expect("glz::" not in diagnostics, "failure does not leak Glaze diagnostics")
    harness.expect("glaze" not in diagnostics, "failure does not name Glaze")


def main() -> None:
    home = harness.scratch_dir()
    workdir = home / "work"
    workdir.mkdir()
    (workdir / "image.png").write_bytes(base64.b64decode(IMAGE_B64))

    first = run_oneshot(home, workdir, "start lifecycle", "session_lifecycle.txt")
    assert_no_glaze_diagnostics(first)
    harness.expect(first.returncode == 0, "initial lifecycle run exits successfully", first)
    harness.expect("Lifecycle complete." in first.stdout, "initial response reaches stdout", first)

    state_sessions = home / "state" / "hax" / "sessions"
    session_files = list(state_sessions.rglob("*.jsonl"))
    harness.expect(len(session_files) == 1, "initial run closes exactly one session file", first)
    if len(session_files) != 1:
        return
    path = session_files[0]
    first_id = session_id(first)
    harness.expect(path.stem.endswith("_" + first_id), "reported id names the session file")
    before_resume = read_records(path)

    harness.expect(len(before_resume) == 18, "initial run writes the complete record set")
    if len(before_resume) < 18:
        return
    header = before_resume[0]
    harness.expect(header["type"] == "session", "header is a session record")
    harness.expect(header["version"] == 1, "header has the current format version")
    harness.expect(header["id"] == first_id, "header id matches the filename")
    harness.expect(header["cwd"] == str(workdir), "header records the conversation directory")
    harness.expect(header["provider"] == "mock", "header records the provider")
    harness.expect(header["model"] == "mock-model", "header records the model")
    for key in ("model_label", "effort", "preset", "forked_from"):
        harness.expect(key not in header, f"header omits absent optional field {key}")

    items = before_resume[1:]
    expected_kinds = [
        "turn_boundary",
        "user",
        "reasoning",
        "assistant",
        "tool_call",
        "tool_result",
        "turn_usage",
        "turn_boundary",
        "reasoning",
        "assistant",
        "tool_call",
        "tool_result",
        "turn_usage",
        "turn_boundary",
        "reasoning",
        "assistant",
        "turn_usage",
    ]
    harness.expect([item["kind"] for item in items] == expected_kinds,
                   "boundaries preserve the three provider turns")
    harness.expect(items[1]["text"] == "start lifecycle", "user message is persisted")
    harness.expect(items[2]["reasoning_text"] == "planning write",
                   "reasoning text is persisted")
    harness.expect(items[2]["provider"] == "mock", "reasoning provenance keeps the provider")
    harness.expect(items[2]["model"] == "mock-model", "reasoning provenance keeps the model")
    harness.expect(items[3]["text"] == "Writing the lifecycle marker",
                   "assistant message is persisted")

    write_call = items[4]
    write_args = json.loads(write_call["arguments"])
    harness.expect(write_call["tool_name"] == "write", "write tool call is persisted")
    harness.expect(write_args == {"path": "notes.txt", "content": "saved before resume\n"},
                   "write arguments retain their authored JSON")
    write_result = items[5]
    harness.expect(write_result["call_id"] == write_call["call_id"],
                   "write result is paired with its call")
    harness.expect(write_result["output"].startswith("created notes.txt"),
                   "write result is observable")
    harness.expect("origin" not in write_result, "ordinary write result has no synthetic origin")
    harness.expect("output_hidden_tail" not in write_result,
                   "ordinary write result has no hidden tail")
    assert_usage(items[6], {"input": 120, "output": 34, "cached": 20,
                             "cache_write": 10, "cache_write_1h": 2, "cost": 0.125,
                             "in_tokens": 90, "cost_total": 0.125})

    read_call = items[10]
    read_result = items[11]
    harness.expect(read_call["tool_name"] == "read", "read tool call is persisted")
    harness.expect(json.loads(read_call["arguments"]) == {"path": "image.png"},
                   "read arguments retain their authored JSON")
    harness.expect(read_result["call_id"] == read_call["call_id"],
                   "read result is paired with its call")
    harness.expect(read_result["output"].startswith("Read image image.png"),
                   "image read result is observable")
    harness.expect(read_result["images"] == [{"mime": "image/png", "data": IMAGE_B64,
                                               "width": 1, "height": 1}],
                   "image payload and dimensions survive persistence")
    assert_usage(items[12], {"input": 80, "output": 25, "in_tokens": 80})
    harness.expect("cached" not in items[12]["usage"], "absent cached usage stays absent")
    harness.expect("cost" not in items[12]["usage"], "absent cost stays absent")
    harness.expect(items[14]["reasoning_text"] == "finalizing lifecycle",
                   "final reasoning is persisted")
    harness.expect(items[15]["text"] == "Lifecycle complete.",
                   "final assistant message is persisted")
    assert_usage(items[16], {"input": 60, "output": 12, "cost": 0.25,
                              "in_tokens": 60, "cost_total": 0.25})
    harness.expect(not any("origin" in item for item in items),
                   "ordinary lifecycle records do not invent origins")

    second = run_oneshot(home, workdir, "continue", "session_resume.txt",
                         [f"--resume={first_id}"])
    assert_no_glaze_diagnostics(second)
    harness.expect(second.returncode == 0, "resumed lifecycle run exits successfully", second)
    harness.expect("Resumed conversation complete." in second.stdout,
                   "resumed response reaches stdout", second)
    harness.expect((workdir / "notes.txt").read_text() == "saved before resume\n",
                   "resumed run sees the file written before close", second)

    after_resume = read_records(path)
    harness.expect(after_resume[: len(before_resume)] == before_resume,
                   "resume/load leaves the closed prefix byte-for-byte semantic equivalent")
    appended = after_resume[len(before_resume):]
    expected_appended_kinds = [
        "turn_boundary",
        "user",
        "reasoning",
        "assistant",
        "tool_call",
        "tool_result",
        "turn_usage",
        "turn_boundary",
        "reasoning",
        "assistant",
        "turn_usage",
    ]
    harness.expect([item["kind"] for item in appended] == expected_appended_kinds,
                   "continuation appends a fresh user turn and follow-up boundary")
    if len(appended) == len(expected_appended_kinds):
        harness.expect(appended[1]["text"] == "continue", "continuation prompt is persisted")
        harness.expect("saved before resume" in appended[5]["output"],
                       "resumed read observes the saved file")
        assert_usage(appended[6], {"input": 45, "output": 11, "cached": 5,
                                   "cost": 0.05, "in_tokens": 40, "cost_total": 0.05})
        assert_usage(appended[10], {"input": 30, "output": 8, "in_tokens": 30})

    legacy_path = path.parent / LEGACY_FILENAME
    legacy_fixture = (
        harness.REPO_ROOT / "tests" / "fixtures" / "session" / "legacy_compatibility.jsonl"
    )
    shutil.copyfile(legacy_fixture, legacy_path)
    legacy = run_oneshot(home, workdir, "legacy continuation", "session_legacy_resume.txt",
                         [f"--resume={LEGACY_ID}", "--provider=mock", "--model=mock-model"])
    assert_no_glaze_diagnostics(legacy)
    harness.expect(legacy.returncode == 0, "legacy fixture resumes successfully", legacy)
    harness.expect("Legacy compatibility loaded." in legacy.stdout,
                   "legacy continuation is observable", legacy)


if __name__ == "__main__":
    main()
