/*
    SPIFTL.h - Embedded, Static Wear-Leveling FTL Library

    Copyright (c) 2024 Earle F. Philhower, III <earlephilhower@yahoo.com>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 3 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this program. If not, see https://www.gnu.org/licenses/
*/

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <bitset>
#include <string.h>
#include <cassert>
#include <vector>
#include <list>
#include <map>

#include "FlashInterface.h"

#ifndef FTL_DEBUG
#define FTL_DEBUG 0
#endif


class SPIFTL {
public:
    // `journal` opts in to the append-only delta-journal (see "DELTA JOURNAL"
    // section below). Crucially the on-flash GEOMETRY is identical whether or
    // not journaling is on: flashLBAs is the upstream value, and the journal
    // ring is carved opportunistically out of the normal free-block pool at
    // runtime (and released back under near-full pressure). That means a
    // journal-on build mounts an existing journal-off volume - and vice versa -
    // with NO reformat and no data loss. `journal` only controls whether the
    // dirty-tracking bitsets are allocated and the append fast-path is used;
    // it costs nothing (geometry or memory) when off.
    SPIFTL(FlashInterface *fi, bool journal = false) : _fi(fi), _journal(journal) {
        flashBytes = fi->size();
        assert(flashBytes <= 16 * 1024 * 1024); // We assume 16MB or less flash space with certain bitfields
        eraseBlocks = flashBytes / ebBytes;
        int theoreticalLBAs = eraseBlocks * ebBytes / lbaBytes;
        metaEBBytes = /* peCount */ eraseBlocks + /* ebState */ (eraseBlocks + 1) / 2 + /* l2p */ (theoreticalLBAs * 2) + /* peCountOffset */ 4;
        metaEBs = 2 * (1 + metaEBBytes / (ebBytes - 64 /* header/footer/checksums */));
        flashLBAs = (eraseBlocks - 3 /* required for GC */ - metaEBs) * (ebBytes / lbaBytes);
        flashWriteBufferSize = fi->writeBufferSize();
        peCount = new uint8_t[eraseBlocks];
        ebState = new uint8_t[(eraseBlocks + 1) / 2];
        metaEBList = new int16_t[metaEBs];
        l2p = new L2P[flashLBAs];
        metadataEBList.reserve(metaEBs); // Guarantee it can fit the list and avoid any memory allocations during FTL persistence
        for (int i = 0; i < journalEBs; i++) {
            journalEBList[i] = -1;
        }
        if (_journal) {
            dirtyL2P = new uint8_t[(flashLBAs + 7) / 8];
            dirtyPe = new uint8_t[(eraseBlocks + 7) / 8];
            bzero(dirtyL2P, (flashLBAs + 7) / 8);
            bzero(dirtyPe, (eraseBlocks + 7) / 8);
        }
    };

    ~SPIFTL() {
        delete[] dirtyPe;
        delete[] dirtyL2P;
        delete[] l2p;
        delete[] metaEBList;
        delete[] ebState;
        delete[] peCount;
    }

    inline int lbaCount() {
        return flashLBAs;
    }

    inline int ebCount() {
        return eraseBlocks;
    }

    inline int getPECountOffset() {
        return peCountOffset;
    }

    inline uint8_t getPECount(int eb) {
        return peCount[eb];
    }

    // Introspection (mainly for tests / diagnostics).
    inline uint32_t debugEpoch() const { return metadataEpoch; }
    inline int debugJournalRecords() const { return (int)journalSeq; }
    inline int debugJournalEB(int i) const { return (i >= 0 && i < journalEBs) ? journalEBList[i] : -1; }
    inline bool debugJournalSuspended() const { return journalSuspended; }
    inline int debugEmptyEBs() const { return emptyEBs; }

    bool format() {
#if FTL_DEBUG
        printf("formatting FTL\n");
#endif
        bzero(l2p, sizeof(L2P) * flashLBAs);
        bzero(peCount, sizeof(uint8_t) * eraseBlocks);
        bzero(ebState, sizeof(uint8_t) * ((eraseBlocks + 1) / 2));
        peCountOffset = 0;
        highestPECount = 0;
        emptyEBs = eraseBlocks;
        for (int i = 0; i < metaEBs; i++) {
            emptyEBs--;
            setEBMeta(i);
            metaEBList[i] = i;
        }
        metadataAge = 0;
        if (_journal) {
            // The ring + base snapshot are established lazily by the first
            // persist() (which routes to doFullSnapshot while journalBaseValid
            // is false), so a freshly formatted FTL has no journal markers yet.
            for (int i = 0; i < journalEBs; i++) journalEBList[i] = -1;
            journalSeq = 0;
            journalBaseValid = false;
            journalNeedsSnapshot = false;
            journalSuspended = false;
            clearDirty();
        }
        // Blow away anything that looks like old metadata!
        for (int i = 0; i < eraseBlocks; i++) {
            const uint8_t *eb = _fi->readEB(i);
            if (!memcmp(eb, metadataSig, 8)) {
#if FTL_DEBUG
                printf("format erasing eb %d\n", i);
#endif
                _fi->eraseBlock(i);
            }
        }
        return true;
    }

    bool check() {
        int max = 0;
        int min = 65536;
        int c = 0;
        int metas = 0;
        bool ret = true;
        for (int i = 0; i < eraseBlocks; i++) {
            c += !getEBState(i) ? 1 : 0;
            if (peCount[i] > max) {
                max = peCount[i];
            }
            if (min > peCount[i]) {
                min = peCount[i];
            }
            if (ebIsMeta(i)) {
                metas++;
            }
        }
        if (metas > metaEBs) {
            printf("ERROR: metas > metaEBs  %d > %d\n", metas, metaEBs);
            ret = false;
        }
        if (c != emptyEBs) {
            printf("ERROR: emptyEBs mismatch %d != %d\n", c, emptyEBs);
            ret = false;
        }
        if (max != highestPECount) {
            printf("ERROR: highestPECount mismatch %d != %d\n", max, highestPECount);
            ret = false;
        }
        if (max - min > maxPEDiff + 1) {
            printf("ERROR: maxPEDiff mismatch %d - %d    %d != %d\n", max, min, max - min, maxPEDiff);
            ret = false;
        }
        uint8_t val[eraseBlocks];
        bzero(val, sizeof(val));
        for (int i = 0; i < flashLBAs; i++) {
            if (l2p_val(i)) {
                auto eb = l2p_eb(i);
                auto idx = l2p_idx(i);
                if (ebIsMeta(eb)) {
                    printf("ERROR: LBA %d points to metadata\n", i);
                    ret = false;
                }
                if (val[eb] & 1 << idx) {
                    printf("ERROR: LBA %d crosslinked in eb %d idx %d\n", i, eb, idx);
                    ret = false;
                }
                val[eb] |= 1 << idx;
            }
        }
        return ret;
    }


