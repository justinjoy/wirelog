#!/usr/bin/env python3
"""Verify the authoritative perf-nightly DOOP wiring is still active."""

from pathlib import Path
import re
import unittest


WORKFLOW = Path(__file__).resolve().parents[2] / ".github/workflows/perf-nightly.yml"


def job(text: str, name: str) -> str:
    match = re.search(
        rf"^  {re.escape(name)}:\n(.*?)(?=^  \S|\Z)",
        text,
        re.MULTILINE | re.DOTALL,
    )
    assert match, f"missing job: {name}"
    return match[1]


def step(text: str, name: str) -> str:
    matches = re.findall(
        rf"^      - name: {re.escape(name)}\n(.*?)(?=^      - |^  \S|\Z)",
        text,
        re.MULTILINE | re.DOTALL,
    )
    assert len(matches) == 1, f"expected exactly one step: {name}"
    return matches[0]


def field(body: str, key: str) -> str:
    match = re.search(
        rf"^        {re.escape(key)}:(?: [|>])?\n((?:          [^\n]*\n)+)",
        body,
        re.MULTILINE,
    )
    if match:
        return match[1]
    inline = re.search(rf"^        {re.escape(key)}: ([^\n]+)$", body,
                       re.MULTILINE)
    assert inline, f"missing {key} block"
    return inline[1] + "\n"


def validate(text: str) -> None:
    stable = job(text, "perf-stable")
    install = field(step(stable, "Install dependencies"), "run")
    assert "curl" in install and "unzip" in install, \
        "stable runner must install DOOP download tools"

    host = field(step(stable, "Verify stable timing host"), "run")
    assert "nproc --all" in host and "cpu_count >= 8" in host, \
        "stable runner must verify W=8 CPU capacity"

    download = field(step(stable, "Download pinned DOOP dataset"), "run")
    assert download.strip() == "bench/data/doop/download.sh", \
        "stable path must download the pinned DOOP dataset"

    evaluator = step(stable, "Run evaluator timing gates")
    evaluator_run = field(evaluator, "run")
    assert "taskset -c 0 meson test -C build-perf-error" in evaluator_run, \
        "W=1 evaluator affinity must remain on CPU 0"
    assert "crdt_perf_gate cspa_w1_gate sub_ms_graph_perf_gate" in evaluator_run
    assert "doop_w8_gate" not in evaluator_run, \
        "W=1 evaluator invocation must not hide DOOP on CPU 0"

    doop = step(stable, "Run DOOP W=8 timing gate")
    doop_env = field(doop, "env")
    for variable in (
        "WIRELOG_PERF_GATE: '1'",
        "WIRELOG_PERF_REQUIRE: '1'",
        "WL_DOOP_PERF_GATE_TARGET_MS: ${{ vars.WL_DOOP_PERF_GATE_TARGET_MS }}",
    ):
        assert variable in doop_env, f"DOOP step must set {variable}"
    doop_run = field(doop, "run")
    assert "taskset -c 0-7 meson test -C build-perf-error doop_w8_gate" in doop_run, \
        "DOOP must run with W=8 CPU affinity"
    assert "--logbase doop-testlog" in doop_run
    assert "doop-testlog.txt >>" in doop_run
    assert 'exit "$rc"' in doop_run

    verify = field(step(stable, "Verify timing gates executed"), "run")
    assert "check-perf-gate-execution.sh" in verify


class PerfNightlyDoopWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = WORKFLOW.read_text(encoding="utf-8")

    def test_actual_workflow(self):
        validate(self.text)

    def test_doop_removal_fails(self):
        mutated = self.text.replace(
            "taskset -c 0-7 meson test -C build-perf-error doop_w8_gate",
            "taskset -c 0-7 meson test -C build-perf-error",
        )
        with self.assertRaisesRegex(AssertionError, "DOOP must"):
            validate(mutated)

    def test_single_cpu_affinity_fails(self):
        mutated = self.text.replace("taskset -c 0-7 meson test -C build-perf-error doop_w8_gate",
                                    "taskset -c 0 meson test -C build-perf-error doop_w8_gate")
        with self.assertRaisesRegex(AssertionError, "DOOP must"):
            validate(mutated)


if __name__ == "__main__":
    unittest.main()
