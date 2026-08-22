#!/usr/bin/env bash

set -euo pipefail

readonly MANNY_SCRIPT_DIRECTORY="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly MANNY_DEFAULT_SOURCE_DIRECTORY="$(cd -- "${MANNY_SCRIPT_DIRECTORY}/.." && pwd -P)"
readonly MANNY_SOURCE_DIRECTORY="${MANNY_SOURCE_DIRECTORY:-${MANNY_DEFAULT_SOURCE_DIRECTORY}}"
readonly MANNY_CACHE_ROOT="${MANNY_CACHE_ROOT:-${XDG_CACHE_HOME:-${HOME}/.cache}/GW2-MannyUploader}"
readonly MANNY_MSVC_ROOT="${MANNY_MSVC_ROOT:-${MANNY_CACHE_ROOT}/msvc18}"
readonly MANNY_WINE_PREFIX="${MANNY_WINE_PREFIX:-${MANNY_CACHE_ROOT}/msvc-wine-prefix}"
readonly MANNY_CLANG_FORMAT="${MANNY_CLANG_FORMAT:-${MANNY_CACHE_ROOT}/llvm18/bin/clang-format}"
readonly MANNY_CLANG_FORMAT_LIBRARY_DIRECTORY="${MANNY_CLANG_FORMAT_LIBRARY_DIRECTORY:-${MANNY_CACHE_ROOT}/llvm18/lib}"
readonly MANNY_LINUX_BUILD_DIRECTORY="${MANNY_LINUX_BUILD_DIRECTORY:-${MANNY_SOURCE_DIRECTORY}/out/build/local-linux-debug}"
readonly MANNY_DEBUG_BUILD_DIRECTORY="${MANNY_DEBUG_BUILD_DIRECTORY:-${MANNY_SOURCE_DIRECTORY}/out/build/local-msvc-debug}"
readonly MANNY_RELEASE_BUILD_DIRECTORY="${MANNY_RELEASE_BUILD_DIRECTORY:-${MANNY_SOURCE_DIRECTORY}/out/build/local-msvc-release}"
readonly MANNY_DEBUG_DEPENDENCY_DIRECTORY="${MANNY_DEBUG_DEPENDENCY_DIRECTORY:-${MANNY_SOURCE_DIRECTORY}/out/build/local-msvc-deps-debug}"
readonly MANNY_RELEASE_DEPENDENCY_DIRECTORY="${MANNY_RELEASE_DEPENDENCY_DIRECTORY:-${MANNY_SOURCE_DIRECTORY}/out/build/local-msvc-deps-release}"
readonly MANNY_BUILD_JOBS="${MANNY_BUILD_JOBS:-4}"
readonly MANNY_RUN_LINUX="${MANNY_RUN_LINUX:-1}"