    bool start() {
        _fi->deserialize();
        populateMetadataMap();
        if (loadHighestEpochMetadata()) {
#if FTL_DEBUG
            printf("restored metadata from flash\n");
#endif
            metadataAge = 0;
            if (_journal) {
                clearDirty();
                journalSuspended = false;
                int found = scanJournalList(); // locate the ring from ebState markers
                if (found > 0) {
                    replayJournal();           // apply delta records up to the torn tail
                    recomputeDerivedState();   // rebuild ebState valid counts from L2P
                    journalBaseValid = true;
                    journalNeedsSnapshot = false;
#if FTL_DEBUG
                    printf("journal replayed %d records from %d ring EBs\n", (int)journalSeq, found);
#endif
                } else {
                    // Volume predates journaling (an existing journal-off FS) or
                    // the ring was released. Establish a ring now if there's
                    // room; doFullSnapshot() leaves us suspended (full-snapshot
                    // mode) if the volume is too full. Either way: no reformat,
                    // existing data is preserved (geometry is identical).
                    journalBaseValid = false;
                    doFullSnapshot();
                }
            }
            return true;
        } else {
            return format();
        }
    }

    bool persist() {
        // Lazy mode: data bytes are already on flash via SPIFTL::write() (the
        // _fi->program() call inside that function). What we are skipping
        // here is only the L2P / peCount / ebState metadata serialization
        // into the metadata erase blocks - that is what dominates the
        // ~750 ms freeze on a 4 MB partition. The embedder is expected to
        // call forceSync() during a real idle window (or before USB MSC
        // mount, slot switch, shutdown) to coalesce many lazy writes into
        // one persist.
        //
        // Power-loss safety: on reboot we revert to the most recently
        // persisted L2P. Bytes written between the last forceSync() and
        // the power cut are still physically on flash but unreachable
        // through the L2P, so they appear as if the writes never happened.
        // The embedder is responsible for any per-file mirror / boot-
        // recovery scheme it wants on top.
        if (_lazyPersist) {
            return true;
        }
        // Journal mode: append a small delta record instead of rewriting the
        // whole metadata snapshot. This is the fast path (one already-erased
        // page program, no erase) that makes every f_close cheap AND durable.
        if (_journal) {
            return journalPersist();
        }
        bool ret = doPersist();
        _fi->serialize();
        return ret;
    }

    bool persistIfDirty() {
        if (_journal) {
            // Persist if anything changed, a snapshot is pending, or no base
            // exists yet (and we're not deliberately suspended - a suspended
            // FTL's last snapshot is already durable).
            if (dirtyL2Pcount || dirtyPecount || journalNeedsSnapshot ||
                (!journalBaseValid && !journalSuspended)) {
                return persist();
            }
            return false;
        }
        if (metadataAge) {
            return persist();
        }
        return false;
    }

    // Always persist regardless of lazy mode. This is the explicit
    // "I really mean it" sync that the embedder calls during idle / slot
    // switch / pre-USB-MSC-mount / shutdown. Returns true if metadata is
    // (now) coherent on flash, false if the underlying flush failed.
    bool forceSync() {
        if (_journal) {
            if (!dirtyL2Pcount && !dirtyPecount && !journalNeedsSnapshot &&
                (journalBaseValid || journalSuspended)) {
                // Nothing changed since the last persist and metadata is
                // already durable (ring base, or last full snapshot). No-op.
                return true;
            }
            return journalPersist();
        }
        if (!metadataAge) {
            // Nothing has been written since the last persist; the on-flash
            // metadata is still authoritative. No-op success.
            return true;
        }
        bool ret = doPersist();
        _fi->serialize();
        return ret;
    }

    // Opt-in lazy persist. When enabled, persist() (which FatFS calls on
    // every CTRL_SYNC, i.e. every f_close) becomes a no-op and the
    // embedder is responsible for invoking forceSync() at coarse-grained
    // safe points. Default off, so existing consumers see no behavior
    // change. Memory cost: 1 byte.
    void setLazyPersist(bool enable) { _lazyPersist = enable; }
    bool isLazyPersist() const { return _lazyPersist; }

    // Opt-in delta-journal. Can only be ENABLED if the instance was
    // constructed with journaling on (which allocates the dirty-tracking
    // bitsets); calling setJournal(true) on an instance built with journal=
    // false is ignored, since there are no bitsets to track deltas. Disabling
    // is always allowed and reverts persist()/forceSync() to full snapshots.
    // Note: geometry is identical either way, so toggling never reformats.
    void setJournal(bool enable) {
        if (enable && !dirtyL2P) {
            return; // Not reserved at construction; cannot enable.
        }
        _journal = enable;
    }
    bool isJournal() const { return _journal; }

    bool write(int lba, const uint8_t *data) {
        if ((lba < 0) || (lba >= flashLBAs)) {
            return false ;
        }
        if (openEB < 0) {
            openEB = selectBestEB();
        }
#if FTL_DEBUG
        printf("wrote %d to eb %d idx %d\n", lba, openEB, openEBNextIndex);
#endif
        if (!l2p_val(lba)) {
            validLBAs++;
        }

        _fi->program(openEB, openEBNextIndex * lbaBytes, data, lbaBytes);
        int oldEB, oldIndex;
        if (findLBA(lba, &oldEB, &oldIndex)) {
            clearLBAValid(oldEB);
            if (!getEBState(oldEB) && (oldEB != openEB)) {
                emptyEBs++;
            }
        }
        setLBAValid(openEB);
        setLBA(lba, openEB, openEBNextIndex);
        markL2P(lba);
        openEBNextIndex++;
        if (openEBNextIndex >= ebBytes / lbaBytes) {
            openEB = -1;
            openEBNextIndex = 0;
        }
        ageMetadata();
        return true;
    }

    bool read(int lba, uint8_t *dest) {
        if ((lba < 0) || (lba >= flashLBAs)) {
            return false;
        }
        int oldEB, oldIndex;
        if (findLBA(lba, &oldEB, &oldIndex)) {
            _fi->read(oldEB, oldIndex * lbaBytes, dest, lbaBytes);
        } else {
            bzero(dest, lbaBytes);
        }
        return true;
    }

    bool trim(int lba) {
        if ((lba < 0) || (lba >= flashLBAs)) {
            return false;
        }
        if (l2p_val(lba)) {
#if FTL_DEBUG
            printf("trim lba %d eb %d idx %d\n", lba, l2p_eb(lba), l2p_idx(lba));
#endif
            clearLBAValid(l2p_eb(lba));
            validLBAs--;
            if (!getEBState(l2p_eb(lba)) && (l2p_eb(lba) != openEB)) {
                emptyEBs++;
#if FTL_DEBUG
                printf("freeing eb %d\n", l2p_eb(lba));
#endif
            }
            l2p[lba] = 0; // invalid
            markL2P(lba);
            ageMetadata();
        }
        return true;
    }

