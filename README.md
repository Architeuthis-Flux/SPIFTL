# SPIFTL - Embedded, Static Wear-Leveling FTL Library
(c) 2024 Earle F. Philhower, III <earlephilhower@yahoo.com>

This library implements a static wear-leveling FTL algorithm suitable for
use on embedded systems with SPI flash.  Using static wear leveling should
help extend the useful life of flash memory, especially when combined with
the FAT filesystem which has certain high-write usage areas such as the FAT
tables and directory entries.

There were three design goals:
* Preserve data
* Keep the flash array within a limited PE (program/erase) cycle range
* Miimize the memory at the cost of write speed

While the process used here is similar in concept to what a modern SSD does,
this is definitely not a general purpose SSD FTL layer.  It is missing things
like bad block handling, parallelism, short-circuit paths, data retention
scans and rewrite, and much more.  It is also limited to 16MB of flash and
erase pages of 4KB for memory and expediency considerations.

An implementation for the Arduino-Pico RP2040 core as well as a NBD
(Network Block Device) plugin is included.  Porting to other architectures
should only require developing a small FlashInterface subclass.

This software is provided on an AS-IS basis and no comes with no warranties.
See LICENSE.md for the full GNU LESSER GENERAL PUBLIC LICENSE.

## How it works (static wear leveling)

SPIFTL is a small **log-structured** flash translation layer. The flash is
divided into 4 KB **erase blocks** (EBs); each EB holds eight 512-byte logical
blocks (LBAs). The core data structures (all in RAM, serialized to flash as
"metadata"):

* **L2P map** - `l2p[lba]` -> `(eb, index, valid)` packed into a `uint16`. This
  is the logical-to-physical mapping.
* **`peCount[eb]`** - the program/erase count of each block (the wear counter).
  It is an 8-bit value; when any block reaches 251 the FTL subtracts
  `maxPEDiff` (64) from every counter and adds it to a global `peCountOffset`,
  so the true erase count is `peCountOffset + peCount[eb]` and the byte never
  overflows.
* **`ebState[eb]`** - a nibble per block: `0` = free, `1..8` = number of valid
  LBAs it holds, `0xf` = metadata block, `0xe` = journal block.

**Writes are out-of-place.** A write never erases in place; it appends the new
sector to the current *open* EB at the next free slot, updates `l2p[lba]` to
point there, and marks the old slot invalid. When the open EB fills (8 slots)
a new one is opened.

**Garbage collection + wear leveling.** A new open EB is chosen by
`selectBestEB()`, which keeps at least 3 free blocks available by running
`garbageCollect()` and always writes into the **lowest-PE** (least-worn) free
block. GC picks its victim with `gcScore()`, which blends two goals:

* *Reclaim*: blocks with fewer valid LBAs are cheaper to evacuate (`8 - state`).
* *Static wear leveling*: a block whose erase count lags the most-worn block by
  `>= maxPEDiff` scores highest, so **cold/static data is forcibly relocated**
  out of an under-worn block and that block is put back into rotation. Without
  this, blocks holding data that never changes would never wear while hot
  blocks burn out. This is what makes the leveling *static*, not just dynamic.

The result is a hard bound: across the whole device, `max(peCount) -
min(peCount) <= maxPEDiff` (64). Measured over 1,000,000 random writes to a
256 KB image (64 EBs), every block lands within a **~54-count spread around
~14,000 erases** - i.e. wear is spread essentially perfectly evenly (see
`staticwearleveltest.cpp`).

**Metadata** (L2P + peCount + ebState) is itself written to a rotating set of
metadata EBs, epoch-tagged and CRC32-protected, double-buffered so a torn write
always leaves a previous good copy. Those metadata blocks age out and relocate
via the same wear-leveling machinery (`metaAgeRewrite()`), so the metadata
region is not a wear hot spot either.

## Lazy Persist Mode (Architeuthis-Flux fork)

