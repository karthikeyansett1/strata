# Strata design

## Build environment

The target environment is Ubuntu 24.04. The development container selects Clang and provides Ninja, CMake, debugging tools, and Python development tooling. CI exercises debug and release builds plus separate AddressSanitizer and ThreadSanitizer configurations. GoogleTest is pinned to a specific revision.

The build requires CMake 3.24 or newer and a compiler that supports C++20. The Phase 0 Linux environment on macOS used Colima 0.10.3, Ubuntu 24.04 on aarch64, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1, and Python 3.12.3.

On this Colima setup, Docker's default syscall filter blocked ThreadSanitizer from setting its required address-space layout. The same instrumented smoke test passed when run directly in the Linux VM with `colima ssh -- "$PWD/build-tsan/strata_smoke_test"`.
