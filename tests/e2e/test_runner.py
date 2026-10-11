"""
Central E2E Test Runner & CLI Entrypoint
Supports Dual-Mode Execution: Contract/Synthetic (Offline) & Live Device (ADB)
4-Tier test coverage (Tiers 1-4) with structured JSON and JUnit XML reporting.
"""

import os
import sys
import argparse
import unittest
from typing import List, Optional

# Ensure repository root is on sys.path
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

from tests.e2e.common import (
    TestResult,
    TestStatus,
    DeviceBridge,
    generate_json_report,
    generate_junit_xml,
    print_terminal_report,
)
from tests.e2e.tier1_feature_tests import run_tier1_tests
from tests.e2e.tier2_boundary_tests import run_tier2_tests
from tests.e2e.tier3_combinatorial_tests import run_tier3_tests
from tests.e2e.tier4_workload_tests import run_tier4_tests


def run_e2e_suite(
    workspace_dir: str,
    mode: str = "auto",
    tier: str = "all",
    report_json: Optional[str] = None,
    report_xml: Optional[str] = None,
    verbose: bool = False,
) -> int:
    """
    Executes selected test tiers and generates reports.
    Returns exit code:
      0: All executed tests passed or skipped.
      1: One or more test assertions failed.
      2: Environment or setup error.
    """
    workspace_dir = os.path.abspath(workspace_dir)

    # Resolve execution mode and device connection
    bridge = DeviceBridge()
    is_connected = bridge.is_connected()

    if mode == "device":
        if not is_connected:
            print(f"[ERROR] Live device mode requested, but Samsung Galaxy A20 ({bridge.serial}) is not connected.", file=sys.stderr)
            return 2
        offline_mode = False
        exec_mode_str = "device"
        active_device = bridge
    elif mode == "offline":
        offline_mode = True
        exec_mode_str = "offline"
        active_device = None
    else: # auto
        if is_connected:
            offline_mode = False
            exec_mode_str = "device"
            active_device = bridge
        else:
            offline_mode = True
            exec_mode_str = "offline"
            active_device = None

    if verbose:
        print(f"[INFO] Malicomp E2E Test Engine Initialized")
        print(f"[INFO] Workspace: {workspace_dir}")
        print(f"[INFO] Execution Mode: {exec_mode_str} (Device Serial: {bridge.serial if active_device else 'N/A'})")
        print(f"[INFO] Target Tier(s): {tier}")

    all_results: List[TestResult] = []

    # Execute Tiers
    selected_tiers = [1, 2, 3, 4] if tier in ("all", "0") else [int(t.strip()) for t in tier.split(",")]

    if 1 in selected_tiers:
        if verbose:
            print("[INFO] Executing Tier 1: Feature Coverage (Category-Partition F1-F19)...")
        t1_results = run_tier1_tests(workspace_dir, active_device)
        all_results.extend(t1_results)

    if 2 in selected_tiers:
        if verbose:
            print("[INFO] Executing Tier 2: Boundary Value Analysis & Corner Cases...")
        t2_results = run_tier2_tests(workspace_dir, active_device)
        all_results.extend(t2_results)

    if 3 in selected_tiers:
        if verbose:
            print("[INFO] Executing Tier 3: Pairwise Combinatorial Tests...")
        t3_results = run_tier3_tests(workspace_dir, active_device)
        all_results.extend(t3_results)

    if 4 in selected_tiers:
        if verbose:
            print("[INFO] Executing Tier 4: Real-World Workload Tests...")
        t4_results = run_tier4_tests(workspace_dir, active_device, offline_mode=offline_mode)
        all_results.extend(t4_results)

    # Print summary
    print_terminal_report(all_results, verbose=verbose)

    # Generate Reports
    device_id = bridge.serial if active_device else None
    if report_json:
        generate_json_report(all_results, report_json, execution_mode=exec_mode_str, device_id=device_id)
        if verbose:
            print(f"[INFO] Structured JSON report written to: {report_json}")

    if report_xml:
        generate_junit_xml(all_results, report_xml)
        if verbose:
            print(f"[INFO] JUnit XML report written to: {report_xml}")

    failed_count = sum(1 for r in all_results if r.status == TestStatus.FAIL)
    return 1 if failed_count > 0 else 0


# =============================================================================
# Standard unittest discoverability adapter
# =============================================================================
class E2ETestSuite(unittest.TestCase):
    """unittest adapter allowing execution via python3 -m unittest discover."""

    @classmethod
    def setUpClass(cls):
        cls.workspace_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

    def test_tier1_features(self):
        results = run_tier1_tests(self.workspace_dir, None)
        failed = [f"{r.id}: {r.error_message}" for r in results if r.status == TestStatus.FAIL]
        self.assertEqual(len(failed), 0, f"Tier 1 failures:\n" + "\n".join(failed))

    def test_tier2_boundaries(self):
        results = run_tier2_tests(self.workspace_dir, None)
        failed = [f"{r.id}: {r.error_message}" for r in results if r.status == TestStatus.FAIL]
        self.assertEqual(len(failed), 0, f"Tier 2 failures:\n" + "\n".join(failed))

    def test_tier3_combinations(self):
        results = run_tier3_tests(self.workspace_dir, None)
        failed = [f"{r.id}: {r.error_message}" for r in results if r.status == TestStatus.FAIL]
        self.assertEqual(len(failed), 0, f"Tier 3 failures:\n" + "\n".join(failed))

    def test_tier4_workloads_offline(self):
        results = run_tier4_tests(self.workspace_dir, None, offline_mode=True)
        failed = [f"{r.id}: {r.error_message}" for r in results if r.status == TestStatus.FAIL]
        self.assertEqual(len(failed), 0, f"Tier 4 failures:\n" + "\n".join(failed))


def main():
    parser = argparse.ArgumentParser(
        description="MaliCompat End-to-End (E2E) Test Suite Runner",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument(
        "--mode",
        choices=["offline", "device", "auto"],
        default="auto",
        help="Execution mode: 'offline' (contract/host), 'device' (Galaxy A20 ADB), 'auto' (detect)"
    )
    parser.add_argument(
        "--tier",
        default="all",
        help="Tier(s) to execute: '1', '2', '3', '4', or 'all' (or comma-separated like '1,2')"
    )
    parser.add_argument(
        "--report-json",
        default=None,
        help="File path to write structured JSON report (e.g. reports/e2e_report.json)"
    )
    parser.add_argument(
        "--report-xml",
        default=None,
        help="File path to write JUnit XML report (e.g. reports/e2e_junit.xml)"
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable detailed per-test log output"
    )
    parser.add_argument(
        "--workspace",
        default=".",
        help="Root workspace path"
    )

    args = parser.parse_args()
    exit_code = run_e2e_suite(
        workspace_dir=args.workspace,
        mode=args.mode,
        tier=args.tier,
        report_json=args.report_json,
        report_xml=args.report_xml,
        verbose=args.verbose,
    )
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