    void dump() {
#if FTL_DEBUG
        printf("Erase Blocks (maxpe=%d, peCountOffset=%d, emptyEBs=%d, validLBAs=%d)\n", highestPECount, peCountOffset, emptyEBs, validLBAs);
        printf("MetaEBList: ");
        for (int i = 0; i < metaEBs; i++) {
            printf("%d ", metaEBList[i]);
        }
        printf("\n");
        for (int i = 0; i < eraseBlocks; i++) {
            printf("  EB%02d: pe=%d ebState=%01X meta=%d gcscore=%d\n", i, peCount[i], getEBState(i), ebIsMeta(i) ? 1 : 0, gcScore(i));
        }
#endif
    }

    const int ebBytes = 4096;
    const int lbaBytes = 512;
    const int maxPEDiff = 64;

private:
    FlashInterface *_fi;

    int flashBytes;
    int eraseBlocks;
    int metaEBBytes;
    int metaEBs;
    int flashLBAs;
    int flashWriteBufferSize;

    typedef struct {
        uint16_t ebBytes;
        uint16_t lbaBytes;
        uint32_t flashBytes;
        uint16_t metaEBBytes;
        uint16_t flashLBAs;
    } FTLInfo;

    uint8_t *peCount; // We'll just track up to 250, and when we hit 251 we will subtract maxPEDiff from them all
    // ebState: 0 = free, 1...8 = # of LBAs valid, 9..0xd = undefined,
    //          0xe = delta-journal block, 0xf = meta
    const unsigned int ebMeta = 0x0f;
    const unsigned int ebJournal = 0x0e;
    uint8_t *ebState;
    int16_t *metaEBList;

    unsigned int peCountOffset;
    int highestPECount;
    int emptyEBs;
    int validLBAs;
    uint8_t metadataAge;

    // L2P format.  Can't use bitfields since GCC will make every element 32-bits
    //typedef struct {
    //    unsigned eb  : 12;
    //    unsigned idx : 3;
    //    unsigned val : 1;
    //} L2P;
    typedef uint16_t L2P;
    L2P *l2p;

    int openEB = -1; // EB currently being written.  < 0 == none open
    int openEBNextIndex = 0; // Which LBA w/in that EBA should be written next

    // When true, persist() is a no-op and the embedder must call
    // forceSync() at coarse-grained safe points to actually serialize
    // metadata to flash. See setLazyPersist() / forceSync() above.
    bool _lazyPersist = false;

    // ---- DELTA JOURNAL STATE (only used when _journal is true)
    //
    // The journal is an append-only ring of `journalEBs` erase blocks. Each
    // persist() programs exactly one already-erased page (flashWriteBufferSize
    // bytes) holding the L2P / peCount entries that changed since the previous
    // record, plus a CRC. No erase is performed on the hot path, so a persist
    // is ~one page-program (sub-millisecond) instead of the full ~16 KB
    // snapshot rewrite (~750 ms). A full snapshot is taken (and the journal
    // ring reset) only when the ring fills, when a single delta is too large
    // to fit in one page, or when bookkeeping that a delta can't capture moves
    // (peCount rebase, metadata EB relocation).
    bool _journal;                      // journaling enabled (dirty bitsets allocated, append used)
    static const int journalEBs = 2;    // erase blocks used by the ring (carved from the free pool)
    int16_t journalEBList[journalEBs];  // physical EBs of the ring, ascending; -1 if unused
    uint32_t journalSeq = 0;            // next record/page number within this snapshot cycle
    bool journalBaseValid = false;      // a base snapshot + ring exist on flash
    bool journalNeedsSnapshot = false;  // a non-delta-able change happened; snapshot next persist
    bool journalSuspended = false;      // ring released under near-full pressure; full-snapshot mode
    uint8_t *dirtyL2P = nullptr;        // bitset[flashLBAs]: L2P entries changed since last record
    uint8_t *dirtyPe = nullptr;         // bitset[eraseBlocks]: peCount entries changed since last record
    int dirtyL2Pcount = 0;
    int dirtyPecount = 0;
    const char journalSig[4] = {'J', 'R', 'N', 'L'};

    // The ring is carved from the same free-block pool that GC and metadata
    // rotation draw on, so when the volume gets near-full we must hand those
    // blocks back or selectBestEB()'s "keep >=3 empty" loop could never reach
    // its target (the journal would be hogging blocks GC needs). We therefore
    // suspend (release the ring, fall back to full-snapshot persist) when free
    // blocks drop to journalSuspendBelow, and only re-establish the ring once
    // free blocks recover past journalResumeAbove. The hysteresis gap avoids
    // thrashing right at the boundary. These are erase-block counts.
    static const int journalSuspendBelow = journalEBs + 3; // <= this many free -> release ring
    static const int journalResumeAbove  = journalEBs + 8; // >= this many free -> rebuild ring

    inline void markL2P(int lba) {
        if (!_journal || !dirtyL2P) return;
        if (!(dirtyL2P[lba >> 3] & (1 << (lba & 7)))) {
            dirtyL2P[lba >> 3] |= (1 << (lba & 7));
            dirtyL2Pcount++;
        }
    }
    inline void markPe(int eb) {
        if (!_journal || !dirtyPe) return;
        if (!(dirtyPe[eb >> 3] & (1 << (eb & 7)))) {
            dirtyPe[eb >> 3] |= (1 << (eb & 7));
            dirtyPecount++;
        }
    }
    inline bool testL2P(int lba) { return dirtyL2P[lba >> 3] & (1 << (lba & 7)); }
    inline bool testPe(int eb) { return dirtyPe[eb >> 3] & (1 << (eb & 7)); }
    inline void clearDirty() {
        if (!dirtyL2P) return;
        bzero(dirtyL2P, (flashLBAs + 7) / 8);
        bzero(dirtyPe, (eraseBlocks + 7) / 8);
        dirtyL2Pcount = 0;
        dirtyPecount = 0;
    }
    inline int pagesPerEB() { return ebBytes / flashWriteBufferSize; }
    inline int journalCapacityPages() { return journalEBs * pagesPerEB(); }

    // ---- L2P AND ERASE BLOCK MANAGEMENT

    inline void setEBState(int eb, unsigned int state) {
        int idx = eb / 2;
        if (eb & 1) {
            ebState[idx] = (ebState[idx] & 0x0f) | (state << 4);
        } else {
            ebState[idx] = (ebState[idx] & 0xf0) | state;
        }
    }

    inline unsigned int getEBState(int eb) {
        return 0x0f & (ebState[eb / 2] >> ((eb & 1) ? 4 : 0));
    }

    inline bool ebIsMeta(int eb) {
        return getEBState(eb) == ebMeta;
    }

    inline void setEBMeta(int eb) {
        setEBState(eb, ebMeta);
    }

    inline bool ebIsJournal(int eb) {
        return getEBState(eb) == ebJournal;
    }

    inline uint16_t l2p_eb(int lba) {
        return l2p[lba] & ((1 << 12) - 1);
    }

    inline uint8_t l2p_idx(int lba) {
        return (l2p[lba] >> 12) & ((1 << 3) - 1);
    }

    inline bool l2p_val(int lba) {
        return l2p[lba] & 1 << 15;
    }

    inline L2P make_l2p(int idx, int eb) {
        L2P t = 1 << 15;
        t |= idx << 12;
        t |= eb;
        return t;
    }

