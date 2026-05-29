/*
    journaltest.cpp - Crash-recovery / durability tests for the SPIFTL
    delta-journal (the append-only fast-persist mode).

    Build & run:
        g++ -std=c++17 -g -O0 -DSPIFTL_RAM_STRICT -o journaltest journaltest.cpp && ./journaltest

    The test drives the host RAM flash emulator (FlashInterfaceRAM), which
    persists to "flash.bin" via serialize()/deserialize(). A "reboot" is
    therefore just: destroy the FTL+flash, then construct a fresh pair and
    call start() (which deserializes flash.bin and replays the journal).

    Copyright (c) 2024 Earle F. Philhower, III  (test additions: JumperlOS fork)
    GNU LGPL v3 - see LICENSE.md
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <cassert>
#include <map>
#include <vector>

#include "SPIFTL.h"
#include "FlashInterfaceRAM.h"

static const int FLASH_SIZE = 4 * 1024 * 1024;
static const int LBA_BYTES = 512;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; } } while (0)

// Deterministic content for a (lba, version) pair so we can verify exact bytes.
static void fillPattern(uint8_t *buf, int lba, int version) {
    for (int i = 0; i < LBA_BYTES; i++) {
        buf[i] = (uint8_t)((lba * 31 + version * 7 + i * 13) & 0xff);
    }
}

// The "truth" model: lba -> version (version 0 == trimmed/absent).
typedef std::map<int, int> Truth;

static void verifyAll(SPIFTL &ftl, const Truth &truth, const char *where) {
    uint8_t got[LBA_BYTES], want[LBA_BYTES];
    for (auto &kv : truth) {
        int lba = kv.first, ver = kv.second;
        ftl.read(lba, got);
        if (ver == 0) {
            uint8_t zero[LBA_BYTES];
            bzero(zero, sizeof(zero));
            if (memcmp(got, zero, LBA_BYTES)) {
                printf("  FAIL [%s]: trimmed lba %d returned data\n", where, lba);
                g_fail++;
            }
        } else {
            fillPattern(want, lba, ver);
            if (memcmp(got, want, LBA_BYTES)) {
                printf("  FAIL [%s]: lba %d expected v%d, got mismatch\n", where, lba, ver);
                g_fail++;
            }
        }
    }
}

static void doWrite(SPIFTL &ftl, Truth &truth, int lba, int version) {
    uint8_t buf[LBA_BYTES];
    fillPattern(buf, lba, version);
    ftl.write(lba, buf);
    truth[lba] = version;
}

// ---- flash.bin manipulation to simulate a torn write ----

static void freshFlash() {
    remove("flash.bin");
}

static const int EB_BYTES = 4096;
static const int PAGE_BYTES = 128;            // FlashInterfaceRAM::writeBufferSize()
static const int PAGES_PER_EB = EB_BYTES / PAGE_BYTES;

// Corrupt exactly one page of one erase block in flash.bin (simulating a torn
// final page program). Flips a payload byte so the page CRC no longer matches.
static bool corruptPage(int ebIndex, int pageWithin) {
    FILE *f = fopen("flash.bin", "rb+");
    if (!f) return false;
    long off = (long)ebIndex * EB_BYTES + (long)pageWithin * PAGE_BYTES + 30 /* a payload byte */;
    fseek(f, off, SEEK_SET);
    uint8_t b;
    if (fread(&b, 1, 1, f) != 1) { fclose(f); return false; }
    b ^= 0xff;
    fseek(f, off, SEEK_SET);
    fwrite(&b, 1, 1, f);
    fclose(f);
    return true;
}

// ---- scenarios ----

static void scenarioBasic() {
    printf("Scenario A: basic journal durability across reboot\n");
    freshFlash();
    Truth truth;
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, /*journal=*/true);
        ftl.start();                       // formats (no snapshot yet)
        for (int k = 0; k < 25; k++) {
            doWrite(ftl, truth, k, 1);
            ftl.persist();                 // simulate f_close -> journal append
        }
        CHECK(ftl.check(), "check() after writes");
        CHECK(ftl.debugJournalRecords() >= 1, "at least one journal record");
        verifyAll(ftl, truth, "A-prereboot");
    }
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();                       // deserialize + replay journal
        CHECK(ftl.check(), "check() after reboot");
        verifyAll(ftl, truth, "A-postreboot");
    }
    printf("  done (records replayed correctly)\n");
}

