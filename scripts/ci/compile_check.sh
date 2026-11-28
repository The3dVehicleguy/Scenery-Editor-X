#!/usr/bin/env bash
# Compiles the editor sources whose per-OS code the unit suites (build_unit_tests.sh) can't build, because they need a
# library the suites don't, so that code is compiled on Linux, macOS and Windows on every pull request and not first in
# a user's build: update/update_client.cpp (libcurl). Compile only: the editor build links them and the editor runs
# them. The engine logger (spdlog, xMath) is replaced by scripts/ci/stubs, which takes the arguments and does nothing.
#
#   bash scripts/ci/compile_check.sh
#
# Needs libcurl's headers: libcurl4-openssl-dev on Linux, the macOS SDK's own, mingw-w64-ucrt-x86_64-curl in MSYS2;
# and the json submodule. The unit-tests workflow (.github/workflows/unit-tests.yml) runs it after the suites.
#
# Environment: CXX (default g++, clang++ on macOS), SEDX_TEST_OUT (default build/unit-tests; objects go in its
# compile-check/), MACOSX_DEPLOYMENT_TARGET (macOS, default 13.0 like the editor build).

set -euo pipefail
cd "$(dirname "$0")/../.."

case "$(uname -s)" in
	Darwin) PLATFORM=macos ;;
	*_NT*|MINGW*|MSYS*|CYGWIN*) PLATFORM=windows ;;
	*) PLATFORM=linux ;;
esac

PLATFORM_FLAGS=""
if [ "$PLATFORM" = macos ]; then
	: "${CXX:=clang++}"
	: "${MACOSX_DEPLOYMENT_TARGET:=13.0}"
	PLATFORM_FLAGS="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
else
	: "${CXX:=g++}"
fi

OUT=${SEDX_TEST_OUT:-build/unit-tests}/compile-check
mkdir -p "$OUT"

SOURCES="source/SceneryEditorX/update/update_client.cpp"

echo "Compile check on $PLATFORM with $CXX"
status=0
for src in $SOURCES; do
	obj="$OUT/$(basename "$src" .cpp).o"
	echo "  c++ $src"
	# CXX may hold a launcher ("ccache g++"); PLATFORM_FLAGS is empty or one flag
	# shellcheck disable=SC2086
	if ! $CXX -std=c++20 -O2 -Wall -Wextra -pthread $PLATFORM_FLAGS -Iscripts/ci/stubs -Isource -Idependency/json/include \
		-c "$src" -o "$obj"; then
		echo "FAILED to compile $src"
		status=1
	fi
done
exit "$status"
