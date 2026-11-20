#!/usr/bin/env bash
set -euo pipefail

build_dir=${1:-build-verify}
python=$(command -v "${STRATA_PYTHON:-python3}")
cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTRATA_BUILD_PYTHON=ON \
    -DPython_EXECUTABLE="$python"
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
ctest --test-dir "$build_dir" --output-on-failure --no-tests=error --verbose
PYTHONPATH="$build_dir" "$python" -m unittest discover -s python/tests -v
