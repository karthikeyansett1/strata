# Strata design

## Build environment

The target environment is Ubuntu 24.04. The development container selects Clang and provides Ninja, CMake, debugging tools, and Python development tooling. CI exercises debug and release builds plus separate AddressSanitizer and ThreadSanitizer configurations. GoogleTest is pinned to a specific revision.

The build requires CMake 3.24 or newer and a compiler that supports C++20. The Phase 0 Linux environment on macOS used Colima 0.10.3, Ubuntu 24.04 on aarch64, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1, and Python 3.12.3.

On this Colima setup, Docker's default syscall filter blocked ThreadSanitizer from setting its required address-space layout. The same instrumented smoke test passed when run directly in the Linux VM with `colima ssh -- "$PWD/build-tsan/strata_smoke_test"`.

## Phase 1 write-ahead log

The single mutable memtable uses `wal-1.log` in the database directory. The database creates the leaf directory if needed. Its parent must already exist. Opening the WAL acquires an exclusive advisory `flock` for the lifetime of the handle and syncs the database and parent directories before the handle can accept a write. A second opener gets `kBusy`. External modifications to a live database directory are unsupported.

Every field with more than one byte is encoded little-endian.

| Field | Bytes | Meaning |
|---|---:|---|
| CRC32C | 4 | Covers the encoded length and body |
| Length | 4 | Number of bytes in the body |
| Type | 1 | Put = 1, delete = 2 |
| Sequence | 8 | Contiguous write sequence starting at 1 |
| Key length | 4 | Bytes in the key |
| Key | Variable | Binary key |
| Value length | 4 | Bytes in the value, zero for a delete |
| Value | Variable | Binary payload |

The body is limited to 64 MiB. A put with an empty value is distinct from a delete. Appends handle interrupted or short writes and call `fsync` before publishing to the memtable. Both sync-mode settings use one `fsync` per write during Phase 1. Group commit is scheduled for Phase 3.

Recovery verifies checksums, structure, and sequences before applying records. At EOF, a truncated record or a final checksum mismatch is treated as a torn append. The WAL is truncated to the preceding valid boundary and synced before new writes. A checksum mismatch with bytes beyond the advertised frame is corruption. When a suspect length points to or past EOF and disagrees with the body metadata or the metadata is incomplete, recovery looks for a later independently valid frame before truncating. A complete final frame with a bad checksum is indistinguishable from damage to that final frame under this format. A detected interior error returns `kCorruption` and preserves the file.

An append or sync error leaves durability uncertain. The handle rejects further writes until reopened, and existing reads continue against the last published memtable state. An allocation failure after sync has the same outcome. Recovery may replay a complete write whose caller never received success.

## Phase 1 concurrency

The DB writer mutex serializes sequence allocation, WAL appends and sync, and publication to the memtable. The skiplist's shared mutex protects its nodes, higher-level links, and height generator. Reads take only the skiplist's shared lock, so they can continue during WAL I/O. Writes acquire the DB writer mutex before taking the skiplist's exclusive lock. Recovery runs under the file lock before publishing the DB handle. The caller must keep the handle alive for the full duration of every operation.
