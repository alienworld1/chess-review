#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${CHESS_REVIEW_APPIMAGE_BUILD_DIR:-${ROOT_DIR}/build/appimage}"
APPDIR="${BUILD_DIR}/AppDir"
DIST_DIR="${CHESS_REVIEW_DIST_DIR:-${ROOT_DIR}/dist}"
VERSION="1.0.0"
APPIMAGE_NAME="chess-review-${VERSION}-x86_64.AppImage"
APPIMAGE_PATH="${DIST_DIR}/${APPIMAGE_NAME}"

QT_PREFIX="${QT_PREFIX:-${CMAKE_PREFIX_PATH:-${ROOT_DIR}/.tools/Qt/6.12.0/gcc_64}}"
if [[ ! -d "${QT_PREFIX}/lib" || ! -d "${QT_PREFIX}/plugins" ]]; then
    echo "Error: Qt prefix not found at '${QT_PREFIX}'. Set QT_PREFIX to your Qt 6.12 prefix." >&2
    exit 1
fi

APPIMAGETOOL_BIN="${APPIMAGETOOL:-}"
if [[ -z "${APPIMAGETOOL_BIN}" ]]; then
    if command -v appimagetool >/dev/null 2>&1; then
        APPIMAGETOOL_BIN="$(command -v appimagetool)"
    elif [[ -x "${ROOT_DIR}/.tools/appimagetool-x86_64.AppImage" ]]; then
        APPIMAGETOOL_BIN="${ROOT_DIR}/.tools/appimagetool-x86_64.AppImage"
    else
        echo "Error: appimagetool not found. Set APPIMAGETOOL or place appimagetool-x86_64.AppImage in .tools/." >&2
        exit 1
    fi
fi

rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/lib" "${APPDIR}/usr/plugins" "${DIST_DIR}"

CMAKE_ARGS=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=OFF
    -DCMAKE_INSTALL_PREFIX=/usr
    -DCMAKE_PREFIX_PATH="${QT_PREFIX}"
)
if [[ -d "${ROOT_DIR}/.tools/tomlplusplus-3.4.0" ]]; then
    CMAKE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=${ROOT_DIR}/.tools/tomlplusplus-3.4.0")
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}/cmake" "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}/cmake"
DESTDIR="${APPDIR}" cmake --install "${BUILD_DIR}/cmake"

install -m 0755 "${ROOT_DIR}/packaging/linux/AppRun" "${APPDIR}/AppRun"
install -m 0644 "${ROOT_DIR}/packaging/linux/qt.conf" "${APPDIR}/usr/bin/qt.conf"
cp "${APPDIR}/usr/share/applications/chess-review.desktop" "${APPDIR}/chess-review.desktop"
cp "${APPDIR}/usr/share/icons/hicolor/256x256/apps/chess-review.png" "${APPDIR}/chess-review.png"
cp "${APPDIR}/usr/share/icons/hicolor/256x256/apps/chess-review.png" "${APPDIR}/.DirIcon"
cp "${APPDIR}/usr/share/icons/hicolor/scalable/apps/chess-review.svg" "${APPDIR}/chess-review.svg"

copy_plugin() {
    local category="$1"
    local file="$2"
    local src="${QT_PREFIX}/plugins/${category}/${file}"
    if [[ -f "${src}" ]]; then
        mkdir -p "${APPDIR}/usr/plugins/${category}"
        cp -aL "${src}" "${APPDIR}/usr/plugins/${category}/${file}"
    fi
}