find_clion_tool() {
    local relative_path="$1"
    local candidate=""

    for candidate in /opt/jetbrains/clion-*/"${relative_path}"; do
        if [[ -x "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
        fi
    done | sort -V | tail -n 1
}

if [[ -z "${MANNY_CMAKE:-}" ]]; then
    MANNY_CMAKE="$(command -v cmake || true)"
    if [[ -z "${MANNY_CMAKE}" ]]; then
        MANNY_CMAKE="$(find_clion_tool bin/cmake/linux/x64/bin/cmake)"
    fi
fi
readonly MANNY_CMAKE

if [[ -z "${MANNY_NINJA:-}" ]]; then
    MANNY_NINJA="$(command -v ninja || true)"
    if [[ -z "${MANNY_NINJA}" ]]; then
        MANNY_NINJA="$(find_clion_tool bin/ninja/linux/x64/ninja)"
    fi
fi
readonly MANNY_NINJA

if [[ -z "${MANNY_WINE:-}" ]]; then
    MANNY_WINE="$(command -v wine || true)"
fi
readonly MANNY_WINE

readonly MANNY_CMAKE_BIN_DIRECTORY="$(dirname -- "${MANNY_CMAKE}")"
readonly MANNY_CTEST="${MANNY_CTEST:-${MANNY_CMAKE_BIN_DIRECTORY}/ctest}"
readonly MANNY_CPACK="${MANNY_CPACK:-${MANNY_CMAKE_BIN_DIRECTORY}/cpack}"
readonly MANNY_CL="${MANNY_CL:-${MANNY_MSVC_ROOT}/bin/x64/cl}"
readonly MANNY_RC="${MANNY_RC:-${MANNY_MSVC_ROOT}/bin/x64/rc}"

require_executable() {
    local path="$1"
    local description="$2"

    if [[ -z "${path}" || ! -x "${path}" ]]; then
        printf 'error: %s is missing or not executable: %s\n' "${description}" "${path:-<unset>}" >&2
        exit 1
    fi
}

require_directory() {
    local path="$1"
    local description="$2"

    if [[ ! -d "${path}" ]]; then
        printf 'error: %s is missing: %s\n' "${description}" "${path}" >&2
        exit 1
    fi
}

case "${MANNY_BUILD_JOBS}" in
    ''|*[!0-9]*|0)
        printf 'error: MANNY_BUILD_JOBS must be a positive integer\n' >&2
        exit 1
        ;;
esac

if [[ "${MANNY_RUN_LINUX}" != "0" && "${MANNY_RUN_LINUX}" != "1" ]]; then
    printf 'error: MANNY_RUN_LINUX must be 0 or 1\n' >&2
    exit 1
fi

require_directory "${MANNY_SOURCE_DIRECTORY}" "source directory"
require_directory "${MANNY_MSVC_ROOT}" "local MSVC toolchain"
require_directory "${MANNY_WINE_PREFIX}" "MSVC Wine prefix"
require_executable "${MANNY_CMAKE}" "CMake"
require_executable "${MANNY_CTEST}" "CTest"
require_executable "${MANNY_CPACK}" "CPack"
require_executable "${MANNY_NINJA}" "Ninja"
require_executable "${MANNY_WINE}" "Wine"
require_executable "${MANNY_CL}" "genuine MSVC x64 compiler wrapper"
require_executable "${MANNY_RC}" "Microsoft resource compiler wrapper"
require_executable "${MANNY_CLANG_FORMAT}" "clang-format 18"

if [[ -d "${MANNY_CLANG_FORMAT_LIBRARY_DIRECTORY}" ]]; then
    readonly MANNY_CLANG_FORMAT_LD_LIBRARY_PATH="${MANNY_CLANG_FORMAT_LIBRARY_DIRECTORY}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
else
    readonly MANNY_CLANG_FORMAT_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
fi

readonly MANNY_TOOL_PATH="${MANNY_MSVC_ROOT}/bin/x64:/usr/bin:${PATH}"
export PATH="${MANNY_TOOL_PATH}"
export WINEPREFIX="${MANNY_WINE_PREFIX}"
export WINEARCH=win64
export WINEDEBUG=-all

readonly MANNY_CL_OUTPUT="$("${MANNY_CL}" 2>&1 || true)"
MANNY_CL_VERSION_LINE=""
while IFS= read -r line; do
    if [[ "${line}" == *"Microsoft (R) C/C++ Optimizing Compiler Version"* ]]; then
        MANNY_CL_VERSION_LINE="${line}"
        break
    fi
done <<< "${MANNY_CL_OUTPUT}"
readonly MANNY_CL_VERSION_LINE
if [[ -z "${MANNY_CL_VERSION_LINE}" ]]; then
    printf 'error: configured compiler did not identify itself as Microsoft MSVC\n' >&2
    exit 1
fi

readonly MANNY_FORMAT_OUTPUT="$(LD_LIBRARY_PATH="${MANNY_CLANG_FORMAT_LD_LIBRARY_PATH}" "${MANNY_CLANG_FORMAT}" --version)"
if [[ "${MANNY_FORMAT_OUTPUT}" != *"clang-format version 18."* ]]; then
    printf 'error: source formatting requires clang-format 18, got: %s\n' "${MANNY_FORMAT_OUTPUT}" >&2
    exit 1
fi

printf '[preflight] %s\n' "${MANNY_CL_VERSION_LINE}"
printf '[preflight] %s\n' "${MANNY_FORMAT_OUTPUT}"

printf '[preflight] checking source formatting\n'
LD_LIBRARY_PATH="${MANNY_CLANG_FORMAT_LD_LIBRARY_PATH}" "${MANNY_CMAKE}" \
    -DMANNY_SOURCE_DIRECTORY="${MANNY_SOURCE_DIRECTORY}" \
    -DMANNY_CLANG_FORMAT_EXECUTABLE="${MANNY_CLANG_FORMAT}" \
    -P "${MANNY_SOURCE_DIRECTORY}/cmake/CheckSourceFormatting.cmake"

printf '[preflight] checking whitespace\n'
git -C "${MANNY_SOURCE_DIRECTORY}" diff --check

if [[ "${MANNY_RUN_LINUX}" == "1" ]]; then
    printf '[preflight] configuring Linux Debug\n'
    "${MANNY_CMAKE}" \
        -S "${MANNY_SOURCE_DIRECTORY}" \
        -B "${MANNY_LINUX_BUILD_DIRECTORY}" \
        -G Ninja \
        -DCMAKE_MAKE_PROGRAM="${MANNY_NINJA}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_TESTING=ON \
        -DMANNY_WARNINGS_AS_ERRORS=ON

    printf '[preflight] building Linux Debug\n'
    "${MANNY_CMAKE}" --build "${MANNY_LINUX_BUILD_DIRECTORY}" --parallel "${MANNY_BUILD_JOBS}"

    printf '[preflight] testing Linux Debug\n'
    "${MANNY_CTEST}" \
        --test-dir "${MANNY_LINUX_BUILD_DIRECTORY}" \
        --output-on-failure
fi

configure_windows() {
    local configuration="$1"
    local build_directory="$2"
    local dependency_directory="$3"

    printf '[preflight] configuring genuine MSVC x64 %s\n' "${configuration}"
    "${MANNY_CMAKE}" \
        -S "${MANNY_SOURCE_DIRECTORY}" \
        -B "${build_directory}" \
        -G Ninja \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_BUILD_TYPE="${configuration}" \
        -DCMAKE_C_COMPILER="${MANNY_CL}" \
        -DCMAKE_CXX_COMPILER="${MANNY_CL}" \
        -DCMAKE_RC_COMPILER="${MANNY_RC}" \
        -DCMAKE_MAKE_PROGRAM="${MANNY_NINJA}" \
        -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded \
        -DFETCHCONTENT_BASE_DIR="${dependency_directory}" \
        -DBUILD_TESTING=ON \
        -DMANNY_WARNINGS_AS_ERRORS=ON

    printf '[preflight] building genuine MSVC x64 %s\n' "${configuration}"
    "${MANNY_CMAKE}" --build "${build_directory}" --parallel "${MANNY_BUILD_JOBS}"
}

to_wine_path() {
    local unix_path="$1"
    printf 'Z:%s\n' "${unix_path//\//\\}"
}

run_windows_suite() {
    local configuration="$1"
    local build_directory="$2"
    local tests_directory="${build_directory}/tests"
    local dll_path="${build_directory}/bin/manny_uploader.dll"

    require_executable "${tests_directory}/manny_uploader_tests.exe" "${configuration} Windows test executable"
    require_executable "${tests_directory}/manny_nexus_addon_smoke_test.exe" "${configuration} Nexus smoke host"

    printf '[preflight] testing genuine MSVC x64 %s under Wine\n' "${configuration}"
    MANNY_REQUIRE_WINE_DPAPI=1 "${MANNY_WINE}" "$(to_wine_path "${tests_directory}/manny_uploader_tests.exe")"

    printf '[preflight] smoke-testing genuine MSVC x64 %s DLL under Wine\n' "${configuration}"
    "${MANNY_WINE}" \
        "$(to_wine_path "${tests_directory}/manny_nexus_addon_smoke_test.exe")" \
        "$(to_wine_path "${dll_path}")"
}

configure_windows Debug "${MANNY_DEBUG_BUILD_DIRECTORY}" "${MANNY_DEBUG_DEPENDENCY_DIRECTORY}"
run_windows_suite Debug "${MANNY_DEBUG_BUILD_DIRECTORY}"

configure_windows Release "${MANNY_RELEASE_BUILD_DIRECTORY}" "${MANNY_RELEASE_DEPENDENCY_DIRECTORY}"
run_windows_suite Release "${MANNY_RELEASE_BUILD_DIRECTORY}"

printf '[preflight] testing Windows package and symbol contracts\n'
"${MANNY_CTEST}" \
    --test-dir "${MANNY_RELEASE_BUILD_DIRECTORY}" \
    -C Release \
    --output-on-failure \
    -R '^windows_'

printf '[preflight] creating Release package\n'
"${MANNY_CPACK}" \
    --config "${MANNY_RELEASE_BUILD_DIRECTORY}/CPackConfig.cmake" \
    -C Release

readonly MANNY_VERIFY_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/manny-msvc-preflight.XXXXXX")"
readonly MANNY_PACKAGE_EXTRACT_DIRECTORY="${MANNY_VERIFY_ROOT}/package-verified"

cleanup_verify_root() {
    case "${MANNY_VERIFY_ROOT}" in
        "${TMPDIR:-/tmp}"/manny-msvc-preflight.*)
            "${MANNY_CMAKE}" -E remove_directory "${MANNY_VERIFY_ROOT}"
            ;;
    esac
}
trap cleanup_verify_root EXIT