    inline void setLBAValid(int eb) {
        setEBState(eb, getEBState(eb) + 1);
    }

    inline void clearLBAValid(int eb) {
        setEBState(eb, getEBState(eb) - 1);
    }

    bool findLBA(int lba, int *eb, int *idx) {
        if (l2p_val(lba)) {
            *eb = l2p_eb(lba);
            *idx = l2p_idx(lba);
            return true;
        } else {
            return false;
        }
    }

    inline void setLBA(int lba, int eb, int idx) {
        l2p[lba] = make_l2p(idx, eb);
    }


    // ---- METADATA FORMAT AND PERSISTENCE

    // Metadata EB format
    // 8 byte header:   <signature0..7>
    // 3 byte epoch:    <e><e><e> = 2^23 cycles, way beyond flash lifetime
    // 1 byte index:    <i> = Block within this metadata serialization, since more than one EB needed
    // 4080 byte:       <d>...<d> = packed metadata
    // 4 byte checksum: <c><c><c><c> = CRC32 over bytes 0...4091

    // Metadata packed format
    // ftlInfo:peCountArray:l2pArray:peCountOffset:highestPECount:emptyEBs:validLBAs

    class MetadataCRC32 {
    public:
        MetadataCRC32() {
            crc = 0xffffffff;
        }

        ~MetadataCRC32() {
        }

        inline void add(uint8_t x) {
            add(&x, 1);
        }

        void add(const void *d, uint32_t len) {
            const uint8_t *data = (const uint8_t *)d;
            for (uint32_t i = 0; i < len; i++) {
                crc ^= data[i];
                for (int j = 0; j < 8; j++) {
                    if (crc & 1) {
                        crc = (crc >> 1) ^ 0xedb88320;
                    } else {
                        crc >>= 1;
                    }
                }
            }
        }

        uint32_t get() {
            return ~crc;
        }

        void reset() {
            crc = 0xffffffff;
        }

    private:
        uint32_t crc;
    };

    const char metadataSig[8] = {'S', 'P', 'I', 'F', 'T', 'L', '0', '1'};
    std::vector<uint16_t> metadataEBList;
    int metadataEBoffset;
    uint8_t metadataEBindex;
    MetadataCRC32 metadataCRC;
    uint32_t metadataEpoch = 2; // epoch 0 and 1 are part of formatting on flash, all empty

    void openMetadataStreamForWrite() {
#if FTL_DEBUG
        printf("Serializing metadata epoch %d\n", (int)metadataEpoch + 1);
#endif
        metadataEBList.clear();
        for (int j = 0; j < metaEBs; j++) {
            int i = metaEBList[j];
            if (i < 0) {
                continue;
            }
            const uint8_t *eb = _fi->readEB(i);
            metadataCRC.reset();
            metadataCRC.add(eb, 4096 - 4);
            uint32_t crc = metadataCRC.get();
            bool err = memcmp(&crc, eb + 4096 - 4, 4);
            uint32_t mde = *(uint32_t*)(_fi->readEB(i) + 8) >> 8;
#if FTL_DEBUG
            printf("metaEBList[%d] = %d, epoch %d, err %d\n", j, i, (int)mde, err);
#endif
            if (err || (mde < metadataEpoch)) {
                if (!err) {
                    // Need to erase the MD in this block or we can end up with a large number of old MD blocks, wasting time and memory during FTL bringup
                    _fi->eraseBlock(i);
                }
                setEBState(i, 0);
                metaEBList[j] = -1;
                emptyEBs++;
#if FTL_DEBUG
                printf("Free %d\n", i);
#endif
            }
        }

        for (int i = 0; i < metaEBs; i++) {
            if (metaEBList[i] >= 0) {
                continue;
            }
            int eb = lowestEmptyEB();
            metadataEBList.push_back(eb);
#if FTL_DEBUG
            printf("Allocating %d\n ", eb);
#endif
            setEBMeta(eb);
            metaEBList[i] = eb;
            emptyEBs--;
        }

        metadataEpoch++;
        metadataEBindex = 0;
        metadataEBoffset = 0;
        metadataCRC.reset();
    }

    inline void writeMetadata8b(uint8_t b, char *wb) {
        if (metadataEBoffset == 4096 - 4) {
            uint32_t crc = metadataCRC.get();
            memcpy(&wb[flashWriteBufferSize - 4], &crc, 4);
            _fi->program(metadataEBList.front(), 4096 - flashWriteBufferSize, wb, flashWriteBufferSize);
            metadataEBList.erase(metadataEBList.begin());
            metadataCRC.reset();
            metadataEBoffset = 0;
            metadataEBindex++;
        }
        if (metadataEBoffset == 0) {
            bzero(wb, flashWriteBufferSize);
            memcpy(wb, metadataSig, 8);
            metadataCRC.add(metadataSig, 8);
            uint32_t ne = (metadataEpoch << 8) | metadataEBindex;
            memcpy(wb + 8, &ne, 4);
            metadataCRC.add(&ne, 4);
            metadataEBoffset = 12;
        }
        wb[metadataEBoffset % flashWriteBufferSize] = b;
        metadataCRC.add(b);
        metadataEBoffset++;
        if (0 == metadataEBoffset % flashWriteBufferSize) {
            if (metadataEBoffset == flashWriteBufferSize) {
                eraseEB(metadataEBList.front());
                setEBMeta(metadataEBList.front());
            }
            _fi->program(metadataEBList.front(), metadataEBoffset - flashWriteBufferSize, wb, flashWriteBufferSize);
            bzero(wb, flashWriteBufferSize);
        }
    }

    inline void writeMetadata16b(uint16_t t, char *wb) {
        writeMetadata8b((uint8_t)(t >> 8), wb);
        writeMetadata8b((uint8_t)(t & 0xff), wb);
    }

    inline void writeMetadata32b(uint32_t t, char *wb) {
        writeMetadata8b((uint8_t)(t >> 24), wb);
        writeMetadata8b((uint8_t)(t >> 16), wb);
        writeMetadata8b((uint8_t)(t >> 8), wb);
        writeMetadata8b((uint8_t)(t & 0xff), wb);
    }

    void closeMetadataStream(char *wb) {
        // We be lazy, just 0-pad until index loops (taking into account header size)
        while (metadataEBoffset > 13) {
            writeMetadata8b((uint8_t)0, wb);
        }
    }

