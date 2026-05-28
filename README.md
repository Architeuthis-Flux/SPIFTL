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

`make journaltest` builds and runs crash-recovery / durability tests
(`journaltest.cpp`) against the RAM flash emulator. `-DSPIFTL_RAM_STRICT`
makes the emulator model real NOR (erase -> `0xFF`, program can only clear
bits) so a "program over a non-erased page" bug is caught.






### Old system on JumperlOS

```
	 connect nodes

    10  -  15           connected


[17796 c0] FC> write entry path=/slots/slot0.yaml size=758
[17796 c0] FC> core_sync acquire @write
[17797 c0] FC> core_sync acquired @write
[17797 c0] FC> write ensureCapacity needed=758 currentCap=1024
[17797 c0] FC> write memcpy
[17797 c0] FC> core_sync release @write
[17798 c0] FC> write exit OK ver=2
[17901 c0] FC> core_sync acquire @flushService/snap
[17901 c0] FC> core_sync acquired @flushService/snap
[17901 c0] FC> core_sync release @flushService/snap
[18251 c0] FC> core_sync acquire @flushService/snap
[21068 c0] FC> core_sync release @flushService/snap
[21068 c0] FC> flushService -> canonical write /slots/slot0.yaml (758 bytes)
[21071 c0] FC> flushEntryChunked pause acquired in 0ms
[21071 c0] FC.atomic> ABA step 1: write canonical /slots/slot0.yaml (758 bytes)
[21078 c0] FC> flushEntryChunked /slots/slot0.yaml wt=1 ok=1 (canon=1 bak=0) elapsed=9ms (Core1 paused for 7ms)
[21080 c0] FC> flushService <- canonical write ok=1 (mirror will sync next tick)
[21081 c0] FC> core_sync acquire @flushService/mark
[21081 c0] FC> core_sync acquired @flushService/mark
[21081 c0] FC> core_sync release @flushService/mark
[21081 c0] FC> flushService DONE wrote /slots/slot0.yaml
[22122 c0] FC> flushService mirror-sync /slots/slot0.yaml flushed=2 mirrored=1
[22123 c0] FC> flushEntryMirror ENTER path=/slots/slot0.yaml flushed=2 mirrored=1
[22125 c0] FC> flushEntryChunked pause acquired in 0ms
[22126 c0] FC.atomic> ABA step 2: mirror to /.bak/slots/slot0.yaml (758 bytes)
[22187 c0] FC> flushEntryChunked /slots/slot0.yaml wt=2 ok=1 (canon=0 bak=1) elapsed=64ms (Core1 paused for 62ms)
[22188 c0] FC> flushEntryMirror EXIT path=/slots/slot0.yaml ok=1
[23176 c0] FC> flushService spiftl-meta-sync bursts=2 age=2095ms
[23176 c0] FC> metaSync ENTER reason=idle-tick bursts=2 age=2096ms
[23840 c0] FC> metaSync EXIT ok=1 drained=2 remaining=0 (Core1 paused for 662ms, total 662ms)
```


### With new SPIFTL stuff

```
	 connect nodes

     6  -  13           connected
    19  -  23           connected
[44978 c0] FC> write entry path=/slots/slot0.yaml size=1122
[44979 c0] FC> core_sync acquire @write
[44979 c0] FC> core_sync acquired @write
[44979 c0] FC> write ensureCapacity needed=1122 currentCap=2048
[44980 c0] FC> write memcpy
[44980 c0] FC> core_sync release @write
[44980 c0] FC> write exit OK ver=3
[45165 c0] FC> core_sync acquire @flushService/snap
[45166 c0] FC> core_sync acquired @flushService/snap
[45166 c0] FC> core_sync release @flushService/snap
[45518 c0] FC> core_sync acquire @flushService/snap
[45518 c0] FC> core_sync acquired @flushService/snap
[45518 c0] FC> core_sync release @flushService/snap
[45869 c0] FC> core_sync acquire @flushService/snap
[45869 c0] FC> core_sync acquired @flushService/snap
[45870 c0] FC> core_sync release @flushService/snap
[46221 c0] FC> core_sync acquire @flushService/snap
[46222 c0] FC> core_sync acquired @flushService/snap
[46222 c0] FC> core_sync release @flushService/snap
[46223 c0] FC> flushService -> canonical write /slots/slot0.yaml (1122 bytes)
[46226 c0] FC> flushEntryChunked pause acquired in 0ms
[46226 c0] FC.atomic> ABA step 1: write canonical /slots/slot0.yaml (1122 bytes)
[SPIFTL] persist 2344 us  journal-append (journal=1 epoch=13 recs=1)
[46296 c0] FC> flushEntryChunked /slots/slot0.yaml wt=1 ok=1 (canon=1 bak=0) elapsed=72ms (Core1 paused for 70ms)
[46296 c0] FC> flushService <- canonical write ok=1 (mirror will sync next tick)
[46297 c0] FC> core_sync acquire @flushService/mark
[46297 c0] FC> core_sync acquired @flushService/mark
[46298 c0] FC> core_sync release @flushService/mark
[46298 c0] FC> flushService DONE wrote /slots/slot0.yaml
[47276 c0] FC> flushService mirror-sync /slots/slot0.yaml flushed=3 mirrored=2
[47277 c0] FC> flushEntryMirror ENTER path=/slots/slot0.yaml flushed=3 mirrored=2
[47279 c0] FC> flushEntryChunked pause acquired in 0ms
[47279 c0] FC.atomic> ABA step 2: mirror to /.bak/slots/slot0.yaml (1122 bytes)
[SPIFTL] persist 2285 us  journal-append (journal=1 epoch=13 recs=2)
[47293 c0] FC> flushEntryChunked /slots/slot0.yaml wt=2 ok=1 (canon=0 bak=1) elapsed=16ms (Core1 paused for 14ms)
```

