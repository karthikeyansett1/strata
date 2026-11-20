# Strata design

## Build environment

The target environment is Ubuntu 24.04. The development container selects Clang and provides Ninja, CMake, debugging tools, and Python development tooling. CI exercises debug and release builds plus separate AddressSanitizer and ThreadSanitizer configurations. GoogleTest is pinned to a specific revision.

The build requires CMake 3.24 or newer and a compiler that supports C++20. Exact versions from a successful Linux build are pending.