    bool doPersist() {
        char wb[flashWriteBufferSize]; // Keep on stack to avoid needing to malloc() from inside persist

        openMetadataStreamForWrite(); // Will increment epoch, choose oldest MD copy to overwrite

        // Dump FTLInfo
        FTLInfo f = {.ebBytes = (uint16_t)ebBytes, .lbaBytes = (uint16_t)lbaBytes, .flashBytes = (uint32_t)flashBytes, .metaEBBytes = (uint16_t)metaEBBytes, .flashLBAs = (uint16_t)flashLBAs};
        uint8_t *p = (uint8_t*)&f;
        for (size_t i = 0; i < sizeof(f); i++) {
            writeMetadata8b(p[i], wb);
        }

        // Dump peCount
        for (int i = 0; i < eraseBlocks; i++) {
            writeMetadata8b(peCount[i], wb);
        }

        // Dump ebState
        for (int i = 0; i < (eraseBlocks + 1) / 2; i++) {
            writeMetadata8b(ebState[i], wb);
        }

        // Dump L2P
        uint16_t *q = (uint16_t*)(l2p);
        for (int i = 0; i < flashLBAs; i++) {
            writeMetadata16b(q[i], wb);
        }

        // peCountOffset
        writeMetadata32b(peCountOffset, wb);

        closeMetadataStream(wb); // Will 0-fill and add checksum at end

        metadataAge = 0;

        return true;
    }


    std::map<uint32_t /* epoch */, std::list<int> /* EBs that contain that epoch */> metadataMap;
    const uint8_t *mdOpenEB;

    void populateMetadataMap() {
#if FTL_DEBUG
        printf("populateMetadataMap()\n");
#endif
        metadataMap.clear();
        for (int i = 0; i < eraseBlocks; i++) {
            const uint8_t *eb = _fi->readEB(i);
            if (!memcmp(eb, metadataSig, 8)) {
                metadataCRC.reset();
                metadataCRC.add(eb, 4096 - 4);
                uint32_t crc = metadataCRC.get();
                if (!memcmp(&crc, eb + 4096 - 4, 4)) {
                    uint32_t epoch = *(const uint32_t *)&eb[8];
#if FTL_DEBUG
                    printf("Found MD epoch %d, idx %d at eb %d\n", (int)(epoch >> 8), (int)(epoch & 0xff), i);
#endif
                    auto l = metadataMap.find(epoch >> 8);
                    if (l != metadataMap.end()) {
                        l->second.push_back(i);
                    } else {
                        std::list<int> n;
                        n.push_back(i);
                        metadataMap.insert({epoch >> 8, n});
                    }
                } else {
#if FTL_DEBUG
                    printf("Found header but got CRC mismatch  EB %d\n", i);
#endif
                }
            }
        }
#if FTL_DEBUG
        for (auto x = metadataMap.begin(); x != metadataMap.end(); x++) {
            printf("epoch %d: ", (int)x->first);
            for (auto y = x->second.begin(); y != x->second.end(); y++) {
                printf("%d ", *y);
            }
            printf("\n");
        }
#endif
    }


    void openMetadataStreamForRead() {
        metadataEBoffset = 0;
        mdOpenEB = _fi->readEB(metadataEBList.front());
    }

    inline uint8_t readMetadata8b() {
        if (metadataEBoffset >= 4096 - 4) {
            metadataEBoffset = 0;
            metadataEBList.erase(metadataEBList.begin());
            mdOpenEB = _fi->readEB(metadataEBList.front());
        }
        if (metadataEBoffset < 12) {
            metadataEBoffset = 12;
        }
        return mdOpenEB[metadataEBoffset++];
    }

    inline uint16_t readMetadata16b() {
        return (readMetadata8b() << 8) | readMetadata8b();
    }

    inline uint32_t readMetadata32b() {
        return (readMetadata8b() << 24) | (readMetadata8b() << 16) | (readMetadata8b() << 8) | readMetadata8b();
    }

    bool doLoadHighestEpochMetadata() {
        uint32_t epoch = 0; // Should never be higher than anything on flash
        for (auto x = metadataMap.begin(); x != metadataMap.end(); x++) {
            if (x->first > epoch) {
                epoch = x->first;
            }
        }
        if (!epoch) {
            return false;
        }
#if FTL_DEBUG
        printf("Loading epoch %d from ", (int)epoch);
#endif
        metadataEBList.clear();
        auto ebs = metadataMap.find(epoch)->second;
        uint32_t epochidx = epoch << 8;
        for (int i = 0; i < metaEBBytes; i++) {
            for (auto x = ebs.begin(); x != ebs.end(); x++) {
                auto r = _fi->readEB(*x);
                if (*(uint32_t*)&r[8] == epochidx) {
                    metadataEBList.push_back(*x);
#if FTL_DEBUG
                    printf("%d ", *x);
#endif
                    break;
                }
            }
            epochidx++;
        }
#if FTL_DEBUG
        printf("\n");
#endif
        metadataMap.erase(epoch); // If this doesn't pass muster, then don't check it again
        openMetadataStreamForRead();

        // Dump FTLInfo
        FTLInfo f = {.ebBytes = (uint16_t)ebBytes, .lbaBytes = (uint16_t)lbaBytes, .flashBytes = (uint32_t)flashBytes, .metaEBBytes = (uint16_t)metaEBBytes, .flashLBAs = (uint16_t)flashLBAs};
        FTLInfo onFlash;
        uint8_t *p = (uint8_t*)&onFlash;
        for (size_t i = 0; i < sizeof(f); i++) {
            p[i] = readMetadata8b();
        }
        if (memcmp(&f, &onFlash, sizeof(f))) {
#if FTL_DEBUG
            printf("ERROR: FTL info doesn't match, skipping\n");
#endif
            return false;
        }

        // At this point, we blindly pull everything out. CRCs already verified
        highestPECount = 0;
        for (int i = 0; i < eraseBlocks; i++) {
            peCount[i] = readMetadata8b();
            if (peCount[i] > highestPECount) {
                highestPECount = peCount[i];
            }
        }

        // Clear existing meta EB list
        for (int i = 0; i < metaEBs; i++) {
            metaEBList[i] = -1;
        }
        emptyEBs = 0;
        for (int i = 0, j = 0; i < (eraseBlocks + 1) / 2; i++) {
            ebState[i] = readMetadata8b();
            // Restore metaEBList as we read in
            if (ebIsMeta(i * 2)) {
                metaEBList[j++] = i * 2;
            }
            if (ebIsMeta(i * 2 + 1)) {
                metaEBList[j++] = i * 2 + 1;
            }
            if (getEBState(i * 2) == 0) {
                emptyEBs++;
            }
            if (getEBState(i * 2 + 1) == 0) {
                emptyEBs++;
            }
        }

        validLBAs = 0;
        uint16_t *q = (uint16_t*)(l2p);
        for (int i = 0; i < flashLBAs; i++) {
            q[i] = readMetadata16b();
            if (l2p_val(i)) {
                validLBAs++;
            }
        }

        peCountOffset = readMetadata32b();

        // Nothing to close, this is a read operation only
        metadataEpoch = epoch;
        return true;
    }

    bool loadHighestEpochMetadata() {
        while (metadataMap.begin() != metadataMap.end()) {
            if (doLoadHighestEpochMetadata()) {
                metadataMap.clear();
                return true;
            }
        }
        metadataMap.clear();
        return false;
    }

