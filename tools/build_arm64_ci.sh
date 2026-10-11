#!/usr/bin/env bash
# ==============================================================================
# build_arm64_ci.sh - MaliCompat ARM64 Remote CI Dispatcher & Artifact Downloader
# ==============================================================================
# This script triggers GitHub Actions CI for ARM64 compilation, monitors
# workflow execution, and retrieves built artifacts into the dist/ directory.
#
# RULE: All ARM64 C/C++ binaries must be compiled via GitHub Actions CI
#       (CodeAbhi826/Malicomp), never compiled directly on the host machine.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WORKFLOW_FILE="ci.yml"
WORKFLOW_NAME="MaliCompat CI / Build"
ARTIFACT_NAME="malicompat-arm64"
DEFAULT_BRANCH="master"
DEST_DIR="${REPO_ROOT}/dist"
TIMEOUT_SECS=900
WATCH_RUN=true
PUSH_CHANGES=false
COMMIT_MSG=""
CUSTOM_RUN_ID=""

usage() {
    cat << EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  -b, --branch <BRANCH>      Git branch to build (default: current branch or master)
  -p, --push                 Commit staged changes and push to origin before triggering CI
  -m, --message <MSG>        Commit message when used with --push
  -r, --run-id <ID>          Skip triggering; download artifacts from existing Run ID
  -d, --dest <DIR>           Destination directory for artifacts (default: dist/)
      --no-watch             Trigger CI and return immediately without waiting
  -t, --timeout <SECS>       Timeout in seconds while waiting for workflow (default: 900)
  -h, --help                 Display this help message and exit

Examples:
  # Trigger build on current branch, wait for completion, and download artifacts:
  ./tools/build_arm64_ci.sh

  # Commit modified files, push to master, and trigger CI:
  ./tools/build_arm64_ci.sh --push -m "feat(hook): implement vkCreateAndroidSurfaceKHR"

  # Download artifacts from a previous successful run:
  ./tools/build_arm64_ci.sh --run-id 38124972423
EOF
}

# Parse command line options
while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--branch)
            BRANCH="$2"
            shift 2
            ;;
        -p|--push)
            PUSH_CHANGES=true
            shift
            ;;
        -m|--message)
            COMMIT_MSG="$2"
            shift 2
            ;;
        -r|--run-id)
            CUSTOM_RUN_ID="$2"
            shift 2
            ;;
        -d|--dest)
            DEST_DIR="$2"
            shift 2
            ;;
        --no-watch)
            WATCH_RUN=false
            shift
            ;;
        -t|--timeout)
            TIMEOUT_SECS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

cd "${REPO_ROOT}"

# 1. Verify GitHub CLI prerequisites
echo "=== Step 1: Checking GitHub CLI Environment ==="
if ! command -v gh &> /dev/null; then
    echo "Error: GitHub CLI (gh) is not installed or not in PATH." >&2
    exit 1
fi

echo "Verifying GitHub authentication status..."
if ! gh auth status &> /dev/null; then
    echo "Error: GitHub CLI is not authenticated. Run 'gh auth login' or configure GH_TOKEN." >&2
    exit 1
fi
echo "✓ GitHub CLI authenticated successfully."

# 2. Determine target branch
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "${DEFAULT_BRANCH}")"
TARGET_BRANCH="${BRANCH:-${CURRENT_BRANCH}}"
echo "Target branch: ${TARGET_BRANCH}"

