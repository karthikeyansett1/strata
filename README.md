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

The CLI returns exit code 3 for a missing key. Each successful write syncs the WAL to disk. The engine flushes the memtable to immutable SSTables, then atomically updates the manifest. On open, it loads the table metadata and replays the active WAL. The C++ API exposes `Flush()` and half-open `Scan(start, end)` ranges.