    bool doCheck() {
        int max = 0;
        int min = 65536;
        int c = 0;
        int metas = 0;
        bool pass = true;
        for (int i = 0; i < eraseBlocks; i++) {
            c += !getEBState(i) ? 1 : 0;
            if (peCount[i] > max) {
                max = peCount[i];
            }
            if (min > peCount[i]) {
                min = peCount[i];
            }
            if (ebIsMeta(i)) {
                metas++;
            }
        }
        if (metas > metaEBs) {
#if FTL_DEBUG
            printf("ERROR: metas > metaEBs  %d > %d\n", metas, metaEBs);
#endif
            pass = false;
        }
        if (c != emptyEBs) {
#if FTL_DEBUG
            printf("ERROR: emptyEBs mismatch %d != %d\n", c, emptyEBs);
#endif
            pass = false;
        }
        if (max != highestPECount) {
#if FTL_DEBUG
            printf("ERROR: highestPECount mismatch %d != %d\n", max, highestPECount);
#endif
            pass = false;
        }
        if (max - min > maxPEDiff + 1) {
#if FTL_DEBUG
            printf("ERROR: maxPEDiff mismatch %d - %d    %d != %d\n", max, min, max - min, maxPEDiff);
#endif
            pass = false;
        }
        uint8_t val[eraseBlocks];
        bzero(val, sizeof(val));
        for (int i = 0; i < flashLBAs; i++) {
            if (l2p_val(i)) {
                auto eb = l2p_eb(i);
                auto idx = l2p_idx(i);
                if (ebIsMeta(eb)) {
#if FTL_DEBUG
                    printf("ERROR: LBA %d points to metadata\n", i);
#endif
                    pass = false;
                }
                if (val[eb] & 1 << idx) {
#if FTL_DEBUG
                    printf("ERROR: LBA %d crosslinked in eb %d idx %d\n", i, eb, idx);
#endif
                    pass = false;
                }
                val[eb] |= 1 << idx;
            }
        }
        return pass;
    }

    void eraseEB(int eb) {
#if FTL_DEBUG
        printf("EraseEB(%d)\n", eb);
#endif
        _fi->eraseBlock(eb);
        if (peCount[eb] > 250) {
            for (int i = 0; i < eraseBlocks; i++) {
                if (peCount[i] > maxPEDiff) {
                    peCount[i] -= maxPEDiff;
                } else {
#if FTL_DEBUG
                    printf("ERROR: underflow pecount reset eb %d - maxPEDiff %d\n", peCount[i], maxPEDiff);
#endif
                    peCount[i] = 0;
                }
            }
            highestPECount -= maxPEDiff;
            peCountOffset += maxPEDiff;
            // Every peCount entry just changed; a delta record can't capture
            // that compactly, so fold it into the next full snapshot.
            journalNeedsSnapshot = true;
        }
        peCount[eb]++;
        if (peCount[eb] > highestPECount) {
            highestPECount = peCount[eb];
        }

        setEBState(eb, 0);
        markPe(eb);
    }


    // ----- GARBAGE COLLECTION AND WEAR LEVELING
    inline int highestEmptyEB() {
        int highestEmptyPE = -1;
        int highestEmptyIdx = 0;
        for (int i = 0; i < eraseBlocks; i++) {
            if ((peCount[i] > highestEmptyPE) && (getEBState(i) == 0)) {
                highestEmptyPE = peCount[i];
                highestEmptyIdx = i;
            }
        }
        return highestEmptyIdx;
    }

    inline int lowestEmptyEB() {
        int lowestEmptyPE = 1 << 16; // 1 more than highest possible PECOUNT
        int lowestEmptyIdx = -1;
        for (int i = 0; i < eraseBlocks; i++) {
            if ((peCount[i] <= lowestEmptyPE) && (getEBState(i) == 0)) {
                lowestEmptyPE = peCount[i];
                lowestEmptyIdx = i;
            }
        }
        return lowestEmptyIdx;
    }

    void dumpMetadataEBs() {
#if FTL_DEBUG
        for (int i = 0; i < eraseBlocks; i++) {
            if (ebIsMeta(i)) {
                printf("MDEB %d: ", i);
                auto z = _fi->readEB(i);
                for (int j = 0; j < ebBytes; j++) {
                    printf("%02X ", z[j]);
                }
                printf("\n");
            }
        }
#endif
    }

    void ageMetadata() {
        if (++metadataAge == 0) {
            // Every 256 writes we
            persist();
            metaAgeRewrite();
        }
    }

    // Assumes the destEB is available to write date and has no gaps in its valid bits
    // Really ugly but w/o a reverse P2L map not sure how to get this otherwise
    int collectValidLBAs(int srcEB, int destEB, int destIdx) {
        int curIdx = destIdx;
        const uint8_t *readAddr = _fi->readEB(srcEB);
        for (int i = 0; (i < flashLBAs) && (curIdx < 8); i++) {
            if ((l2p_eb(i) == srcEB) && l2p_val(i)) {
#if FTL_DEBUG
                printf("moving lba%02d to eb%d idx%d\n", i, destEB, curIdx);
#endif
                uint8_t buff[flashWriteBufferSize];
                for (int j = 0; j < lbaBytes; j += sizeof(buff)) {
                    memcpy(buff, readAddr + 512 * l2p_idx(i) + j, sizeof(buff));
                    _fi->program(destEB, 512 * curIdx + j, buff, sizeof(buff));
                }
                clearLBAValid(srcEB);
                if (getEBState(srcEB) == 0) {
                    emptyEBs++;
                }
                l2p[i] = make_l2p(curIdx, destEB);
                markL2P(i);
                setEBState(destEB, getEBState(destEB) + 1);
                curIdx++;
            }
        }
        return curIdx;
    }

    inline int gcScore(int eb) {
        unsigned int state = getEBState(eb);
        if ((state == ebMeta) || (state == ebJournal) || !state) {
            return 0;
        }
        int delta = highestPECount - peCount[eb];
        if (delta >= maxPEDiff) {
            return 10 + delta - maxPEDiff; // Aged out, choose oldest
        }
        if (delta > ((maxPEDiff * 7) / 8)) {
            return 9; // Getting old, try to move before timeout
        }
        return 8 - state;
    }

    int garbageCollect() {
        int ebScore = 0;
        int destEB = lowestEmptyEB(); // We'll write data into the youngest flash
        assert(destEB >= 0);
        eraseEB(destEB);
        emptyEBs--;
        for (int cnt = 0; (getEBState(destEB) < 8) && (cnt < 8); cnt++) {   // Loop until full or at most 8 times since we should have at least 1 move per cycle
            static int eb = 0; // The current EB to GC, we'll start at the last eb checked and loop around
            // Find first non-meta, non-journal EB
            while (ebIsMeta(eb) || ebIsJournal(eb) || (eb == destEB)) {
                eb = (eb + 1) % eraseBlocks;
            }
            ebScore = gcScore(eb);
            for (int i = 1; (i < eraseBlocks) && (ebScore < 8); i++) {
                int ebMod = (eb + i) % eraseBlocks;
                if ((ebScore < gcScore(ebMod)) && (ebMod != destEB)) {
                    eb = ebMod;
                    ebScore = gcScore(eb);
                }
            }
            assert(ebScore > 0); // ERROR, couldn't find anything...we're toast
            assert(eb != destEB);
            setEBState(destEB, collectValidLBAs(eb, destEB, getEBState(destEB)));
        }
        return ebScore;
    }

