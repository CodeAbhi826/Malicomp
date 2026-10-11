#!/usr/bin/env bash
# ==============================================================================
# Unified E2E Test Suite CLI Runner
# Need for Speed: Most Wanted (2005) Android Recompilation on Samsung Galaxy A20
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

MODE="auto"
TIER="all"
REPORT_JSON=""
REPORT_XML=""
VERBOSE=false

show_help() {
    cat << EOF
MaliCompat 4-Tier End-to-End (E2E) Test Suite Runner

Usage:
    $(basename "$0") [OPTIONS]

Options:
    --mode <MODE>          Execution mode: 'offline', 'device', or 'auto' (default: auto)
    --tier <TIER>          Tier to run: 1, 2, 3, 4, or 'all' (default: all)
    --report-json <PATH>   Export machine-readable structured JSON report
    --report-xml <PATH>    Export JUnit XML test report
    -v, --verbose          Display detailed execution logs for each test case
    -h, --help             Display this help message and exit

Examples:
    ./tests/run_e2e_tests.sh --mode offline
    ./tests/run_e2e_tests.sh --mode device --verbose
    ./tests/run_e2e_tests.sh --tier 2
    ./tests/run_e2e_tests.sh --mode offline --report-json reports/e2e_report.json --report-xml reports/e2e_junit.xml
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            MODE="$2"
            shift 2
            ;;
        --tier)
            TIER="$2"
            shift 2
            ;;
        --report-json)
            REPORT_JSON="$2"
            shift 2
            ;;
        --report-xml)
            REPORT_XML="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1" >&2
            show_help
            exit 2
            ;;
    esac
done

# Check Python 3 environment
if ! command -v python3 &>/dev/null; then
    echo "[ERROR] python3 is required but not installed." >&2
    exit 2
fi

ARGS=(
    "--workspace" "${WORKSPACE_ROOT}"
    "--mode" "${MODE}"
    "--tier" "${TIER}"
)

if [[ -n "${REPORT_JSON}" ]]; then
    ARGS+=("--report-json" "${REPORT_JSON}")
fi

if [[ -n "${REPORT_XML}" ]]; then
    ARGS+=("--report-xml" "${REPORT_XML}")
fi

if [[ "${VERBOSE}" == true ]]; then
    ARGS+=("--verbose")
fi

export PYTHONPATH="${WORKSPACE_ROOT}:${PYTHONPATH:-}"
exec python3 "${WORKSPACE_ROOT}/tests/e2e/test_runner.py" "${ARGS[@]}"