static void scenarioOverwriteTrim() {
    printf("Scenario B: overwrite + trim survive reboot\n");
    freshFlash();
    Truth truth;
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        for (int k = 0; k < 40; k++) { doWrite(ftl, truth, k, 1); ftl.persist(); }
        for (int k = 0; k < 40; k += 2) { doWrite(ftl, truth, k, 2); ftl.persist(); }
        for (int k = 1; k < 40; k += 4) { ftl.trim(k); truth[k] = 0; ftl.persist(); }
        CHECK(ftl.check(), "check() B-pre");
        verifyAll(ftl, truth, "B-prereboot");
    }
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        CHECK(ftl.check(), "check() B-post");
        verifyAll(ftl, truth, "B-postreboot");
    }
    printf("  done\n");
}

static void scenarioLostUnsynced() {
    printf("Scenario C: write WITHOUT persist is lost on power cut (no torn metadata)\n");
    freshFlash();
    Truth truth;
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        for (int k = 0; k < 10; k++) { doWrite(ftl, truth, k, 1); ftl.persist(); }
        // Now write one more LBA but DO NOT persist (power dies before f_close).
        uint8_t buf[LBA_BYTES];
        fillPattern(buf, 99, 1);
        ftl.write(99, buf);                // physically programmed, not journaled
        // (no persist) -> destroyed
    }
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        CHECK(ftl.check(), "check() C-post");
        verifyAll(ftl, truth, "C-postreboot");   // lbas 0..9 intact
        uint8_t got[LBA_BYTES], zero[LBA_BYTES];
        bzero(zero, sizeof(zero));
        ftl.read(99, got);
        CHECK(!memcmp(got, zero, LBA_BYTES), "unsynced lba 99 correctly absent");
    }
    printf("  done\n");
}

static void scenarioTornTail() {
    printf("Scenario D: torn final journal page reverts to last good persist\n");
    freshFlash();
    Truth truth, truthBeforeLast;
    int lastEB = -1, lastPage = -1;
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        for (int k = 0; k < 15; k++) { doWrite(ftl, truth, k, 1); ftl.persist(); }
        truthBeforeLast = truth;           // state after the 2nd-to-last persist
        doWrite(ftl, truth, 15, 1);        // this LAST persist will be "torn"
        ftl.persist();
        CHECK(ftl.check(), "check() D-pre");
        // Locate the exact page the final record was appended to.
        int recs = ftl.debugJournalRecords();   // == next seq; last record is recs-1
        int idx = recs - 1;
        int ebSlot = idx / PAGES_PER_EB;
        lastPage = idx % PAGES_PER_EB;
        lastEB = ftl.debugJournalEB(ebSlot);
    }
    CHECK(lastEB >= 0, "located final journal page");
    CHECK(corruptPage(lastEB, lastPage), "corrupt the final journal page");
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();                       // replay must stop at the torn page
        CHECK(ftl.check(), "check() D-post");
        // lba 15 (the torn save) should be gone; 0..14 intact.
        verifyAll(ftl, truthBeforeLast, "D-postreboot");
        uint8_t got[LBA_BYTES], zero[LBA_BYTES];
        bzero(zero, sizeof(zero));
        ftl.read(15, got);
        CHECK(!memcmp(got, zero, LBA_BYTES), "torn lba 15 correctly absent");
    }
    printf("  done\n");
}

static void scenarioRollover() {
    printf("Scenario E: snapshot rollover when journal ring fills\n");
    freshFlash();
    Truth truth;
    uint32_t epochBefore = 0, epochAfter = 0;
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        doWrite(ftl, truth, 0, 1);
        ftl.persist();                     // establishes first snapshot
        epochBefore = ftl.debugEpoch();
        // Hammer one LBA repeatedly: each persist is a 1-record append; once
        // the ring (journalEBs * pages) fills, a full snapshot must roll over.
        for (int i = 2; i < 400; i++) {
            doWrite(ftl, truth, 0, i);
            ftl.persist();
            CHECK(ftl.check(), "check() during rollover loop");
        }
        epochAfter = ftl.debugEpoch();
        verifyAll(ftl, truth, "E-prereboot");
    }
    CHECK(epochAfter > epochBefore, "epoch advanced (a rollover snapshot happened)");
    printf("    epoch %u -> %u\n", epochBefore, epochAfter);
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        CHECK(ftl.check(), "check() E-post");
        verifyAll(ftl, truth, "E-postreboot");
    }
    printf("  done\n");
}

