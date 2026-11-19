/* board_io.cpp - see board_io.h.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#include <Arduino.h>
#include <SD.h>
#include <new>
#include <memory>
#include "board_io.h"
#include "audio_codecs/preview/teensy_stream_adapter.h"

using audio_codecs::preview::SeekableReader;
using audio_codecs::preview::SeekableWriter;
typedef audio_codecs::preview::TeensyFileStream<File> SdStream;

/* The loader has at most the audio file, one sidecar being read and one being
 * written open at once. */
namespace {
struct Slot {
    bool used = false;
    File file;
    alignas(SdStream) uint8_t stream[sizeof(SdStream)];
    SdStream *s() { return reinterpret_cast<SdStream *>(stream); }
};
Slot g_slots[4];

class SdStorage : public Storage {
public:
    SeekableReader *openRead(const char *path) override
    {
        Slot *sl = take();
        if (!sl) return nullptr;
        sl->file = SD.open(path, FILE_READ);
        if (!sl->file || sl->file.isDirectory()) { sl->file.close(); sl->used = false; return nullptr; }
        return new (sl->stream) SdStream(sl->file);
    }
    SeekableWriter *openWrite(const char *path) override
    {
        Slot *sl = take();
        if (!sl) return nullptr;
        if (SD.exists(path)) SD.remove(path);               /* FILE_WRITE appends: start from nothing */
        sl->file = SD.open(path, FILE_WRITE);
        if (!sl->file) { sl->used = false; return nullptr; }
        return new (sl->stream) SdStream(sl->file);
    }
    void close(SeekableReader *r) override { if (r) drop(static_cast<SdStream *>(r)); }
    void close(SeekableWriter *w) override { if (w) drop(static_cast<SdStream *>(w)); }

private:
    Slot *take()
    {
        for (Slot &s : g_slots) if (!s.used) { s.used = true; return &s; }
        return nullptr;
    }
    void drop(SdStream *st)
    {
        for (Slot &s : g_slots)
            if (s.used && s.s() == st) { s.file.flush(); s.file.close(); std::destroy_at(st); s.used = false; return; }
    }
};
SdStorage g_storage;
}

#if defined(ARDUINO_NOSH)
/* Nosh shares one pad, GPIO_SD_B2_05, between the flash read strobe and the SD
 * card: the boot header runs FlexSPI1 at 133 MHz sampling on the DQS pad
 * loopback, and uSDHC2 needs the same pad as CMD.  The moment SD.begin() takes
 * the pad, every read from flash returns junk, and this firmware keeps most of
 * its code there.  So before the card starts, move the flash to the internal
 * loopback, which needs no pad, and to the 60 MHz limit that mode has.
 * Runs from RAM with interrupts off: flash is unreadable part way through. */
FASTRUN __attribute__((noinline)) static void flash_release_dqs_pad()
{
    volatile uint32_t &mcr0 = *(volatile uint32_t *)0x400CC000u;
    volatile uint32_t &sts0 = *(volatile uint32_t *)0x400CC0E0u;
    volatile uint32_t &root = *(volatile uint32_t *)0x40CC0A00u;   /* CLOCK_ROOT20 = FLEXSPI1 */
    __disable_irq();
    while ((sts0 & 3u) != 3u) { }                 /* sequence and arbitration idle */
    mcr0 |= 2u;                                   /* MDIS */
    root = (root & ~0xFFu) | 8u;                  /* same source, /9: 528 / 9 = 58.7 MHz */
    for (volatile int i = 0; i < 2000; i++) { }
    mcr0 &= ~(3u << 4);                           /* RXCLKSRC 0: internal loopback */
    mcr0 &= ~2u;
    mcr0 |= 1u;                                   /* SWRESET */
    while (mcr0 & 1u) { }
    __enable_irq();
}
#endif

bool board_sd_begin()
{
#if defined(ARDUINO_NOSH)
    flash_release_dqs_pad();
#endif
    return SD.begin(BUILTIN_SDCARD);
}
Storage &board_storage() { return g_storage; }
static void *mem_alloc(size_t n) { return extmem_malloc(n); }
static void mem_release(void *p) { extmem_free(p); }
LoaderMem board_mem() { return LoaderMem{mem_alloc, mem_release}; }
uint32_t board_now_us() { return micros(); }
