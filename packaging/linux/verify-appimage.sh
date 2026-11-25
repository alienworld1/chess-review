#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST_DIR="${CHESS_REVIEW_DIST_DIR:-${ROOT_DIR}/dist}"
VERSION="1.0.0"
APPIMAGE_NAME="chess-review-${VERSION}-x86_64.AppImage"
APPIMAGE_PATH="${DIST_DIR}/${APPIMAGE_NAME}"

if [[ ! -x "${APPIMAGE_PATH}" ]]; then
    echo "Error: AppImage not found or not executable: ${APPIMAGE_PATH}" >&2
    exit 1
fi
if [[ ! -f "${APPIMAGE_PATH}.sha256" ]]; then
    echo "Error: Checksum file not found: ${APPIMAGE_PATH}.sha256" >&2
    exit 1
fi

(
    cd "${DIST_DIR}"
    sha256sum -c "${APPIMAGE_NAME}.sha256"
)

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

(
    cd "${WORK_DIR}"
    "${APPIMAGE_PATH}" --appimage-extract >/dev/null
)

EXTRACTED="${WORK_DIR}/squashfs-root"
test -x "${EXTRACTED}/AppRun"
test -x "${EXTRACTED}/usr/bin/chess-review"
test -f "${EXTRACTED}/usr/bin/qt.conf"

desktop-file-validate "${EXTRACTED}/chess-review.desktop"
appstreamcli validate --no-net "${EXTRACTED}/usr/share/metainfo/io.github.chess_review.chess-review.metainfo.xml"

for notice in THIRD_PARTY_NOTICES.md cburnett-LICENSE.txt cburnett-provenance.json chess-library-LICENSE chess-openings-COPYING.txt tomlplusplus-LICENSE; do
    test -f "${EXTRACTED}/usr/share/licenses/chess-review/${notice}"
done

if find "${EXTRACTED}" \( -iname '*stockfish*' -o -iname '*fake_uci*' \) | grep -q .; then
    echo "Error: bundled chess engine binary found inside AppImage." >&2
    exit 1
fi

while IFS= read -r -d '' elf; do
    ldd_out="$(env -i PATH=/usr/bin:/bin LD_LIBRARY_PATH="${EXTRACTED}/usr/lib" ldd "${elf}")"
    if grep -q 'not found' <<<"${ldd_out}"; then
        echo "Error: missing shared dependency for ${elf}:" >&2
        grep 'not found' <<<"${ldd_out}" >&2
        exit 1
    fi
    if grep -qE "(\.tools/|${ROOT_DIR})" <<<"${ldd_out}"; then
        echo "Error: host/developer path leaked into ldd for ${elf}:" >&2
        grep -E "(\.tools/|${ROOT_DIR})" <<<"${ldd_out}" >&2
        exit 1
    fi
done < <(find "${EXTRACTED}/usr/bin/chess-review" "${EXTRACTED}/usr/plugins" -type f -print0)

CLEAN_HOME="${WORK_DIR}/home"
CLEAN_CONFIG="${CLEAN_HOME}/.config"
CLEAN_DATA="${CLEAN_HOME}/.local/share"
CLEAN_CACHE="${CLEAN_HOME}/.cache"
CLEAN_RUNTIME="${WORK_DIR}/runtime"
mkdir -p "${CLEAN_HOME}" "${CLEAN_CONFIG}" "${CLEAN_DATA}" "${CLEAN_CACHE}" "${CLEAN_RUNTIME}"
chmod 0700 "${CLEAN_RUNTIME}"

run_clean() {
    env -i \
        HOME="${CLEAN_HOME}" \
        XDG_CONFIG_HOME="${CLEAN_CONFIG}" \
        XDG_DATA_HOME="${CLEAN_DATA}" \
        XDG_CACHE_HOME="${CLEAN_CACHE}" \
        XDG_RUNTIME_DIR="${CLEAN_RUNTIME}" \
        PATH="/usr/bin:/bin" \
        QT_QPA_PLATFORM="offscreen" \
        "$@"
}

version_out="$(run_clean "${EXTRACTED}/AppRun" --version)"
if [[ "${version_out}" != "chess-review ${VERSION}" ]]; then
    echo "Error: unexpected --version output: '${version_out}'" >&2
    exit 1
fi

help_out="$(run_clean "${EXTRACTED}/AppRun" --help)"
grep -q -- '--debug-logging' <<<"${help_out}"

appimage_version="$(run_clean "${APPIMAGE_PATH}" --appimage-extract-and-run --version)"
if [[ "${appimage_version}" != "chess-review ${VERSION}" ]]; then
    echo "Error: unexpected AppImage --version output: '${appimage_version}'" >&2
    exit 1
fi

APP_LOG="${WORK_DIR}/app-startup.log"
run_clean "${EXTRACTED}/AppRun" >"${APP_LOG}" 2>&1 &
APP_PID=$!

DB_PATH="${CLEAN_DATA}/chess-review/chess-review.sqlite3"
ready=0
for _ in $(seq 1 100); do
    if ! kill -0 "${APP_PID}" 2>/dev/null; then
        echo "Error: AppRun exited prematurely during clean-environment smoke test:" >&2
        cat "${APP_LOG}" >&2
        exit 1
    fi
    if [[ -f "${DB_PATH}" ]] && python3 - "${DB_PATH}" <<'PY' 2>/dev/null
import sqlite3, sys
con = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
required = {"schema_migrations", "games", "reviews", "review_jobs", "puzzles", "engines"}
if not required.issubset(tables):
    sys.exit(1)
ver = con.execute("SELECT COALESCE(MAX(version), 0) FROM schema_migrations").fetchone()[0]
sys.exit(0 if ver == 16 else 1)
PY
    then
        ready=1
        break
    fi
    sleep 0.1
done

if [[ "${ready}" -ne 1 ]]; then
    kill -TERM "${APP_PID}" 2>/dev/null || true
    wait "${APP_PID}" 2>/dev/null || true
    echo "Error: clean-environment AppRun startup did not produce expected schema v16 database." >&2
    cat "${APP_LOG}" >&2
    exit 1
fi

if grep -qiE 'could not load the Qt platform plugin|fatal|segmentation fault|aborted' "${APP_LOG}"; then
    kill -TERM "${APP_PID}" 2>/dev/null || true
    wait "${APP_PID}" 2>/dev/null || true
    echo "Error: startup warnings/errors detected:" >&2
    cat "${APP_LOG}" >&2
    exit 1
fi

kill -TERM "${APP_PID}" 2>/dev/null || true
wait "${APP_PID}" 2>/dev/null || true

echo "Verified AppImage clean-environment startup (schema v16 DB created, zero missing libs, zero host paths)."