copy_plugin_dir() {
    local category="$1"
    local dir="${QT_PREFIX}/plugins/${category}"
    if [[ -d "${dir}" ]]; then
        mkdir -p "${APPDIR}/usr/plugins/${category}"
        local so
        shopt -s nullglob
        for so in "${dir}"/*.so; do
            cp -aL "${so}" "${APPDIR}/usr/plugins/${category}/$(basename "${so}")"
        done
        shopt -u nullglob
    fi
}

for p in libqxcb.so libqoffscreen.so libqwayland.so libqminimal.so; do
    copy_plugin platforms "${p}"
done
copy_plugin sqldrivers libqsqlite.so
for p in libqsvg.so libqjpeg.so libqico.so; do
    copy_plugin imageformats "${p}"
done
copy_plugin iconengines libqsvgicon.so
for p in libqopensslbackend.so libqcertonlybackend.so; do
    copy_plugin tls "${p}"
done
copy_plugin platformthemes libqxdgdesktopportal.so
copy_plugin platforminputcontexts libcomposeplatforminputcontextplugin.so
copy_plugin platforminputcontexts libibusplatforminputcontextplugin.so
copy_plugin_dir xcbglintegrations
copy_plugin_dir wayland-decoration-client
copy_plugin_dir wayland-graphics-integration-client
copy_plugin_dir wayland-shell-integration

LIB_SEARCH_DIRS=("${QT_PREFIX}/lib")
if [[ -d "${ROOT_DIR}/.tools/xcb-cursor/usr/lib/x86_64-linux-gnu" ]]; then
    LIB_SEARCH_DIRS+=("${ROOT_DIR}/.tools/xcb-cursor/usr/lib/x86_64-linux-gnu")
fi
LIB_SEARCH_DIRS+=(/lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)

declare -A BUNDLED_LIBS=()
QUEUE=()

enqueue_elf() {
    QUEUE+=("$1")
}

copy_library_if_local() {
    local soname="$1"
    if [[ -n "${BUNDLED_LIBS[${soname}]+x}" ]]; then
        return 0
    fi
    local dir candidate=""
    for dir in "${LIB_SEARCH_DIRS[@]}"; do
        if [[ "${dir}" == "${QT_PREFIX}/lib" || "${dir}" == *"/.tools/xcb-cursor/"* ]]; then
            if [[ -e "${dir}/${soname}" ]]; then
                candidate="${dir}/${soname}"
                break
            fi
        elif [[ "${soname}" == libxcb-cursor.so* && -e "${dir}/${soname}" ]]; then
            candidate="${dir}/${soname}"
            break
        fi
    done
    if [[ -n "${candidate}" ]]; then
        cp -aL "${candidate}" "${APPDIR}/usr/lib/${soname}"
        BUNDLED_LIBS["${soname}"]=1
        enqueue_elf "${APPDIR}/usr/lib/${soname}"
    fi
}

enqueue_elf "${APPDIR}/usr/bin/chess-review"
while IFS= read -r -d '' plugin; do
    enqueue_elf "${plugin}"
done < <(find "${APPDIR}/usr/plugins" -type f -name '*.so' -print0)

idx=0
while ((idx < ${#QUEUE[@]})); do
    elf="${QUEUE[$idx]}"
    ((idx += 1))
    while IFS= read -r needed; do
        [[ -n "${needed}" ]] || continue
        copy_library_if_local "${needed}"
    done < <(readelf -d "${elf}" | awk -F'[][]' '/\(NEEDED\)/ { print $2 }')
done

if command -v strip >/dev/null 2>&1; then
    chmod u+w "${APPDIR}/usr/bin/chess-review"
    strip --strip-unneeded "${APPDIR}/usr/bin/chess-review"
    find "${APPDIR}/usr/lib" "${APPDIR}/usr/plugins" -type f -print0 | while IFS= read -r -d '' f; do
        chmod u+w "${f}"
        strip --strip-unneeded "${f}" >/dev/null 2>&1 || true
    done
fi

while IFS= read -r -d '' elf; do
    if LD_LIBRARY_PATH="${APPDIR}/usr/lib" ldd "${elf}" | grep -q 'not found'; then
        echo "Error: unresolved shared library in ${elf}:" >&2
        LD_LIBRARY_PATH="${APPDIR}/usr/lib" ldd "${elf}" | grep 'not found' >&2
        exit 1
    fi
    if LD_LIBRARY_PATH="${APPDIR}/usr/lib" ldd "${elf}" | grep -q '\.tools/'; then
        echo "Error: ${elf} still links into .tools/:" >&2
        LD_LIBRARY_PATH="${APPDIR}/usr/lib" ldd "${elf}" | grep '\.tools/' >&2
        exit 1
    fi
done < <(find "${APPDIR}/usr/bin/chess-review" "${APPDIR}/usr/plugins" "${APPDIR}/usr/lib" -type f -print0)

if find "${APPDIR}" \( -iname '*stockfish*' -o -iname '*fake_uci*' \) | grep -q .; then
    echo "Error: bundled chess engine detected inside AppDir." >&2
    exit 1
fi

desktop-file-validate "${APPDIR}/chess-review.desktop"
appstreamcli validate --no-net "${APPDIR}/usr/share/metainfo/io.github.chess_review.chess-review.metainfo.xml"

rm -f "${APPIMAGE_PATH}" "${APPIMAGE_PATH}.sha256"
APPIMAGETOOL_ARGS=()
if [[ "${APPIMAGETOOL_BIN}" == *.AppImage ]]; then
    APPIMAGETOOL_ARGS+=(--appimage-extract-and-run)
fi
APPIMAGE_RUNTIME_FILE="${APPIMAGE_RUNTIME:-}"
if [[ -z "${APPIMAGE_RUNTIME_FILE}" && -f "${ROOT_DIR}/.tools/runtime-x86_64" ]]; then
    APPIMAGE_RUNTIME_FILE="${ROOT_DIR}/.tools/runtime-x86_64"
fi
if [[ -n "${APPIMAGE_RUNTIME_FILE}" ]]; then
    APPIMAGETOOL_ARGS+=(--runtime-file "${APPIMAGE_RUNTIME_FILE}")
fi
ARCH=x86_64 "${APPIMAGETOOL_BIN}" "${APPIMAGETOOL_ARGS[@]}" "${APPDIR}" "${APPIMAGE_PATH}"
chmod 0755 "${APPIMAGE_PATH}"

(
    cd "${DIST_DIR}"
    sha256sum "${APPIMAGE_NAME}" > "${APPIMAGE_NAME}.sha256"
)

echo "Built AppImage: ${APPIMAGE_PATH}"
cat "${APPIMAGE_PATH}.sha256"
