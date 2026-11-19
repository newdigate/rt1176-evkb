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

/* Nosh shares GPIO_SD_B2_05 between uSDHC2 CMD and the flash's DQS-pad loopback;
 * the core's Nosh boot header (bootdata.c) keeps the flash on the internal
 * loopback, so taking the pad here is safe.  This firmware runs code from flash. */
bool board_sd_begin() { return SD.begin(BUILTIN_SDCARD); }
Storage &board_storage() { return g_storage; }
static void *mem_alloc(size_t n) { return extmem_malloc(n); }
static void mem_release(void *p) { extmem_free(p); }
LoaderMem board_mem() { return LoaderMem{mem_alloc, mem_release}; }
uint32_t board_now_us() { return micros(); }