printf '[preflight] verifying Release package\n'
"${MANNY_CMAKE}" \
    -DMANNY_PACKAGE_OUTPUT_DIRECTORY="${MANNY_RELEASE_BUILD_DIRECTORY}/package" \
    -DMANNY_PACKAGE_EXTRACT_DIRECTORY="${MANNY_PACKAGE_EXTRACT_DIRECTORY}" \
    -P "${MANNY_SOURCE_DIRECTORY}/cmake/VerifyWindowsPackage.cmake"

printf '[preflight] smoke-testing packaged Release DLL under Wine\n'
"${MANNY_WINE}" \
    "$(to_wine_path "${MANNY_RELEASE_BUILD_DIRECTORY}/tests/manny_nexus_addon_smoke_test.exe")" \
    "$(to_wine_path "${MANNY_PACKAGE_EXTRACT_DIRECTORY}/manny_uploader.dll")"

readonly MANNY_SYMBOL_FILE="${MANNY_RELEASE_BUILD_DIRECTORY}/symbols/Release/manny_uploader.pdb"

printf '[preflight] staging and verifying Release linker symbols\n'
"${MANNY_CMAKE}" \
    -DMANNY_SYMBOL_SEARCH_DIRECTORY="${MANNY_RELEASE_BUILD_DIRECTORY}" \
    -DMANNY_SYMBOL_FILE="${MANNY_SYMBOL_FILE}" \
    -P "${MANNY_SOURCE_DIRECTORY}/cmake/StageWindowsSymbols.cmake"
"${MANNY_CMAKE}" \
    -DMANNY_SYMBOL_FILE="${MANNY_SYMBOL_FILE}" \
    -P "${MANNY_SOURCE_DIRECTORY}/cmake/VerifyWindowsSymbols.cmake"

if [[ "${MANNY_RUN_LINUX}" == "1" ]]; then
    printf '[preflight] all local Linux and genuine-MSVC-under-Wine checks passed\n'
else
    printf '[preflight] all requested genuine-MSVC-under-Wine checks passed (Linux skipped)\n'
fi
