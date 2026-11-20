# Strata

[![CI](https://github.com/karthikeyansett1/strata/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/karthikeyansett1/strata/actions/workflows/ci.yml)

Strata is a C++20 project targeting a crash-safe, multithreaded LSM-tree storage engine for Linux with an epoll server, HNSW vector search, and Python bindings. The current scaffold contains the build system and a library smoke test.

Run the build and test with this command. On macOS, the wrapper uses Docker to run it on Ubuntu.

```sh
./scripts/dev.sh bash -ec 'cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure'
```

Configure separate build directories with `-DSTRATA_ASAN=ON` or `-DSTRATA_TSAN=ON` to test with either sanitizer.

If Colima provides Docker on macOS, set `DOCKER_CONTEXT=colima` when running the wrapper.
