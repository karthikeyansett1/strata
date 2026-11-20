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

## Phase 4 TCP server

The Linux server uses one level-triggered epoll loop for accepting connections and performing nonblocking socket I/O. Worker threads execute database commands. The loop assigns a monotonic connection ID rather than identifying a connection by its reusable file descriptor. The completion queue has one mutex and wakes the loop through an eventfd. Only the loop touches client buffers and epoll interests. It dispatches one request per connection at a time, pauses reads while a response is pending, and resumes the buffered pipeline after writing the full response. This preserves request and response order through partial I/O. TCP_NODELAY avoids delayed small replies. Disconnects can leave a database command in flight. Its completion is discarded if the connection ID is no longer live.

The text protocol accepts LF and CRLF, space-delimited keys, and values with spaces or an empty value. It limits command lines to 64 KiB and encoded responses to 16 MiB. Binary content that cannot be represented in the protocol returns an error. The server caps live connections and limits acceptance when too many jobs remain in flight. An explicit stop or SIGINT/SIGTERM stops acceptance and drains jobs already submitted before the database closes. A client that receives `OK` has received a storage acknowledgement after WAL sync.

### Measurement procedure

The benchmark times direct C++ database calls with 16-byte keys and 100-byte values. The timed interval excludes seeding and clean shutdown. Random keys and operation streams are deterministic for a given thread count. Reads and mixed workloads preload up to 10,000 keys. Mixed workers alternate reads and writes with opposite starting phases. The matrix covers all four workloads, both sync modes, and 1, 2, 4, and 8 threads. The command defaults to 1,000,000 operations per case. The published matrix uses 20,000 per case in the two-vCPU Linux VM with database files on its local virtual disk.

The crash harness alternates sync modes across ephemeral loopback servers using one database. It waits for the client to append a key after reading `OK`, samples a seeded delay, and sends SIGKILL to the server. It verifies the previous cycle's acknowledged keys and their values after each restart and rechecks the complete acknowledgement set at the final restart. This exercises process crashes with the kernel and virtual disk still running.

### Random-read profile

On the ARM64 Ubuntu VM, `scripts/profile_reads.sh` sampled `cpu-clock:u` at 499 Hz with Linux perf. The byte-at-a-time CRC32C routine accounted for 78.68% of user-space samples. The implementation now uses ARMv8 CRC32C or x86 SSE4.2 when the CPU advertises support, with a software fallback. File formats and checksums are unchanged. An independent bitwise test covers unaligned inputs and partial words.

Three timed runs on each build used 300,000 random gets, one thread, and a freshly seeded SSTable. Median throughput increased from 81,254 to 275,511 ops/s (3.39×). The median of the p99 latencies fell from 14.625 to 4.958 µs. The accelerated checksum accounted for 14.18% of samples. Raw runs and profiles are in `results/read-profile-{before,after}.csv` and `results/perf-{before,after}.txt`.

To reproduce the baseline, build this phase with the `src/crc32c.cc` from `v0.3.0` in a separate checkout. Use `-DCMAKE_BUILD_TYPE=RelWithDebInfo` for both builds and run the same profiling script for each. On Colima the measurements used perf directly in the Linux VM. The script accepts the executable path in `STRATA_PERF` and can run perf with `STRATA_PERF_SUDO=1` while keeping the benchmark unprivileged.

## Phase 5 vector storage and search

A logical database keeps vectors in its `__strata_vectors` subdirectory, using the same WAL, manifest, and SSTable implementation as ordinary key-value data. Within that engine, vector keys are `v/` followed by the ID. This keeps preexisting binary keys and key-value scans independent of vector records. The value contains a 32-bit little-endian dimension followed by that many IEEE-754 float32 components in little-endian order. The dimension is inferred from records and is fixed once the store contains a vector. A store accepts dimensions from 1 to 4096 and finite components with absolute value at most 10¹⁵. The standalone C++ vector store expects the logical database directory to exist.

On open, the engine recovers the underlying key-value database and rebuilds HNSW from the sorted vector namespace. The graph is an in-memory derivative and is never part of recovery. A vector write persists the value before updating the graph. Inserts connect the new node in place. Overwriting an ID performs a full rebuild to avoid stale graph nodes. An update with an uncertain result invalidates the cache until reopen. The vector store's shared mutex permits concurrent searches and serializes persistence and index updates. It precedes the private database locks on the write path. Ordinary key-value operations use a separate database handle. The server shares one vector store among its workers.

The graph draws a geometric layer count with continuation probability 1/M. Insertion greedily descends upper layers, explores `ef_construction` candidates per insertion layer, selects diverse neighbors, and prunes reciprocal adjacency lists. Queries descend greedily, then search the base layer with a candidate budget of at least k. If the reachable graph cannot supply k candidates, an exact pass fills the result. The default configuration uses cosine distance, M=16, ef_construction=100, and ef_search=64. L2 search ranks by squared distance and returns Euclidean distance. Cosine search normalizes nonzero vectors and computes one minus their dot product. Cosine distance involving a zero vector is 1. Scalar, runtime-dispatched x86 AVX2, and ARM64 NEON kernels share the same interface.

### Vector evaluation

The evaluation used the first 100,000 AG News training titles and the first 200 test titles, with disjoint splits and 384-dimensional embeddings. It ran the model publisher's ARM64 int8 ONNX export of `sentence-transformers/all-MiniLM-L6-v2`, at revision `1110a243fdf4706b3f48f1d95db1a4f5529b4d41`. Titles were truncated to 24 tokens, mean-pooled with the attention mask, and normalized. The model and dataset are downloaded into an ignored cache. [Model exports](https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/tree/1110a243fdf4706b3f48f1d95db1a4f5529b4d41/onnx) · [AG News dataset paper](https://proceedings.neurips.cc/paper/5782-character-level-convolutional-networks-for-text-clasclas-sification.pdf)

Search ran single-threaded on the ARM64 Linux VM, timing in-memory C++ indexes through the same NumPy binding. Recall is mean overlap with exact top-10 labels, and speedup divides exact latency by graph latency. At ef_search=64, recall@10 was 97.45% with a 20.18× speedup. The result CSV includes all tested search budgets, mean latency, and graph-build time. The retrieval demo uses the same model family to select passages from a local corpus after reopening the database, and returns an extractive answer with source IDs.