// Heavy random churn to exercise GC + wear-leveling interacting with the
// journal (collectValidLBAs / eraseEB marking, metaAgeRewrite forcing
// snapshots, ring rollover), then verify durability across a reboot.
static void scenarioChurn(int seed, int ops) {
    printf("Scenario F: %d random ops (seed %d) with GC + journal, then reboot\n", ops, seed);
    freshFlash();
    srand(seed);
    int nLBA;
    Truth truth;
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        nLBA = ftl.lbaCount();
        for (int i = 0; i < ops; i++) {
            int lba = rand() % nLBA;
            if ((rand() % 16) == 0 && truth.count(lba) && truth[lba]) {
                ftl.trim(lba);
                truth[lba] = 0;
            } else {
                int ver = (truth.count(lba) ? truth[lba] : 0) + 1;
                if (ver > 250) ver = 1;    // keep version in a byte-ish range
                doWrite(ftl, truth, lba, ver);
            }
            if ((rand() % 4) == 0) ftl.persist();      // ~25% of ops are an f_close
            if ((i % 5000) == 0) CHECK(ftl.check(), "check() during churn");
        }
        ftl.persist();                                  // final sync
        CHECK(ftl.check(), "check() F-pre");
        verifyAll(ftl, truth, "F-prereboot");
        printf("    %d valid LBAs, epoch %u, %d journal records since last snapshot\n",
               (int)truth.size(), ftl.debugEpoch(), ftl.debugJournalRecords());
    }
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        CHECK(ftl.check(), "check() F-post");
        verifyAll(ftl, truth, "F-postreboot");
    }
    printf("  done\n");
}

// Fill the volume to capacity (which forces the journal to suspend and hand
// its ring blocks back to GC), confirm writes still succeed and survive a
// reboot, then free space and confirm the journal resumes appending.
static void scenarioNearFull() {
    printf("Scenario G: fill to capacity (journal suspends), then free + resume\n");
    freshFlash();
    Truth truth;
    int nLBA;
    {
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        nLBA = ftl.lbaCount();
        // Fill every LBA, persisting periodically. As free blocks run out the
        // ring is released and persist() falls back to full snapshots - this
        // must NOT hang or assert.
        for (int lba = 0; lba < nLBA; lba++) {
            doWrite(ftl, truth, lba, 1);
            if ((lba % 64) == 0) ftl.persist();
        }
        ftl.persist();
        CHECK(ftl.check(), "check() G full");
        CHECK(ftl.debugJournalSuspended(), "journal suspended at capacity");
        printf("    full: %d LBAs, empty EBs %d, suspended=%d\n",
               nLBA, ftl.debugEmptyEBs(), (int)ftl.debugJournalSuspended());
        verifyAll(ftl, truth, "G-full");
    }
    {   // survives reboot while suspended
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        CHECK(ftl.check(), "check() G reboot-full");
        verifyAll(ftl, truth, "G-reboot-full");
        // Free a big chunk; trims return blocks to the pool.
        for (int lba = 0; lba < nLBA / 2; lba++) {
            ftl.trim(lba); truth[lba] = 0;
            if ((lba % 64) == 0) ftl.persist();
        }
        ftl.persist();
        // A few more writes should now ride the resumed journal fast-path.
        for (int lba = 0; lba < 10; lba++) { doWrite(ftl, truth, lba, 5); ftl.persist(); }
        CHECK(ftl.check(), "check() G after-free");
        CHECK(!ftl.debugJournalSuspended(), "journal resumed after freeing space");
        printf("    after free: empty EBs %d, suspended=%d, records=%d\n",
               ftl.debugEmptyEBs(), (int)ftl.debugJournalSuspended(), ftl.debugJournalRecords());
        verifyAll(ftl, truth, "G-after-free");
    }
    {   // final reboot durability
        FlashInterfaceRAM fi(FLASH_SIZE);
        SPIFTL ftl(&fi, true);
        ftl.start();
        CHECK(ftl.check(), "check() G final");
        verifyAll(ftl, truth, "G-final");
    }
    printf("  done\n");
}