    // Check all metadata EBs for age-out and rewrite if necessary
    void metaAgeRewrite() {
        for (int i = 0; i < metaEBs; i++) {
            int eb = metaEBList[i];
            if (eb < 0) {
                continue;
            }
            if (highestPECount - peCount[eb] >= maxPEDiff) {
                int destEB = lowestEmptyEB(); // We'll write data into the youngest flash
#if FTL_DEBUG
                printf("Aged-out metadata %d to %d\n", eb, destEB);
#endif
                assert(destEB >= 0);
                assert(destEB != eb);
                eraseEB(destEB);
                const uint8_t *readAddr = _fi->readEB(eb);
                uint8_t buff[flashWriteBufferSize];
                for (int i = 0; i < ebBytes; i += sizeof(buff)) {
                    memcpy(buff, readAddr + i, sizeof(buff));
                    _fi->program(destEB, i, buff, sizeof(buff));
                }
                setEBState(eb, 0);
                setEBMeta(destEB);
                metaEBList[i] = destEB;
                // The metadata EB list moved; the delta journal references a
                // base snapshot whose ebState no longer matches, so force a
                // fresh snapshot at the next persist to re-anchor.
                journalNeedsSnapshot = true;
            }
        }
    }

    int selectBestEB() {
        // Near-full guard: if the journal ring is holding blocks GC may need to
        // keep its 3-block reserve, release it first (folding its deltas into a
        // snapshot). Done before the GC loop below so emptyEBs can actually
        // reach its target instead of spinning against the ring.
        if (_journal && journalBaseValid && !journalSuspended &&
            (emptyEBs <= journalSuspendBelow)) {
            suspendJournalUnderPressure();
        }
        int ebScore = 0;
        // We need 3 EBs minimum to be free, and any score > 10 means we need to move for PE count wear leveling
        while ((emptyEBs < 3) || (ebScore > 10)) {
            ebScore = garbageCollect();
            metaAgeRewrite();
        }
        emptyEBs--;
        int eb = lowestEmptyEB();
#if FTL_DEBUG
        printf("selectBestEB() = %d\n", eb);
#endif
        eraseEB(eb);
        return eb;
    }


    // ----- DELTA JOURNAL
    //
    // On-flash journal page layout (one page == flashWriteBufferSize bytes,
    // the minimum program unit; 256 B on RP2040 hardware). Each page is a
    // self-contained, atomically-programmed record:
    //
    //   [0]    "JRNL"            signature (4 B)
    //   [4]    baseEpoch (u32)   snapshot epoch this delta extends
    //   [8]    recordSeq (u32)   monotonic page index within the cycle
    //   [12]   subIndex (u8)     reserved (always 0 in this implementation)
    //   [13]   subTotal (u8)     reserved (always 1: single-page records)
    //   [14]   highestPECount (u16)
    //   [16]   peCountOffset (u32)
    //   [20]   nL2P (u16)        count of changed L2P entries
    //   [22]   nPe  (u16)        count of changed peCount entries
    //   [24]   nL2P x {lba:u16, l2p:u16} then nPe x {eb:u16, pe:u8}
    //   [pb-4] crc32 over bytes [0 .. pb-4)
    //
    // A delta that does not fit in one page falls back to a full snapshot, so
    // every journal record is exactly one program with no erase. ebState is
    // NOT journaled: its per-EB valid counts are recomputed from the L2P map
    // on boot (recomputeDerivedState), keeping records minimal.
    static const int JHDR = 24;

    bool journalPersist() {
        // Suspended (released the ring under pressure): rebuild it only if free
        // space has recovered past the hysteresis high-water mark; otherwise
        // just take a plain full snapshot (exactly upstream behavior).
        if (journalSuspended) {
            if (emptyEBs >= journalResumeAbove) {
                return doFullSnapshot();   // will re-allocate the ring
            }
            bool ret = doPersist();        // full snapshot captures everything...
            clearDirty();                  // ...so the delta set resets too
            _fi->serialize();
            return ret;
        }
        if (!journalBaseValid || journalNeedsSnapshot) {
            return doFullSnapshot();
        }
        if (!dirtyL2Pcount && !dirtyPecount) {
            return true; // nothing changed since last record
        }
        int need = JHDR + 4 * dirtyL2Pcount + 3 * dirtyPecount + 4 /* crc */;
        if (need > flashWriteBufferSize) {
            return doFullSnapshot(); // delta too big for one page
        }
        if ((int)journalSeq >= journalCapacityPages()) {
            return doFullSnapshot(); // ring full
        }
        buildAndProgramJournalPage();
        clearDirty();
        metadataAge = 0;
        _fi->serialize();
        return true;
    }

    bool doFullSnapshot() {
        freeOldJournal();                 // release any current ring back to the pool
        bool haveRing = tryAllocateJournal(); // carve a fresh ring iff there's room
        bool ret = doPersist();           // bumps epoch, writes snapshot (records ring iff haveRing)
        journalSeq = 0;
        clearDirty();
        journalNeedsSnapshot = false;
        journalBaseValid = haveRing;
        journalSuspended = !haveRing;     // no room -> stay in full-snapshot mode
        _fi->serialize();
        return ret;
    }

    void freeOldJournal() {
        for (int i = 0; i < journalEBs; i++) {
            int eb = journalEBList[i];
            if (eb >= 0) {
                setEBState(eb, 0);
                emptyEBs++;
                journalEBList[i] = -1;
            }
        }
    }

    // Carve `journalEBs` blocks for the ring, but only if doing so still leaves
    // GC its 3-block reserve. Returns false (ring not allocated) when the
    // volume is too full - the caller then runs in full-snapshot mode.
    bool tryAllocateJournal() {
        if (emptyEBs < journalEBs + 3) {
            return false;
        }
        int picked[journalEBs];
        for (int i = 0; i < journalEBs; i++) {
            int eb = lowestEmptyEB();
            if (eb < 0) { // shouldn't happen given the check above; be safe
                // roll back any we already marked
                for (int k = 0; k < i; k++) { setEBState(picked[k], 0); emptyEBs++; }
                return false;
            }
            setEBState(eb, ebJournal); // reserve so next lowestEmptyEB() skips it
            emptyEBs--;
            picked[i] = eb;
        }
        // Insertion sort ascending so physical fill order is deterministic and
        // reproducible from an ebState scan on boot (journalEBs is tiny).
        for (int i = 1; i < journalEBs; i++) {
            int v = picked[i], j = i - 1;
            while (j >= 0 && picked[j] > v) { picked[j + 1] = picked[j]; j--; }
            picked[j + 1] = v;
        }
        for (int i = 0; i < journalEBs; i++) {
            journalEBList[i] = picked[i];
            eraseEB(picked[i]);             // blank it for append-only programming
            setEBState(picked[i], ebJournal); // eraseEB() cleared the nibble; re-mark
        }
        journalSeq = 0;
        return true;
    }

