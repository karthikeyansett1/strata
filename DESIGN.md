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

## Phase 2 SSTables and manifest

A synchronous flush writes the sorted latest memtable entries, including tombstones, to a new level-0 SSTable. The rotation budget measures encoded WAL bytes since the last flush, including overwrites, and is checked before the next write. `Flush()` can rotate sooner. Closing a handle leaves its active WAL available for replay. Phase 2 has one on-disk level, with newest-to-oldest point lookup across its files. Background flush and higher-level compaction are deferred to Phase 3.

All numeric fields are little-endian. Each data block contains sorted rows encoded as key length (32 bits), key, sequence (64 bits), type (8 bits), value length (32 bits), and value. A CRC32C follows the rows. The builder targets `block_bytes`, allowing one larger row in its own block. The Bloom section stores its probe count (8 bits) and bitmap, followed by a CRC32C. The index stores the last key of each block with 64-bit offset and size, followed by a CRC32C. A 48-byte footer stores 64-bit Bloom offset and size, index offset and size, and entry count, then a 32-bit format magic and CRC32C. Open validates the footer, Bloom filter, and index. Point reads use the index and filter before reading one data block. Data checksums are verified when blocks are read.

The binary manifest has a 32-bit format magic followed by 64-bit flushed sequence, next table number, active WAL start sequence, and table count. Each table stores its 64-bit id, 32-bit level, and length-prefixed smallest and largest keys. A CRC32C covers all preceding bytes. The active WAL filename is derived from its start sequence. A Phase 1 directory with no manifest continues to use `wal-1.log` until its first flush.

Flush installs the SSTable with file sync, rename, and directory sync. It then creates and syncs an empty next WAL before installing the manifest with the same atomic replacement protocol. Before the manifest switch, recovery follows the old manifest or legacy WAL and ignores uncommitted files. After the durable switch, recovery loads the new table and next WAL, even if the old WAL still exists. An error during manifest installation conservatively disables further writes until reopen. The old WAL is unlinked only after the manifest is durable. Unreferenced files from interrupted flushes can remain on disk.

The directory advisory lock stays held through WAL rotation. The existing per-WAL lock also remains. The writer mutex protects WAL I/O, sequences, and rotation accounting. The view mutex protects the visible memtable and table list. Writers take the writer mutex first, then the view mutex while publishing or flushing. Readers take the view mutex in shared mode and can proceed during ordinary WAL sync, but synchronous flush blocks reads. Scans hold a stable view, read the overlapping SSTable blocks, and heap-merge the sorted range results by key and sequence. The newest tombstone suppresses older values.

Options require a positive rotation budget, block targets up to 64 MiB, and 1 through 64 Bloom bits per key. SSTable index, Bloom, and manifest sections are capped at 256 MiB each.

## Phase 3 versions, maintenance, and group commit

### Durable WAL rotation

Manifest v2 adds a second 64-bit WAL start field after the original WAL start. A zero second start means only one WAL is live. With two live WALs, the first belongs to the immutable memtable and the second to the active memtable. The flushed sequence still equals the first WAL start minus one. The reader accepts existing v1 manifests and the manifest-free Phase 1 layout. The next manifest replacement upgrades the file to v2. WAL and SSTable bytes are unchanged.

A writer syncs the empty replacement WAL and commits the two-WAL manifest before accepting writes in the new generation. The worker writes and syncs the immutable memtable's SSTable while foreground writes continue. The worker then atomically replaces the manifest, advancing the flushed sequence and leaving the active WAL as the sole live WAL. It unlinks the immutable WAL after that replacement is durable. Recovery before rotation follows the original WAL. Recovery after rotation replays both WALs and resumes the pending flush. The immutable WAL must end exactly at the sequence before the new WAL starts, and only the active WAL may have a torn tail.

The database keeps at most one active and one immutable memtable. A writer with a full active memtable waits for the immutable slot to clear. `Flush()` waits for all writes published by the time it captures its target sequence. On clean shutdown the caller stops invoking the API, the DB flushes its active memtable, and the pool drains and joins. If maintenance or syncing fails, the DB rejects further writes and keeps its durable files available for recovery after reopening.

### Compaction and snapshots

After the L0 threshold is reached, maintenance merges every live SSTable into one L1 table. This bounds table count and lets the merge discard tombstones because no older table remains. An all-tombstone result has no output table. Compaction preserves any active or immutable memtable and its WAL. `CompactAll()` flushes its captured frontier and waits for a compaction pass. The worker prioritizes a pending immutable flush. Each DB serializes maintenance jobs through one pool task to keep file-number allocation and manifest installation ordered.

Compaction installs a new Version only after its manifest is durable. Readers retain their Version through SSTable I/O. Versions share table handles, so marking a table obsolete defers unlink until the last referencing version is released. A crash can leave unreferenced files on disk. Recovery loads only referenced files. Builds and merges use in-memory sorted rows.

### Locks and publication

The directory advisory lock excludes other database handles throughout rotation. The WAL also retains its advisory file lock while open. The writer mutex owns the request queue, sequence allocation, WAL I/O, manifest updates, maintenance state, and statistics. Its condition variable coordinates group leaders and followers, flush completion, compaction completion, and rotation backpressure. The view mutex protects Version publication and the active memtable's batch visibility. A memtable's skiplist shared mutex protects its nodes and height generator. The global order when locks are held together is writer, view, then skiplist. Maintenance releases the writer mutex before copying immutable rows or doing table build and merge I/O. Point reads obtain the active entry with the Version under the view lock and release it before checking immutable data or tables. Scans copy active rows at that same boundary.

The pool mutex protects its queue and stop flag. Workers release it before running tasks. The owner-only shutdown path uses a once flag and joins with no DB or pool mutex held. An atomic obsolete flag on each table handle is set only after a successful manifest install.

The queue uses FIFO leadership. `kAlways` serves one request per WAL sync. In `kBatch`, the leader waits up to 150 microseconds to gather requests and appends up to 32 records within the current rotation budget, allowing one oversized record. A single sync covers the group. On success the leader publishes the records in sequence order under the view lock before waking the callers. An append or sync failure fails queued callers and prevents further writes. A durable prefix can be replayed after an unacknowledged batch. The per-handle statistics count write syncs and compactions for testing and diagnostics.