// Static-wear-leveling stressor: 1/4 of LBAs are written once (static data),
// the other 3/4 are rewritten + persisted repeatedly (hot data). Returns the
// total flash erases (sum of PE counts) and fills *spread with max-min PE.
// `persistEach` mimics an f_close after every write.
static long wearStress(bool journal, int writes, int* spread, int* mnOut, int* mxOut) {
    freshFlash();
    // 4 MB partition (1024 EBs, metaEBs=10) - representative of JumperlOS, and
    // big enough that a full snapshot rewrites many metadata blocks (the cost
    // the journal avoids). No cross-reboot here, so skip the disk serialize.
    FlashInterfaceRAM fi(4 * 1024 * 1024);
    fi.persistToDisk = false;
    SPIFTL ftl(&fi, journal);
    ftl.start();
    int nLBA = ftl.lbaCount();
    uint8_t buf[LBA_BYTES];
    srand(99);
    for (int i = 0; i < nLBA / 4; i++) { fillPattern(buf, i, 1); ftl.write(i, buf); }
    for (int i = 0; i < writes; i++) {
        int lba = nLBA / 4 + (rand() % ((nLBA * 3) / 4));
        fillPattern(buf, lba, (i & 0x7f) + 1);
        ftl.write(lba, buf);
        ftl.persist();                  // every write is an f_close in this stressor
    }
    ftl.persist();
    CHECK(ftl.check(), journal ? "check() wear journal-on" : "check() wear journal-off");
    int ebs = ftl.ebCount();
    int mn = 1 << 30, mx = 0; long sum = 0;
    for (int i = 0; i < ebs; i++) {
        int pe = ftl.getPECountOffset() + ftl.getPECount(i);
        if (pe < mn) mn = pe;
        if (pe > mx) mx = pe;
        sum += pe;
    }
    if (spread) *spread = mx - mn;
    if (mnOut) *mnOut = mn;
    if (mxOut) *mxOut = mx;
    return sum;
}

// Verify static wear leveling holds with journaling on, AND compare total
// flash erases against the full-snapshot-per-persist (journal off) path - the
// headline durability/endurance win of the journal.
static void scenarioWearLevel(int writes) {
    printf("Scenario H: wear leveling + erase comparison (%d write+persist cycles)\n", writes);
    int spreadOn = 0, mnOn = 0, mxOn = 0, spreadOff = 0;
    long erasesOff = wearStress(false, writes, &spreadOff, nullptr, nullptr);
    long erasesOn  = wearStress(true,  writes, &spreadOn,  &mnOn, &mxOn);
    CHECK(spreadOn  <= 64 + 1, "journal-on PE spread within maxPEDiff");
    CHECK(spreadOff <= 64 + 1, "journal-off PE spread within maxPEDiff");
    printf("    journal ON : total erases=%ld  PE min=%d max=%d spread=%d\n",
           erasesOn, mnOn, mxOn, spreadOn);
    printf("    journal OFF: total erases=%ld  spread=%d  (full snapshot every persist)\n",
           erasesOff, spreadOff);
    if (erasesOn > 0) {
        printf("    -> journaling did %.1fx fewer flash erases for the same workload\n",
               (double)erasesOff / (double)erasesOn);
    }
    printf("  done\n");
}

int main(int argc, char **argv) {
    int seed = (argc >= 2) ? atoi(argv[1]) : 12345;
    int churnOps = (argc >= 3) ? atoi(argv[2]) : 100000;
    printf("SPIFTL delta-journal crash-recovery tests\n");
    scenarioBasic();
    scenarioOverwriteTrim();
    scenarioLostUnsynced();
    scenarioTornTail();
    scenarioRollover();
    scenarioNearFull();
    scenarioWearLevel(20000);
    scenarioChurn(seed, churnOps);
    remove("flash.bin");
    if (g_fail) {
        printf("\nRESULT: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("\nRESULT: all journal tests passed\n");
    return 0;
}
