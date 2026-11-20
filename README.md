# Strata

[![CI](https://github.com/karthikeyansett1/strata/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/karthikeyansett1/strata/actions/workflows/ci.yml)

Strata is a C++20 storage engine for Linux. Its current API supports concurrent point reads and writes, deletes, ordered range scans, and recovery from checksummed SSTables and a write-ahead log. The project is building toward an LSM tree with an epoll server, HNSW vector search, and Python bindings.

Build the library and CLI and run the tests. On macOS, the wrapper uses Docker to run on Ubuntu.

```sh
./scripts/dev.sh bash -ec 'cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure'
```

If Colima provides Docker, set `DOCKER_CONTEXT=colima` when invoking the wrapper. Configure separate build directories with `-DSTRATA_ASAN=ON` or `-DSTRATA_TSAN=ON` to test with either sanitizer.

Try the CLI after building. On macOS, prefix these commands with `./scripts/dev.sh`.

```sh
build/strata_cli ./data put sample 'hello Strata'
build/strata_cli ./data get sample
build/strata_cli ./data del sample
```

The CLI returns exit code 3 for a missing key. Successful writes are acknowledged after WAL sync. The C++ API supports per-write sync or group commit. The engine flushes memtables and compacts immutable SSTables in the background. On open, it loads the manifest and replays the referenced WALs. The API exposes `Flush()`, `CompactAll()`, and half-open `Scan(start, end)` ranges.

## TCP server

Start the Linux server with the build above.

```sh
build/strata_server --port 7379 --data-dir ./data --sync-mode batch --threads 4
```

Connect over TCP and send newline-terminated `PUT key value`, `GET key`, `DEL key`, `SCAN start end`, or `STATS` commands. Requests may be pipelined. `SCAN` and `STATS` finish with an `END` line.

## Verification and measurements

Build in Release mode before running the benchmark and crash harness on Linux.

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./scripts/run_bench.sh build-release/strata_bench 20000 results/bench.csv
python3 scripts/plot_bench.py results/bench.csv
./scripts/crash_torture.sh
```

Run the commands through `./scripts/dev.sh` on macOS. The container includes Matplotlib for the charts. See the [benchmark CSV](results/bench.csv), [throughput](results/throughput.png), [p99 latency](results/latency.png), and [crash-test summary](results/torture.log).