    // Called from the allocation path when free blocks get low: fold the
    // current state (snapshot + applied deltas) into a fresh snapshot, hand the
    // ring's blocks back to the pool, and switch to full-snapshot mode until
    // space recovers. This keeps GC's free-block reserve available.
    void suspendJournalUnderPressure() {
        doPersist();          // fold everything into a durable snapshot (no ring written)
        freeOldJournal();     // give the ring's blocks back -> emptyEBs += journalEBs
        journalSeq = 0;
        clearDirty();
        journalBaseValid = false;
        journalSuspended = true;
        journalNeedsSnapshot = false;
        _fi->serialize();
    }

    void buildAndProgramJournalPage() {
        uint8_t pg[flashWriteBufferSize];
        bzero(pg, flashWriteBufferSize);
        memcpy(pg, journalSig, 4);
        uint32_t be = metadataEpoch;
        memcpy(pg + 4, &be, 4);
        uint32_t seq = journalSeq;
        memcpy(pg + 8, &seq, 4);
        pg[12] = 0;
        pg[13] = 1;
        uint16_t hpc = (uint16_t)highestPECount;
        memcpy(pg + 14, &hpc, 2);
        uint32_t pco = (uint32_t)peCountOffset;
        memcpy(pg + 16, &pco, 4);
        uint8_t *p = pg + JHDR;
        uint16_t nL2P = 0, nPe = 0;
        for (int lba = 0; lba < flashLBAs; lba++) {
            if (testL2P(lba)) {
                uint16_t l = (uint16_t)lba;
                uint16_t v = (uint16_t)l2p[lba];
                memcpy(p, &l, 2); p += 2;
                memcpy(p, &v, 2); p += 2;
                nL2P++;
            }
        }
        for (int eb = 0; eb < eraseBlocks; eb++) {
            if (testPe(eb)) {
                uint16_t e = (uint16_t)eb;
                memcpy(p, &e, 2); p += 2;
                *p++ = peCount[eb];
                nPe++;
            }
        }
        memcpy(pg + 20, &nL2P, 2);
        memcpy(pg + 22, &nPe, 2);
        MetadataCRC32 c;
        c.add(pg, flashWriteBufferSize - 4);
        uint32_t crc = c.get();
        memcpy(pg + flashWriteBufferSize - 4, &crc, 4);

        int ebSlot = journalSeq / pagesPerEB();
        int off = (journalSeq % pagesPerEB()) * flashWriteBufferSize;
#if FTL_DEBUG
        printf("journal page seq %d -> eb %d off %d (%d l2p, %d pe)\n",
               (int)journalSeq, journalEBList[ebSlot], off, nL2P, nPe);
#endif
        _fi->program(journalEBList[ebSlot], off, pg, flashWriteBufferSize);
        journalSeq++;
    }

    bool validJournalPage(const uint8_t *pg, uint32_t expectSeq) {
        if (memcmp(pg, journalSig, 4)) return false; // blank or not a record
        uint32_t be; memcpy(&be, pg + 4, 4);
        if (be != metadataEpoch) return false;       // belongs to an older cycle
        uint32_t seq; memcpy(&seq, pg + 8, 4);
        if (seq != expectSeq) return false;           // gap (torn tail)
        MetadataCRC32 c;
        c.add(pg, flashWriteBufferSize - 4);
        uint32_t crc = c.get();
        uint32_t stored; memcpy(&stored, pg + flashWriteBufferSize - 4, 4);
        return crc == stored;
    }

    void applyJournalPage(const uint8_t *pg) {
        uint16_t hpc; memcpy(&hpc, pg + 14, 2); highestPECount = hpc;
        uint32_t pco; memcpy(&pco, pg + 16, 4); peCountOffset = pco;
        uint16_t nL2P; memcpy(&nL2P, pg + 20, 2);
        uint16_t nPe; memcpy(&nPe, pg + 22, 2);
        const uint8_t *p = pg + JHDR;
        // The CRC32 lives in the last 4 bytes; never read entries past it.
        // Defense-in-depth: even though validJournalPage() verified the CRC,
        // bounds-check the decoded indices so a (vanishingly unlikely) CRC
        // collision or a page from a mismatched format can never scribble
        // outside the l2p[] / peCount[] arrays.
        const uint8_t *limit = pg + (flashWriteBufferSize - 4);
        for (int i = 0; i < nL2P; i++) {
            if (p + 4 > limit) return;
            uint16_t lba; memcpy(&lba, p, 2); p += 2;
            uint16_t v; memcpy(&v, p, 2); p += 2;
            if (lba < flashLBAs) l2p[lba] = v;
        }
        for (int i = 0; i < nPe; i++) {
            if (p + 3 > limit) return;
            uint16_t eb; memcpy(&eb, p, 2); p += 2;
            uint8_t pe = *p++;
            if (eb < eraseBlocks) peCount[eb] = pe;
        }
    }

    int scanJournalList() {
        int j = 0;
        for (int i = 0; i < journalEBs; i++) journalEBList[i] = -1;
        for (int eb = 0; (eb < eraseBlocks) && (j < journalEBs); eb++) {
            if (ebIsJournal(eb)) journalEBList[j++] = eb;
        }
        return j;
    }

    void replayJournal() {
        journalSeq = 0;
        int cap = journalCapacityPages();
        for (int pageIdx = 0; pageIdx < cap; pageIdx++) {
            int ebSlot = pageIdx / pagesPerEB();
            int off = (pageIdx % pagesPerEB()) * flashWriteBufferSize;
            int eb = journalEBList[ebSlot];
            if (eb < 0) break;
            const uint8_t *pg = _fi->readEB(eb) + off;
            if (!validJournalPage(pg, (uint32_t)pageIdx)) break; // first blank/torn page ends replay
            applyJournalPage(pg);
            journalSeq = pageIdx + 1;
        }
    }

    // After loading a snapshot and replaying the journal, the L2P map is
    // current but ebState's per-EB valid counts (and the derived scalars)
    // reflect only the snapshot. Rebuild them from the L2P map, preserving the
    // meta and journal block markers.
    void recomputeDerivedState() {
        for (int eb = 0; eb < eraseBlocks; eb++) {
            if (!ebIsMeta(eb) && !ebIsJournal(eb)) setEBState(eb, 0);
        }
        validLBAs = 0;
        for (int lba = 0; lba < flashLBAs; lba++) {
            if (l2p_val(lba)) {
                int eb = l2p_eb(lba);
                if (!ebIsMeta(eb) && !ebIsJournal(eb)) setEBState(eb, getEBState(eb) + 1);
                validLBAs++;
            }
        }
        emptyEBs = 0;
        highestPECount = 0;
        for (int eb = 0; eb < eraseBlocks; eb++) {
            if (getEBState(eb) == 0) emptyEBs++;
            if (peCount[eb] > highestPECount) highestPECount = peCount[eb];
        }
    }

};