This fork adds an opt-in **lazy persist** mode that decouples metadata
serialization from FatFS' `disk_ioctl(CTRL_SYNC)` (which fires on every
`f_close`). The motivation: on a 4 MB FatFS partition, `persist()` takes
~750 ms because the L2P map plus per-EB bookkeeping has to be rewritten
into 4-13 metadata erase blocks at ~120 ms each. Doing this on every
file close is unacceptable for embedded use cases that save small files
frequently (e.g. JumperlOS' breadboard slot-state snapshots).

### API

```cpp
SPIFTL ftl(_fi);
ftl.setLazyPersist(true);   // opt in (default off, upstream behavior)
// ... do many writes ...
ftl.forceSync();            // explicit "I really mean it" persist
```

* `setLazyPersist(bool)` - flip lazy mode on/off. Default off.
* `isLazyPersist()` - introspection.
* `forceSync()` - persist regardless of lazy mode. Always safe to call.

When lazy mode is on:
* `persist()` (which FatFS calls for every CTRL_SYNC) becomes a no-op.
* `write()` still programs the data sector to flash immediately, so
  reads in the same boot session see the new content.
* The L2P / peCount / ebState **metadata** is NOT serialized until the
  embedder calls `forceSync()` at a coarse-grained safe point (idle
  window, slot switch, pre-USB-MSC mount, shutdown, etc).

### Power-loss semantics

Without `forceSync()`, the on-flash metadata is whatever was last
persisted. On reboot, SPIFTL reverts to that L2P map, which means:

* Sectors written between the last persist and the power cut are still
  physically on flash but **unreachable** through the L2P, so files
  appear at the previous version.
* This is no worse than upstream behavior on a power loss between
  `f_write()` and `f_close()` - just a wider window. Pair with an
  application-level mirror (e.g. an A/B file pair) if the wider loss
  window is unacceptable.
* The on-flash metadata is never inconsistent - either a previous epoch
  is loaded, or the new one is. CRC mismatches drop back to the older
  good copy.

### Memory cost

One byte (`bool _lazyPersist`). No additional buffers, no flash usage.

## Delta-Journal Mode (Architeuthis-Flux fork)

The lazy-persist mode above makes the freeze rare; **delta-journal mode**
makes the freeze itself cheap, so you don't have to defer at all. Instead
of rewriting the entire ~16 KB metadata snapshot on every `persist()`, the
journal appends only the L2P / `peCount` entries that *changed* since the
previous record to an already-erased flash page. A program-only append
needs **no erase**, so a persist drops from ~750 ms to roughly one page
program (sub-millisecond on a W25Q128JV: ~0.4 ms typ / 3 ms max, vs ~45 ms
for a single 4 KB sector erase). The result: every `f_close` is both fast
**and** immediately power-loss durable.

### No reformat, ever (identical geometry)

Crucially the on-flash **geometry is identical** whether journaling is on or
off: `flashLBAs` is the upstream value, and the journal ring (2 erase blocks)
is carved opportunistically out of the normal free-block pool at runtime, not
reserved up front. So a journal-on build mounts an existing journal-off volume
(and vice versa) with **no reformat and no data loss** - the `FTLInfo` matches,
the snapshot loads, and a ring is established on first persist if there's room.

Because the ring shares the free pool with GC and metadata rotation, the FTL
**suspends** the journal (folds its deltas into a full snapshot and hands the 2
blocks back) when free blocks drop near GC's 3-block reserve, and **resumes**
once free blocks recover (hysteresis avoids thrashing). So when the volume is
nearly full, saves transparently fall back to the upstream full-snapshot path;
with normal free space they ride the fast append path.

### API

```cpp
SPIFTL ftl(_fi, /*journal=*/true);  // allocate dirty-tracking bitsets + use append
// ... do writes; persist()/forceSync() now append a delta page ...
```

* `SPIFTL(fi, journal)` - `journal` defaults to `false` (exact upstream
  behavior). When true it allocates the dirty-tracking bitsets and uses the
  append fast-path. It does NOT change geometry or reserve blocks, so it's
  safe to flip between builds without reformatting.
* `setJournal(bool)` - toggle at runtime. Enabling only works if the
  instance was constructed with journaling reserved (bitsets allocated);
  disabling reverts to full snapshots.
* `isJournal()` - introspection.
* `persist()` / `forceSync()` automatically use the journal when enabled.

### How it works

* A ring of `journalEBs` erase blocks (marked `ebState == 0x0e`) holds
  append-only records. Each record is one page (`writeBufferSize` bytes,
  256 B on RP2040) containing a `"JRNL"` signature, the base snapshot
  epoch, a monotonic sequence number, the changed `{lba, l2p}` and
  `{eb, peCount}` entries, and a CRC32.
* On `persist()`: program one page, done. A **full snapshot** (the old
  ~750 ms path) is taken only when the ring fills, when a single delta is
  too large to fit in one page, when a change a delta can't capture occurs
  (a `peCount` rebase, or a metadata-EB relocation), or when the volume is
  near-full (ring suspended). After a snapshot the ring is re-allocated (if
  there's room) and the sequence resets.
* `ebState` is **not** journaled: its per-EB valid counts are recomputed
  from the L2P map on boot, keeping records tiny.
* On boot: load the highest-epoch snapshot (unchanged), then replay
  journal pages in sequence order, stopping at the first blank or
  CRC-failed page (a torn final write is simply dropped).

### Power-loss semantics

Durable up to the **last completed `persist()`** (e.g. every `f_close`),
not just the last coarse-grained `forceSync()`. A power cut during a page
program leaves that page failing CRC, so replay stops before it: the
torn save is lost but every prior save is intact, and the on-flash state
is never inconsistent. This is strictly stronger than lazy mode while also
being far faster.

Note: under pathological *full-device* random churn, garbage collection
and wear-level rewrites fire constantly and force frequent snapshots, so
the journal's advantage shrinks toward the snapshot cost. The target
workload - small files saved frequently with ample free space (e.g.
JumperlOS slot snapshots) - stays on the fast append path.

### Memory cost

Two dirty-tracking bitsets sized to the partition: `flashLBAs` bits +
`eraseBlocks` bits (~1 KB on a 4 MB partition, ~4 KB at the 16 MB max),
allocated only when journaling is enabled. The 2 ring blocks are borrowed
from the normal free pool (not reserved), so they don't reduce usable
capacity. Zero cost - geometry or memory - when journaling is off.

### Tests

`make journaltest` builds and runs the crash-recovery / durability / wear
suite (`journaltest.cpp`) against the RAM flash emulator. `-DSPIFTL_RAM_STRICT`
makes the emulator model real NOR (erase -> `0xFF`, program can only clear
bits) so a "program over a non-erased page" bug is caught. Scenarios:

* A - basic durability across a reboot (write, persist, reboot, verify).
* B - overwrites + trims survive a reboot.
* C - a write with no following `persist()` is correctly lost on power cut.
* D - a torn final journal page (bad CRC) reverts to the last good persist.
* E - snapshot rollover when the ring fills.
* G - fill to capacity (journal suspends, GC keeps working), then free + resume.
* H - static wear leveling holds with journaling on, vs. erase count off.
* F - 100k random write/trim ops with GC + journal, verified across a reboot.

`make statictest` runs the upstream 1,000,000-write wear-leveling soak.

## Measured results

Persist latency on a real **W25Q128JV** (RP2350, 4 MB FatFS partition), saving
a ~1 KB breadboard slot file (the JumperlOS workload):

| metric                         | full snapshot (old) | delta-journal (new) |
|--------------------------------|---------------------|---------------------|
| metadata `persist()` per save  | ~750 ms             | **~2.3 ms**         |
| flash erases per `persist()`   | ~5 metadata EBs     | **0** (page append) |
| power-loss durable per f_close | only via deferral   | **yes, every save** |
| reformat to enable             | n/a                 | **none**            |

That ~2.3 ms is the actual on-device number (`[SPIFTL] persist 2293 us
journal-append`); the metadata persist went from dominating a save to noise.
(The remaining ~15-80 ms of a slot save is the FatFS data write itself -
sector programs + dir/FAT update - which the journal does not change; embedders
that save on every UI event should still defer/coalesce that at the app layer.)

Endurance + wear, from scenario H (4 MB image, 20k write+persist cycles, 1/4
static + 3/4 hot data):

| metric                    | journal off (snapshot/persist) | journal on |
|---------------------------|--------------------------------|------------|
| total flash erases        | 268,403                        | **175,663** (1.5x fewer) |
| PE-count spread (max-min) | 59                             | 59 (`maxPEDiff` = 64) |

Both modes hold the static-wear-leveling bound; journaling does **1.5x fewer
total erases** for this write-heavy stressor (the metadata-erase component
specifically drops ~50x - it's the data-sector writes, unchanged by the
journal, that dominate the remainder). The journal ring rotates through the
free pool and is not a wear hot spot.