# Helper function to download and install artifacts
download_and_extract() {
    local run_id="$1"
    local dest="$2"

    echo "=== Downloading Artifacts for Run ID: ${run_id} ==="
    mkdir -p "${dest}/bin"

    local temp_dl
    temp_dl="$(mktemp -d /tmp/malicompat_dl_XXXXXX)"
    trap 'rm -rf "${temp_dl}"' EXIT

    echo "Downloading '${ARTIFACT_NAME}' from run ${run_id} to temporary workspace..."
    gh run download "${run_id}" -n "${ARTIFACT_NAME}" -D "${temp_dl}"

    # Handle both flat and nested extractions
    local src_dir="${temp_dl}"
    if [[ -d "${temp_dl}/${ARTIFACT_NAME}" ]]; then
        src_dir="${temp_dl}/${ARTIFACT_NAME}"
    fi

    echo "Syncing artifacts into destination: ${dest}"
    # Copy shared libraries
    if compgen -G "${src_dir}/*.so" > /dev/null; then
        cp -v "${src_dir}"/*.so "${dest}/"
        chmod 755 "${dest}"/*.so
    fi

    # Copy driver packages (.zip)
    if compgen -G "${src_dir}/*.zip" > /dev/null; then
        cp -v "${src_dir}"/*.zip "${dest}/"
    fi

    # Copy binary executables
    if [[ -d "${src_dir}/bin" ]] && compgen -G "${src_dir}/bin/*" > /dev/null; then
        cp -v "${src_dir}/bin"/* "${dest}/bin/"
        chmod 755 "${dest}/bin"/*
    fi

    rm -rf "${temp_dl}"
    trap - EXIT

    echo "=== Artifact Download Complete ==="
    echo "Contents of ${dest}:"
    ls -lh "${dest}"
    if [[ -d "${dest}/bin" ]]; then
        echo "Contents of ${dest}/bin:"
        ls -lh "${dest}/bin"
    fi

    # Validate binaries
    echo "=== Binary Architecture Verification ==="
    if [[ -f "${dest}/libmalicompat.so" ]]; then
        file "${dest}/libmalicompat.so"
    else
        echo "Warning: libmalicompat.so not found in ${dest}!" >&2
    fi

    for test_bin in malicompat-probe malicompat-shim-test malicompat-bc-test; do
        if [[ -f "${dest}/bin/${test_bin}" ]]; then
            file "${dest}/bin/${test_bin}"
        else
            echo "Warning: ${test_bin} not found in ${dest}/bin!" >&2
        fi
    done
}

# If custom run ID was requested, download directly
if [[ -n "${CUSTOM_RUN_ID}" ]]; then
    download_and_extract "${CUSTOM_RUN_ID}" "${DEST_DIR}"
    exit 0
fi

# 3. Handle Git Push if requested or if commits are unpushed
if [[ "${PUSH_CHANGES}" == "true" ]]; then
    echo "=== Step 2: Committing and Pushing Changes ==="
    if [[ -n "${COMMIT_MSG}" ]]; then
        echo "Staging changes and committing: ${COMMIT_MSG}"
        git add -A
        git commit -m "${COMMIT_MSG}" || true
    fi
    echo "Pushing commits to origin/${TARGET_BRANCH}..."
    git push origin "${TARGET_BRANCH}"
else
    # Check if local commits exist that haven't been pushed
    UNPUSHED=$(git log "origin/${TARGET_BRANCH}..HEAD" --oneline 2>/dev/null || true)
    if [[ -n "${UNPUSHED}" ]]; then
        echo "Warning: You have unpushed commits on ${TARGET_BRANCH}:"
        echo "${UNPUSHED}"
        read -rp "Push these commits before triggering CI? [Y/n] " CONFIRM
        CONFIRM="${CONFIRM:-Y}"
        if [[ "${CONFIRM}" =~ ^[Yy]$ ]]; then
            git push origin "${TARGET_BRANCH}"
        else
            echo "Proceeding with remote origin/${TARGET_BRANCH} as-is..."
        fi
    fi
fi

TARGET_COMMIT_SHA="$(git rev-parse "origin/${TARGET_BRANCH}" 2>/dev/null || git rev-parse HEAD)"
echo "Target remote commit SHA: ${TARGET_COMMIT_SHA}"

# 4. Trigger GitHub Actions Workflow
echo "=== Step 3: Triggering GitHub Actions Workflow ==="
echo "Dispatching workflow '${WORKFLOW_FILE}' on ref '${TARGET_BRANCH}'..."
gh workflow run "${WORKFLOW_FILE}" --ref "${TARGET_BRANCH}"

echo "Waiting for GitHub Actions to register the run..."
RUN_ID=""
START_TIME=$(date +%s)

while [[ -z "${RUN_ID}" ]]; do
    CURRENT_TIME=$(date +%s)
    ELAPSED=$((CURRENT_TIME - START_TIME))
    if [[ ${ELAPSED} -gt 60 ]]; then
        echo "Error: Timed out waiting for workflow run to register (60s)." >&2
        exit 1
    fi
    sleep 3

    # Look for queued or in_progress run matching target commit or recently created
    RUN_INFO=$(gh run list --workflow="${WORKFLOW_FILE}" --branch="${TARGET_BRANCH}" --limit 5 --json databaseId,headSha,status,createdAt -q '.[0]')
    if [[ -n "${RUN_INFO}" ]]; then
        FOUND_ID=$(echo "${RUN_INFO}" | jq -r '.databaseId // empty')
        FOUND_STATUS=$(echo "${RUN_INFO}" | jq -r '.status // empty')
        if [[ -n "${FOUND_ID}" ]]; then
            RUN_ID="${FOUND_ID}"
            break
        fi
    fi
done

echo "Found workflow run ID: ${RUN_ID}"
RUN_URL="https://github.com/CodeAbhi826/Malicomp/actions/runs/${RUN_ID}"
echo "Run URL: ${RUN_URL}"

if [[ "${WATCH_RUN}" == "false" ]]; then
    echo "Workflow dispatched successfully. Returning immediately (--no-watch)."
    exit 0
fi

# 5. Monitor Workflow Execution
echo "=== Step 4: Monitoring Workflow Execution ==="
echo "Watching run ${RUN_ID} (timeout: ${TIMEOUT_SECS}s)..."

set +e
gh run watch "${RUN_ID}" --exit-status
WATCH_EXIT_CODE=$?
set -e

if [[ ${WATCH_EXIT_CODE} -ne 0 ]]; then
    echo "Error: Workflow run ${RUN_ID} failed or completed with non-zero exit code (${WATCH_EXIT_CODE})." >&2
    echo "Viewing run details:"
    gh run view "${RUN_ID}"
    exit ${WATCH_EXIT_CODE}
fi

echo "✓ Workflow run ${RUN_ID} completed successfully!"

# 6. Download Artifacts into Destination
download_and_extract "${RUN_ID}" "${DEST_DIR}"

echo "=== All Steps Completed Successfully! ==="
