/*
    FlashInterfaceRAM.h - Host flash emulation for SPIFTL

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

#include "FlashInterface.h"

// DRAM simulation for host-based testing, NBD, etc.
class FlashInterfaceRAM : public FlashInterface {
public:
    FlashInterfaceRAM(int size) {
        _flashSize = size;
        _flash = new uint8_t[_flashSize];
        _isErased = new uint8_t[_flashSize / ebBytes];
        bzero(_isErased, _flashSize / ebBytes);
        // Model a blank chip: every cell starts in the erased state. (Without
        // this the buffer is uninitialized heap, which makes crash-recovery
        // tests non-deterministic - a fresh instance could observe a prior
        // run's freed image.) deserialize() overrides this if a file exists.
#ifdef SPIFTL_RAM_STRICT
        memset(_flash, 0xff, _flashSize);
#else
        bzero(_flash, _flashSize);
#endif
    }

    virtual ~FlashInterfaceRAM() override {
        delete[] _isErased;
        delete[] _flash;
    }

    virtual int size() override {
        return _flashSize;
    }

    virtual int writeBufferSize() override {
        return 128;
    }

    virtual const uint8_t *readEB(int eb) override {
        return &_flash[eb * ebBytes];
    }

    // Test hook: skip writing flash.bin on serialize(). Useful for throughput/
    // wear measurements that don't need cross-"reboot" persistence, so a 4 MB
    // image isn't flushed to disk on every persist().
    bool persistToDisk = true;

    virtual void serialize() override {
        if (!persistToDisk) return;
        FILE *f = fopen("flash.bin", "wb");
        if (f) {
            fwrite(_flash, 1, _flashSize, f);
            fclose(f);
        }
    }

    virtual void deserialize() override {
        FILE *f = fopen("flash.bin", "rb");
        if (f) {
            if (fread(_flash, 1, _flashSize, f) != _flashSize) {
                bzero(_flash, _flashSize);
            }
            fclose(f);
        }
    }

    virtual bool eraseBlock(int eb) override {
        if (_isErased[eb]) {
            // Commenting out this check because the MD operations do an erase when changing epochs
            // If they don't erase, they could leave stale MD to be found on startup, wasting RAM and time
            // Testing w/FIO doesn't show any re-erases outside of the MD operations.
            //printf("ERROR: Erasing already erased block eb %d\n", eb);
        }
        _isErased[eb] = 1;
        if (eb < _flashSize / ebBytes) {
#ifdef SPIFTL_RAM_STRICT
            // Model real NOR: an erased cell reads as all-ones (0xFF).
            memset(&_flash[eb * ebBytes], 0xff, ebBytes);
#else
            bzero(&_flash[eb * ebBytes], ebBytes);
#endif
            return true;
        }
        return false;
    }

    virtual bool program(int eb, int offset, const void *data, int size) override {
        if (eb < _flashSize / ebBytes) {
#ifdef SPIFTL_RAM_STRICT
            // Real NOR flash can only clear bits (1 -> 0) on program; setting a
            // bit requires an erase first. Model that (stored &= data) and flag
            // any attempt to set a bit, so tests catch a "program over a non-
            // erased page" bug (a journal append that forgot to erase, a
            // miscomputed write cursor, double-programming the same page, ...).
            const uint8_t *src = (const uint8_t *)data;
            uint8_t *dst = &_flash[eb * ebBytes + offset];
            for (int i = 0; i < size; i++) {
                if ((dst[i] & src[i]) != src[i]) {
                    if (_norViolations < 20) {
                        fprintf(stderr,
                                "NOR VIOLATION: program eb %d off %d: cannot set bits 0x%02x over 0x%02x\n",
                                eb, offset + i, src[i], dst[i]);
                    }
                    _norViolations++;
                }
                dst[i] &= src[i];
            }
            _isErased[eb] = 0;
            return true;
#else
            _isErased[eb] = 0;
            memcpy(&_flash[eb * ebBytes + offset], data, size);
            return true;
#endif
        }
        return false;
    }

#ifdef SPIFTL_RAM_STRICT
    int norViolations() const { return _norViolations; }
#endif

    virtual bool read(int eb, int offset, void *data, int size) override {
        if (eb < _flashSize / ebBytes) {
            memcpy(data, &_flash[eb * ebBytes + offset], size);
            return true;
        }
        return false;
    }

private:
    const int ebBytes = 4096;
    int _flashSize;
    uint8_t *_flash;
    uint8_t *_isErased;
#ifdef SPIFTL_RAM_STRICT
    int _norViolations = 0;
#endif
};
