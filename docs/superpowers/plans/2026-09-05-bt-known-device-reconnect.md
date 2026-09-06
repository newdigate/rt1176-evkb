# BT Known-Device Reconnect (NEW-34, piece 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After a device has paired once, a later boot reaches AVDTP STREAMING against it with no inquiry and no pairing dance, from a bond persisted in the core's EEPROM emulation; a stale bond is detected, erased and replaced in the same attempt.

**Architecture:** A pure `BondTable` (four entries, most-recent first, `"BTBD"`+CRC-32 image) lives in `M2Radio/bt`; `BtLink` answers `Link_Key_Request` from it, upserts on `Link_Key_Notification`, and gains a stored-key rung at the top of `pairAndEncrypt()`'s ladder; `A2dpSource::connect()` pages bonded candidates first and falls back to today's inquiry. A header-only `BondStoreEeprom` persists the image at EEPROM offset 4000; the two hosts (`bt_tone_test`, `acid_box`) load it after `Hci::begin()` and save after every `connect()`. One new QEMU gate, `m2_hci_probe[reconnect]`, runs three connects in one boot against a `reconnect` phase of `hci_peer.py`.

**Tech Stack:** C++11 (Arduino-free in `bt/`, host-tested with `c++`), the imxrt1176 core's `<avr/eeprom.h>` emulation, Python 3 fake controller, POSIX sh gate scripts over `tools/gate-lib.sh` + `tools/qrun`, ARM GCC 10 via CMake.

**Spec:** `docs/superpowers/specs/2026-09-05-bt-known-device-reconnect-design.md` (read it first; every design decision below is justified there).

---

## Conventions for every task

- **Two repos.** M2Radio is `~/Development/M2Radio` (its own git repo, pinned by `evkb.cmake` in this tree; local-first resolution means the evkb builds pick up your uncommitted M2Radio edits). evkb is `~/Development/rt1170/evkb`. Commit each repo separately; the M2Radio pin bump is Task 14.
- **Host tests:** `sh ~/Development/M2Radio/bt/test/run.sh` runs every bt suite and prints `BT-HOST-TESTS: PASS`. Each suite prints `<name>: N checks, M failures`.
- **A probe variant build:** `cd ~/Development/rt1170/evkb/examples/networking/m2_hci_probe && cmake -S . -B build-reconnect -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RECONNECT=ON && cmake --build build-reconnect`. The gate script does this itself; the command is for hand builds.
- **Gates:** run as `./run_qemu_<x>.sh` from the example directory, never `sh run_qemu_<x>.sh`. One QEMU run at a time on the machine.
- **Commit messages** end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- **Byte layouts** are Core 5.2 Vol 4 Part E: Link_Key_Request_Reply is opcode `0x040B`, params `BD_ADDR(6) Link_Key(16)`, answered by Command Complete `Status(1) BD_ADDR(6)`; Authentication_Complete is event `0x06`, params `Status(1) Handle(2)`; status `0x05` = Authentication Failure, `0x06` = PIN or Key Missing.

## File structure

M2Radio (`~/Development/M2Radio`):
- `bt/BondTable.h`, `bt/BondTable.cpp` — NEW. The table, its recency semantics, the image codec, CRC-32. Pure.
- `bt/BondStoreEeprom.h` — NEW, header-only. The EEPROM persistence (Arduino-side).
- `bt/BtLink.h`, `bt/BtLink.cpp` — `setBonds()`, `page()` (extracted from `connect()`), the two key handlers, the stored-key rung, `pairedBy()` → `"stored"`.
- `bt/A2dpSource.h`, `bt/A2dpSource.cpp` — `setBonds()`, its own log seam, the bonded-candidate walk.
- `bt/test/bondtable_test.cpp` — NEW. `bt/test/btlink_test.cpp` — five new arms. `bt/test/run.sh` — test list.

evkb (`~/Development/rt1170/evkb`):
- `examples/networking/m2_hci_probe/hci_peer.py` — the `reconnect` phase.
- `examples/networking/m2_hci_probe/CMakeLists.txt` — the `M2_BT_RECONNECT` option.
- `examples/networking/m2_hci_probe/m2_hci_probe.cpp` — `probeReconnect()`; exclusive event forwarding; no `probeInquiry()` under the new define.
- `examples/networking/m2_hci_probe/run_qemu_reconnect.sh` — NEW gate. `transcript_qemu_reconnect.txt` — NEW fixture.
- `tools/gate-vacuity.test.sh` — three negative cases.
- `examples/audio/bt_tone_test/bt_tone_test.cpp`, `CMakeLists.txt` — host wiring + `M2_BT_FORGET_BONDS`.
- `examples/display/acid_box/acid_box.cpp`, `CMakeLists.txt` — host wiring + knob + `BondTable.cpp.obj` in the flash-routing list.
- `tools/esp32-a2dp-sink/esp32-a2dp-sink.ino` — the `forget` serial command.
- `CLAUDE.md`, `evkb.cmake` — gate count 129, M2Radio pin.

---

### Task 1: `BondTable` (M2Radio, TDD)

**Files:**
- Create: `~/Development/M2Radio/bt/test/bondtable_test.cpp`
- Modify: `~/Development/M2Radio/bt/test/run.sh:5`
- Create: `~/Development/M2Radio/bt/BondTable.h`
- Create: `~/Development/M2Radio/bt/BondTable.cpp`

- [ ] **Step 1: Write the failing test**

Create `~/Development/M2Radio/bt/test/bondtable_test.cpp`:

```cpp
// Host tests for BondTable (NEW-34 piece 1): the image codec, the recency order and the
// bond semantics BtLink relies on.  Pure; no Hci.
#include "BondTable.h"
#include <stdio.h>
#include <string.h>
static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
// A bond at AA:BB:CC:DD:EE:<last> (little-endian on the wire, like every bd[] in hci/ and bt/)
// with a key that ramps from k0.
static Bond mk(uint8_t last, uint8_t k0, const char *name, uint8_t type = 4, uint8_t psrm = 1) {
    Bond b; memset(&b, 0, sizeof b);
    const uint8_t base[6] = { last, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA }; memcpy(b.bd, base, 6);
    for (int i = 0; i < 16; i++) b.key[i] = (uint8_t)(k0 + i);
    b.keyType = type; b.psrm = psrm; BondTable::copyName(b.name, name); return b;
}
static void fixCrc(uint8_t *img) {   // recompute the trailing CRC so ONLY the field under test is wrong
    uint32_t c = BondTable::crc32(img, BondTable::IMAGE_SIZE - 4);
    img[230] = (uint8_t)c; img[231] = (uint8_t)(c >> 8); img[232] = (uint8_t)(c >> 16); img[233] = (uint8_t)(c >> 24);
}
int main() {
    {   // 1. CRC-32 is the IEEE one (reflected 0xEDB88320, init/xorout 0xFFFFFFFF): the standard check value.
        CHECK(BondTable::crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
        CHECK(BondTable::crc32(nullptr, 0) == 0x00000000u);
    }
    {   // 2. Layout: the image depends on a 56-byte Bond and a 234-byte image.
        CHECK(sizeof(Bond) == 56);
        CHECK(BondTable::IMAGE_SIZE == 234);
    }
    {   // 3. Empty table: save/load round trip, not dirty, count 0; a too-small buffer is refused.
        BondTable t; uint8_t img[BondTable::IMAGE_SIZE];
        CHECK(t.count() == 0 && !t.dirty());
        CHECK(t.save(img, sizeof img) == BondTable::IMAGE_SIZE);
        CHECK(memcmp(img, "BTBD", 4) == 0 && img[4] == 1 && img[5] == 0);
        BondTable u; CHECK(u.load(img, sizeof img) && u.count() == 0 && !u.dirty());
        CHECK(t.save(img, 10) == 0);
    }
    {   // 4. upsert: most recent at index 0; an update moves to the front and replaces the key; find/at agree.
        BondTable t;
        t.upsert(mk(0x01, 0x10, "FAKE-HEADSET-01")); t.upsert(mk(0x02, 0x20, "OpenMove by Shokz"));
        CHECK(t.count() == 2 && t.dirty());
        CHECK(t.at(0).bd[0] == 0x02 && t.at(1).bd[0] == 0x01);
        const Bond *f = t.find(mk(0x01, 0, "").bd);
        CHECK(f && f->key[0] == 0x10 && strcmp(f->name, "FAKE-HEADSET-01") == 0 && f->keyType == 4 && f->psrm == 1);
        t.upsert(mk(0x01, 0x30, "FAKE-HEADSET-01"));
        CHECK(t.count() == 2 && t.at(0).bd[0] == 0x01 && t.at(0).key[0] == 0x30 && t.at(1).bd[0] == 0x02);
        CHECK(t.find(mk(0x09, 0, "").bd) == nullptr);
    }
    {   // 5. Eviction: the fifth bond evicts the LAST (least recent) entry.
        BondTable t;
        for (uint8_t i = 1; i <= 4; i++) t.upsert(mk(i, i, "n"));
        CHECK(t.count() == 4 && t.at(3).bd[0] == 1);
        t.upsert(mk(5, 5, "n"));
        CHECK(t.count() == 4 && t.at(0).bd[0] == 5 && t.at(3).bd[0] == 2 && t.find(mk(1, 0, "").bd) == nullptr);
    }
    {   // 6. touch / erase / dirty semantics.
        BondTable t;
        t.upsert(mk(1, 1, "a")); t.upsert(mk(2, 2, "b")); t.upsert(mk(3, 3, "c")); t.clearDirty();
        t.touch(mk(3, 0, "").bd);   CHECK(!t.dirty());                                   // already at the front: nothing changed
        t.touch(mk(1, 0, "").bd);   CHECK(t.dirty() && t.at(0).bd[0] == 1 && t.at(1).bd[0] == 3 && t.at(2).bd[0] == 2);
        t.clearDirty();
        CHECK(!t.erase(mk(9, 0, "").bd) && !t.dirty());
        CHECK(t.erase(mk(3, 0, "").bd) && t.dirty() && t.count() == 2 && t.at(0).bd[0] == 1 && t.at(1).bd[0] == 2);
        t.touch(mk(9, 0, "").bd);   CHECK(t.count() == 2);                              // absent: no-op
    }
    {   // 7. Round trip with content; every bad image loads FALSE AND EMPTY (stale RAM entries gone too).
        BondTable t; t.upsert(mk(1, 0x40, "OpenMove by Shokz", 4, 1)); t.upsert(mk(2, 0x50, "EVKB-SINK", 0, 2));
        uint8_t img[BondTable::IMAGE_SIZE]; CHECK(t.save(img, sizeof img) == BondTable::IMAGE_SIZE);
        BondTable u; CHECK(u.load(img, sizeof img) && u.count() == 2 && !u.dirty());
        CHECK(u.at(0).bd[0] == 2 && u.at(0).keyType == 0 && u.at(0).psrm == 2 && strcmp(u.at(0).name, "EVKB-SINK") == 0);
        CHECK(u.at(1).bd[0] == 1 && memcmp(u.at(1).key, t.at(1).key, 16) == 0);
        uint8_t bad[BondTable::IMAGE_SIZE];
        BondTable v;
        memcpy(bad, img, sizeof bad); bad[6 + 20] ^= 0x01;                      // one key byte flipped, CRC now wrong
        v.upsert(mk(7, 7, "stale")); CHECK(!v.load(bad, sizeof bad) && v.count() == 0);
        memcpy(bad, img, sizeof bad); v.upsert(mk(7, 7, "stale")); CHECK(!v.load(bad, BondTable::IMAGE_SIZE - 1) && v.count() == 0);   // truncated
        memcpy(bad, img, sizeof bad); bad[0] = 'X'; fixCrc(bad);              CHECK(!v.load(bad, sizeof bad) && v.count() == 0);     // magic
        memcpy(bad, img, sizeof bad); bad[4] = 2;   fixCrc(bad);              CHECK(!v.load(bad, sizeof bad) && v.count() == 0);     // version
        memcpy(bad, img, sizeof bad); bad[5] = 5;   fixCrc(bad);              CHECK(!v.load(bad, sizeof bad) && v.count() == 0);     // count > MAX
        uint8_t ff[BondTable::IMAGE_SIZE]; memset(ff, 0xFF, sizeof ff);      CHECK(!v.load(ff, sizeof ff) && v.count() == 0);       // a fresh part
        memset(ff, 0x00, sizeof ff);                                          CHECK(!v.load(ff, sizeof ff) && v.count() == 0);       // a QEMU run
        CHECK(!v.load(nullptr, 0) && v.count() == 0);
    }
    {   // 8. Names: truncated to 31 characters, always terminated; an EMPTY name on update keeps the old one
        //    (the notification handler may not know the name of a device it did not inquire this boot).
        BondTable t; Bond b = mk(1, 1, "");
        BondTable::copyName(b.name, "0123456789012345678901234567890123456789");   // 40 chars
        CHECK(strlen(b.name) == 31 && b.name[31] == 0);
        t.upsert(b);
        t.upsert(mk(1, 2, ""));
        CHECK(strlen(t.at(0).name) == 31 && t.at(0).key[0] == 2);
    }
    printf("bondtable_test: %d checks, %d failures\n", g_checks, g_fails); return g_fails ? 1 : 0;
}
```

- [ ] **Step 2: Add the suite to the runner and run it to verify it fails**

In `~/Development/M2Radio/bt/test/run.sh` change line 5 from
`for t in l2cap_test avdtp_test sdp_test btlink_test sbc_test rtp_test mediapacketizer_test; do`
to
`for t in bondtable_test l2cap_test avdtp_test sdp_test btlink_test sbc_test rtp_test mediapacketizer_test; do`

Run: `sh ~/Development/M2Radio/bt/test/run.sh`
Expected: compile error `fatal error: 'BondTable.h' file not found` (the runner stops on the first suite).

- [ ] **Step 3: Write the header**

Create `~/Development/M2Radio/bt/BondTable.h`:

```cpp
// BondTable -- the bonded-device table for BtLink: up to MAX peers, most recently
// used first, each with its BR/EDR link key.  Pure C++11, no heap, no Arduino; the
// image codec (save/load) is what a host-side store persists (BondStoreEeprom.h).
// NEW-34 piece 1 -- docs/superpowers/specs/2026-09-05-bt-known-device-reconnect-design.md.  MIT.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct Bond {
    uint8_t bd[6];        // as on the wire (little-endian), like every bd[] in hci/ and bt/
    uint8_t key[16];      // the link key from Link_Key_Notification
    uint8_t keyType;      // Link_Key_Notification Key_Type: 0 = legacy combination, 4 = unauthenticated P-192 (Just Works)
    uint8_t psrm;         // Page_Scan_Repetition_Mode we paged with -- Create_Connection needs it without an inquiry
    char    name[32];     // the inquiry's remote name, truncated to 31 chars + NUL (NEW-35's list; the boot policy's name filter)
};

class BondTable {
public:
    static const uint8_t  MAX = 4;
    static const uint8_t  NAME_MAX = 31;
    static const uint8_t  VERSION = 1;
    static const uint16_t IMAGE_SIZE = 4 + 1 + 1 + MAX * 56 + 4;   // "BTBD" ver count entries crc32 = 234

    BondTable();
    void clear();                                        // empty the table; leaves dirty alone
    const Bond *find(const uint8_t bd[6]) const;
    // Insert at the front; an existing entry is updated (an empty new name keeps the old one)
    // and moved to the front; the LAST entry is evicted when full.  Sets dirty.
    void upsert(const Bond &b);
    void touch(const uint8_t bd[6]);                     // move to the front; dirty only if it moved; no-op when absent
    bool erase(const uint8_t bd[6]);                     // sets dirty when it removed something
    uint8_t count() const { return m_count; }
    const Bond &at(uint8_t i) const { return m_b[i]; }   // 0 = most recent; i < count()
    uint16_t save(uint8_t *out, uint16_t cap) const;     // serialise; IMAGE_SIZE, or 0 when cap < IMAGE_SIZE
    // Deserialise.  False AND an EMPTY table on any bad magic/version/count/length/CRC -- a caller
    // can never keep stale RAM entries by mistake.  Clears dirty either way (RAM now == store).
    bool load(const uint8_t *in, uint16_t len);
    bool dirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }
    static uint32_t crc32(const uint8_t *p, size_t n);   // IEEE 802.3, reflected, init/xorout 0xFFFFFFFF ("123456789" -> 0xCBF43926)
    static void copyName(char out[32], const char *in);  // truncating copy, NUL-terminated, zero-padded tail
private:
    int  indexOf(const uint8_t bd[6]) const;
    void moveToFront(int i);
    Bond    m_b[MAX];
    uint8_t m_count;
    bool    m_dirty;
};
```

- [ ] **Step 4: Write the implementation**

Create `~/Development/M2Radio/bt/BondTable.cpp`:

```cpp
#include "BondTable.h"
#include <string.h>

static_assert(sizeof(Bond) == 56, "Bond must be 56 bytes with no padding: the image layout depends on it");
static const uint8_t MAGIC[4] = { 'B', 'T', 'B', 'D' };

BondTable::BondTable() : m_count(0), m_dirty(false) { memset(m_b, 0, sizeof m_b); }

uint32_t BondTable::crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

void BondTable::copyName(char out[32], const char *in) {
    size_t n = 0;
    if (in) while (n < NAME_MAX && in[n]) { out[n] = in[n]; n++; }
    for (size_t i = n; i < 32; i++) out[i] = 0;          // terminator + a zero tail, so equal tables give equal images
}

void BondTable::clear() { memset(m_b, 0, sizeof m_b); m_count = 0; }

int BondTable::indexOf(const uint8_t bd[6]) const {
    for (uint8_t i = 0; i < m_count; i++) if (memcmp(m_b[i].bd, bd, 6) == 0) return (int)i;
    return -1;
}

const Bond *BondTable::find(const uint8_t bd[6]) const { int i = indexOf(bd); return i < 0 ? nullptr : &m_b[i]; }

void BondTable::moveToFront(int i) {
    if (i <= 0) return;
    Bond t = m_b[i];
    memmove(&m_b[1], &m_b[0], sizeof(Bond) * (size_t)i);
    m_b[0] = t;
}

void BondTable::upsert(const Bond &b) {
    Bond nb = b; copyName(nb.name, b.name);              // normalise: terminator + zero tail
    int i = indexOf(b.bd);
    if (i < 0) {
        if (m_count < MAX) m_count++;                    // else the last entry is evicted by the shift below
        memmove(&m_b[1], &m_b[0], sizeof(Bond) * (size_t)(m_count - 1));
        m_b[0] = nb;
    } else {
        if (nb.name[0] == 0) memcpy(nb.name, m_b[i].name, 32);   // an update without a name keeps the old one
        m_b[i] = nb; moveToFront(i);
    }
    m_dirty = true;
}

void BondTable::touch(const uint8_t bd[6]) {
    int i = indexOf(bd);
    if (i > 0) { moveToFront(i); m_dirty = true; }
}

bool BondTable::erase(const uint8_t bd[6]) {
    int i = indexOf(bd);
    if (i < 0) return false;
    memmove(&m_b[i], &m_b[i + 1], sizeof(Bond) * (size_t)(m_count - 1 - i));
    m_count--; memset(&m_b[m_count], 0, sizeof(Bond)); m_dirty = true;
    return true;
}

uint16_t BondTable::save(uint8_t *out, uint16_t cap) const {
    if (cap < IMAGE_SIZE) return 0;
    memcpy(out, MAGIC, 4); out[4] = VERSION; out[5] = m_count;
    memcpy(out + 6, m_b, sizeof m_b);
    uint32_t c = crc32(out, IMAGE_SIZE - 4);
    out[IMAGE_SIZE - 4] = (uint8_t)c; out[IMAGE_SIZE - 3] = (uint8_t)(c >> 8);
    out[IMAGE_SIZE - 2] = (uint8_t)(c >> 16); out[IMAGE_SIZE - 1] = (uint8_t)(c >> 24);
    return IMAGE_SIZE;
}

bool BondTable::load(const uint8_t *in, uint16_t len) {
    clear(); m_dirty = false;
    if (!in || len < IMAGE_SIZE) return false;
    if (memcmp(in, MAGIC, 4) != 0 || in[4] != VERSION || in[5] > MAX) return false;
    uint32_t want = (uint32_t)in[IMAGE_SIZE - 4] | ((uint32_t)in[IMAGE_SIZE - 3] << 8)
                  | ((uint32_t)in[IMAGE_SIZE - 2] << 16) | ((uint32_t)in[IMAGE_SIZE - 1] << 24);
    if (crc32(in, IMAGE_SIZE - 4) != want) return false;
    memcpy(m_b, in + 6, sizeof m_b); m_count = in[5];
    for (uint8_t i = 0; i < MAX; i++) m_b[i].name[NAME_MAX] = 0;   // never trust an image's terminator
    return true;
}
```

- [ ] **Step 5: Run the suites to verify they pass**

Run: `sh ~/Development/M2Radio/bt/test/run.sh`
Expected: a line `bondtable_test: 47 checks, 0 failures` (the count is whatever the file yields; the failures must be 0), every other suite unchanged, and `BT-HOST-TESTS: PASS`.

- [ ] **Step 6: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add bt/BondTable.h bt/BondTable.cpp bt/test/bondtable_test.cpp bt/test/run.sh && git commit -m "feat(bt): BondTable -- four-entry bonded-device table, recency order, BTBD+CRC-32 image codec (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: `BondStoreEeprom.h` (M2Radio, header-only)

**Files:**
- Create: `~/Development/M2Radio/bt/BondStoreEeprom.h`

No host test is possible (it needs the core's `<avr/eeprom.h>`); Task 7's probe build is its compile check and Task 8's gate is its behavioural check.

- [ ] **Step 1: Write the header**

```cpp
// BondStoreEeprom -- persists a BondTable in the core's EEPROM emulation (imxrt1176: the
// top 256 KB of the FlexSPI NOR, 4284 bytes, journaled -- cores/imxrt1176/eeprom.c).
// Header-only ON PURPOSE: bt/test/run.sh compiles every bt/*.cpp into each host suite,
// and this file needs <avr/eeprom.h>.  Hosts: load() once after Hci::begin(); save()
// after every A2dpSource::connect() return (success or failure -- an erased stale bond
// must persist too).  Every write happens outside streaming, since connect() returns
// before media starts.  NEW-34 piece 1.  MIT.
#pragma once
#include <avr/eeprom.h>
#include "BondTable.h"

struct BondStoreEeprom {
    static const uint16_t OFFSET = 4000;                       // 4000 + 234 = 4234 <= 4284 (E2END 0x10BB)
    static_assert(OFFSET + BondTable::IMAGE_SIZE <= (uint16_t)E2END + 1u, "bond image does not fit the emulated EEPROM");
    // Read the image and load the table.  False (and an EMPTY table) on a fresh part (0xFF),
    // a QEMU run (0x00), or anything storage-memory/eeprom_test scribbled over -- never a stale bond.
    static bool load(BondTable &t) {
        uint8_t img[BondTable::IMAGE_SIZE];
        eeprom_read_block(img, (const void *)(uintptr_t)OFFSET, sizeof img);
        return t.load(img, sizeof img);
    }
    // Write the image if the table changed.  eeprom_write_byte skips a byte the emulation
    // already holds, so an unchanged image costs no flash writes; a first pairing changes
    // ~60 bytes, a move-to-front up to the whole 224-byte body.
    static bool save(BondTable &t) {
        if (!t.dirty()) return false;
        uint8_t img[BondTable::IMAGE_SIZE];
        t.save(img, sizeof img);
        eeprom_write_block(img, (void *)(uintptr_t)OFFSET, sizeof img);
        t.clearDirty();
        return true;
    }
    // Bench knob (M2_BT_FORGET_BONDS): zero the image and empty the table.
    static void wipe(BondTable &t) {
        uint8_t z[BondTable::IMAGE_SIZE];
        for (uint16_t i = 0; i < sizeof z; i++) z[i] = 0;
        eeprom_write_block(z, (void *)(uintptr_t)OFFSET, sizeof z);
        t.clear(); t.clearDirty();
    }
};
```

- [ ] **Step 2: Confirm the host suites still build (the header is not compiled by them, but the glob must stay clean)**

Run: `sh ~/Development/M2Radio/bt/test/run.sh`
Expected: `BT-HOST-TESTS: PASS`.

- [ ] **Step 3: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add bt/BondStoreEeprom.h && git commit -m "feat(bt): BondStoreEeprom -- header-only persistence of the bond image at EEPROM offset 4000 (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: `BtLink::page()` extraction (M2Radio, pure refactor)

**Files:**
- Modify: `~/Development/M2Radio/bt/BtLink.h`
- Modify: `~/Development/M2Radio/bt/BtLink.cpp` (the `connect()` function, lines 77-200 of the current file)

The existing `btlink_test` (six arms) must stay green unchanged: its `preamble()` counts every command, so a behavioural drift in the refactor fails there.

- [ ] **Step 1: Replace `bt/BtLink.h` with this complete file**

```cpp
// BtLink -- one BR/EDR ACL link: inquiry by name, Create_Connection, SSP
// pairing with legacy-PIN fallback, encryption -- and, with a BondTable set,
// a stored-key reconnect (NEW-34 piece 1).  Blocking helpers for setup();
// the SSP/PIN/link-key events are answered by onEvent() (submit only, no run()).
// Arduino-free: the clock (now/idle) and the console (LogFn) are injected.
#pragma once
#include <stdint.h>
#include "Hci.h"
#include "BondTable.h"
class BtLink {
public:
    enum Result : uint8_t { OK = 0, NO_INQUIRY_HIT, CONNECT_STATUS, PAIRING_FAILED, PIN_FAILED, ENCRYPTION_FAILED, TIMEOUT };
    static const char *resultName(Result r);
    typedef void (*LogFn)(void *ctx, const char *line);
    explicit BtLink(Hci &hci) : m_hci(hci) {}
    void setLog(LogFn fn, void *ctx) { m_log = fn; m_logCtx = ctx; }
    void setPin(const char *pin4) { for (int i = 0; i < 4; i++) m_pin[i] = pin4[i]; }
    // Force legacy PIN pairing: connect() writes Write_Simple_Pairing_Mode=0 up
    // front so the link is legacy from the start and pairAndEncrypt()'s first (and
    // only) Authentication_Requested takes the PIN_Code_Request path -- NO SSP
    // attempt.  REQUIRED for the IW416<->ESP32 sink: their SSP stalls ~25 s at the
    // LMP IO-cap exchange and then poisons the SSP-fail->PIN fallback on the same
    // link (measured on silicon 2026-09-03: auth_complete=0x0C, secure=pairing_failed).
    void setLegacyPin(bool v) { m_legacyPin = v; }
    // Bonded devices (NEW-34 piece 1).  Null (the default) = today's behaviour exactly:
    // negative link-key replies, nothing stored.  With a table: Link_Key_Request is
    // answered from it, Link_Key_Notification upserts into it, and a key the peer rejects
    // (Authentication_Complete 0x05/0x06) is erased before pairing afresh on the same link.
    // The table's dirty flag is the host's cue to persist (BondStoreEeprom::save).
    void setBonds(BondTable *t) { m_bonds = t; }
    BondTable *bonds() { return m_bonds; }
    // now() = a millisecond clock; idle() = pump the HCI + yield (the app passes millis and its idleMs).
    Result connect(const char *nameSubstr, uint32_t (*now)(), void (*idle)());   // inquiry (~10 s) -> page(hit, PAGE_ATTEMPTS)
    // Page ONE address directly -- no inquiry: Set_Event_Mask, Write_Simple_Pairing_Mode,
    // Write_Page_Timeout, then Create_Connection up to `attempts` times (cancel-a-silent-page,
    // retry on Page Timeout).  connect() calls it for its inquiry hit (clk from the hit, valid);
    // A2dpSource calls it for each bonded candidate (clk 0, invalid: a stored offset is stale).
    // `name` (nullable) is remembered for the bond a later Link_Key_Notification creates.
    Result page(const uint8_t bd[6], uint8_t psrm, uint16_t clk, bool clkValid, const char *name,
                uint8_t attempts, uint32_t (*now)(), void (*idle)());
    Result pairAndEncrypt(uint32_t (*now)(), void (*idle)());                     // stored key first (if offered), then SSP, then legacy PIN
    // HCI_Disconnect (reason 0x13, remote user terminated) and wait for Disconnection_Complete.
    // OK when there is no link.  A2dpSource calls it on every post-connect failure so a retry
    // starts from a clean controller state instead of paging a device we are still linked to.
    Result disconnect(uint32_t (*now)(), void (*idle)());
    static const uint8_t PAGE_ATTEMPTS = 3;   // Create_Connection tries per connect(): a headset just out of pairing mode misses a page
    void onEvent(uint8_t code, const uint8_t *p, uint8_t len);   // forward from the app's Hci::EventFn
    uint16_t handle() const { return m_handle; } const uint8_t *peer() const { return m_bd; }
    bool encrypted() const { return m_encrypted; }
    const char *pairedBy() const { return m_pairedBy; }          // "none" | "ssp" | "pin" | "stored"
private:
    void logf(const char *fmt, ...);                            // vsnprintf into m_lb; emit via m_log if set
    Hci &m_hci; LogFn m_log = nullptr; void *m_logCtx = nullptr; char m_lb[320];
    volatile uint16_t m_handle = 0;
    // non-volatile: published to readers under the same idle()-call memory barrier as the volatile scalars;
    // volatile on an array copied via memcpy is inert anyway
    uint8_t m_bd[6] = {0};
    volatile uint8_t m_psrm = 0; volatile uint16_t m_clk = 0;
    char m_pin[4] = {'1','2','3','4'}; const char *m_pairedBy = "none";
    bool m_legacyPin = false;
    BondTable *m_bonds = nullptr;
    volatile bool m_keyOffered = false;      // this authentication was answered with a STORED key
    char m_pageName[32] = {0};               // the name page() was given, for the bond a notification creates
    volatile bool m_connDone = false, m_authDone = false, m_pairDone = false, m_encDone = false;
    volatile uint8_t m_connStatus = 0xFF, m_authStatus = 0xFF, m_pairStatus = 0xFF, m_encStatus = 0xFF;
    volatile bool m_encrypted = false; volatile bool m_haveLinkKey = false;
    volatile bool m_discDone = false; volatile uint8_t m_discReason = 0;
    volatile bool m_inqComplete = false;
    // A/V inquiry hits (major device class 0x04), enough for the bench.  `named` is
    // per-hit (not a single shared flag) so a late Remote_Name_Complete for hit i
    // can never be mistaken for hit i+1's answer while connect() waits on it.
    struct Hit { uint8_t bd[6]; uint32_t cod; uint8_t psrm; uint16_t clk; volatile bool named; uint8_t nameStatus; char name[249]; };
    static const uint8_t MAX_HITS = 8; Hit m_hits[MAX_HITS]; uint8_t m_nHits = 0; int m_target = -1;
};
```

- [ ] **Step 2: In `bt/BtLink.cpp`, replace `connect()` (from the comment `// --- connect(): OP_INQUIRY -> field-major Inquiry Result parse` through the closing brace before `// --- pairAndEncrypt()`) with these two functions**

```cpp
// --- connect(): OP_INQUIRY -> field-major Inquiry Result parse -> per-hit
// Remote_Name_Request -> choose the target -> page().  Ported from probeInquiry()
// + the first half of probeConnect(). ---
BtLink::Result BtLink::connect(const char *nameSubstr, uint32_t (*now)(), void (*idle)()) {
    m_nHits = 0; m_target = -1;
    m_inqComplete = false;
    // LAP = GIAC 0x9E8B33 little-endian, Inquiry_Length 0x0A = 12.8 s, Num_Responses 0 = unlimited
    const uint8_t params[5] = { 0x33, 0x8B, 0x9E, 0x0A, 0x00 };
    Hci::Reply r;
    Hci::Error e = m_hci.run(OP_INQUIRY, params, sizeof params, &r, 1000, idle);
    if (e != Hci::OK || !r.statusEvent) {
        logf("inquiry=fail reason=%s status=0x%02X", e == Hci::OK ? "not_command_status" : Hci::errorName(e), r.status);
        return TIMEOUT;
    }
    logf("inquiry=started");
    uint32_t t0 = now();
    while (!m_inqComplete && now() - t0 < 15000) idle();     // events arrive via onEvent()
    logf("inquiry_complete: n=%u%s", m_nHits, m_inqComplete ? "" : " timeout=1");

    for (uint8_t i = 0; i < m_nHits; i++) {
        Hit &h = m_hits[i];
        // Remote_Name_Request: BD_ADDR(6) Page_Scan_Repetition_Mode(1) Reserved(1) Clock_Offset(2, bit15=valid)
        uint8_t p[10];
        memcpy(p, h.bd, 6);
        p[6] = h.psrm; p[7] = 0;
        p[8] = (uint8_t)(h.clk & 0xFF);
        p[9] = (uint8_t)((h.clk >> 8) | 0x80);
        h.named = false;
        Hci::Error ne = m_hci.run(OP_REMOTE_NAME_REQ, p, sizeof p, &r, 1000, idle);
        t0 = now();
        // Wait on THIS hit's own flag -- a late Remote_Name_Complete for an
        // earlier hit must never be able to end an unrelated hit's wait (the
        // shared-flag race this replaced).
        while (ne == Hci::OK && !h.named && now() - t0 < 5000) idle();
        char bs[18]; hciFormatBd(h.bd, bs);
        if (ne != Hci::OK)  { logf("inq_name: bd=%s fail reason=%s", bs, Hci::errorName(ne)); continue; }
        if (!h.named)       { logf("inq_name: bd=%s fail reason=no_name_event", bs); continue; }
        logf("inq_name: bd=%s status=0x%02X name=\"%s\"", bs, h.nameStatus, h.name);
    }

    // Choose the target: first hit whose name contains nameSubstr, or (if
    // nameSubstr is null/empty) the first hit.
    if (nameSubstr && nameSubstr[0]) {
        for (uint8_t i = 0; i < m_nHits; i++)
            if (m_hits[i].named && strstr(m_hits[i].name, nameSubstr)) { m_target = (int)i; break; }
    } else if (m_nHits > 0) {
        m_target = 0;
    }
    if (m_target < 0) { logf("connect=fail reason=no_inquiry_hit"); return NO_INQUIRY_HIT; }

    Hit &d = m_hits[m_target];
    char tbs[18]; hciFormatBd(d.bd, tbs);
    logf("connect: target=%s name=\"%s\"", tbs, d.named ? d.name : "?");
    return page(d.bd, d.psrm, d.clk, true, d.named ? d.name : nullptr, PAGE_ATTEMPTS, now, idle);
}

// --- page(): the second half of the old connect(), with the target and the attempt
// count as parameters (NEW-34: A2dpSource pages bonded candidates through here). ---
BtLink::Result BtLink::page(const uint8_t bd[6], uint8_t psrm, uint16_t clk, bool clkValid, const char *name,
                            uint8_t attempts, uint32_t (*now)(), void (*idle)()) {
    memcpy(m_bd, bd, 6); m_psrm = psrm; m_clk = clk;
    BondTable::copyName(m_pageName, name);
    Hci::Reply r;
    uint32_t t0;

    // Enable ALL HCI events, incl. the SSP request events (0x31-0x36) which sit
    // ABOVE the post-Reset default mask -- without this the controller cannot
    // ask the host to run Simple Pairing.
    uint8_t evmask[8]; memset(evmask, 0xFF, sizeof evmask);
    Hci::Error me = m_hci.run(OP_SET_EVENT_MASK, evmask, sizeof evmask, &r, 1000, idle);
    logf("event_mask: st=%s status=0x%02X", me == Hci::OK ? "ok" : Hci::errorName(me), r.status);

    // SSP on by default; OFF when legacy PIN is forced, so the link is legacy from
    // the start (pairAndEncrypt()'s first Authentication_Requested then takes the
    // PIN_Code_Request path with no SSP attempt).  See setLegacyPin().
    uint8_t sspMode = m_legacyPin ? 0x00 : 0x01;
    Hci::Error we = m_hci.run(OP_WRITE_SSP_MODE, &sspMode, 1, &r, 1000, idle);
    logf("ssp_mode: st=%s status=0x%02X mode=%u", we == Hci::OK ? "ok" : Hci::errorName(we), r.status, sspMode);

    // Write_Page_Timeout 0x2000 slots (5.12 s -- the spec default, written EXPLICITLY so the
    // page either completes or reports Page Timeout inside the wait below, whatever the
    // firmware's own default).  Answered by Command Complete; a controller that lacks it is
    // logged and tolerated.
    uint8_t pt[2] = { 0x00, 0x20 };
    Hci::Error pe = m_hci.run(OP_WRITE_PAGE_TIMEOUT, pt, 2, &r, 1000, idle);
    logf("page_timeout: st=%s status=0x%02X slots=0x2000", pe == Hci::OK ? "ok" : Hci::errorName(pe), r.status);

    // Create_Connection: bd(6) pkt_type(2)=0xCC18 psrm(1) reserved(1) clk(2,bit15=valid) role_switch(1)
    // role_switch=0x00 (NOT allowed): the Mac pages this headset that way (PacketLogger reference
    // 2026-09-03: ... 18 CC 01 00 54 88 00) and an A2DP source wants to stay master anyway.
    // Paged up to `attempts` times from here, WITHOUT a fresh inquiry: a headset that has just
    // left pairing mode, or is asleep between page scans, misses a page and answers the next.
    for (uint8_t attempt = 1; attempt <= attempts; attempt++) {
        uint8_t p[13];
        memcpy(p, m_bd, 6);
        p[6] = 0x18; p[7] = 0xCC;
        p[8] = m_psrm; p[9] = 0x00;
        p[10] = (uint8_t)(m_clk & 0xFF);
        p[11] = (uint8_t)((m_clk >> 8) | (clkValid ? 0x80 : 0x00));
        p[12] = 0x00;    // no role switch
        m_connDone = false; m_connStatus = 0xFF;
        Hci::Error ce = m_hci.run(OP_CREATE_CONNECTION, p, sizeof p, &r, 2000, idle);
        if (ce != Hci::OK || !r.statusEvent) {
            logf("connect=fail reason=%s status=0x%02X attempt=%u", ce == Hci::OK ? "not_command_status" : Hci::errorName(ce), r.status, attempt);
            return TIMEOUT;
        }
        // Page Timeout is 5.12 s; a compliant controller has reported one way or the other well
        // inside 10 s.  Silence past that is the bench's "connect=timeout (no Connection_Complete)".
        t0 = now();
        while (!m_connDone && now() - t0 < 10000) idle();
        if (!m_connDone) {
            // The controller is (as far as we can tell) still paging, and may have withheld its
            // command credit for the duration (Num_HCI_Command_Packets=0 in the Command Status,
            // no NOP since).  Reclaim the credit if so -- otherwise the cancel below can never
            // leave and every later command starves by name, the wedge measured 2026-09-03 --
            // then CANCEL the page: the controller stops paging, its Command Complete re-reports
            // the true credit count, and it follows with a Connection_Complete (status 0x02).
            logf("connect=timeout (no Connection_Complete) attempt=%u ncmd=%u -> Create_Connection_Cancel", attempt, m_hci.ncmd());
            if (m_hci.ncmd() == 0) m_hci.reclaimCredit();
            Hci::Reply rc;
            Hci::Error xe = m_hci.run(OP_CREATE_CONN_CANCEL, m_bd, 6, &rc, 2000, idle);
            uint32_t t1 = now();
            while (xe == Hci::OK && !m_connDone && now() - t1 < 1000) idle();
            logf("connect_cancel: st=%s status=0x%02X conn_complete=%s", xe == Hci::OK ? "ok" : Hci::errorName(xe), rc.status,
                 m_connDone ? "seen" : "none");
            if (attempt == attempts) return TIMEOUT;
            continue;
        }
        if (m_connStatus == 0x04 && attempt < attempts) { logf("connect=page_timeout attempt=%u -> retry", attempt); continue; }
        if (m_connStatus != 0x00) { logf("connect=fail status=0x%02X attempt=%u", m_connStatus, attempt); return CONNECT_STATUS; }
        logf("connect=ok handle=0x%04X attempt=%u", (unsigned)m_handle, attempt);
        return OK;
    }
    return TIMEOUT;
}
```

Note one deliberate behavioural detail: the old code returned `TIMEOUT` when the LAST attempt reported Page Timeout 0x04 (it fell out of the loop); the new code returns `CONNECT_STATUS` for that case, because `attempt < attempts` is false and the next line reports it. Existing test 3 (Page Timeout then success) and test 1 (silence) are unaffected. Keep the old behaviour instead if any existing arm depends on it: change the 0x04 line to `if (m_connStatus == 0x04) { logf(...); if (attempt == attempts) return TIMEOUT; continue; }`. Use THIS form (the old semantics) so no existing consumer sees a new result code:

```cpp
        if (m_connStatus == 0x04) { logf("connect=page_timeout attempt=%u%s", attempt, attempt < attempts ? " -> retry" : ""); if (attempt == attempts) return TIMEOUT; continue; }
```

- [ ] **Step 3: Run the host suites to verify the refactor is invisible**

Run: `sh ~/Development/M2Radio/bt/test/run.sh`
Expected: `btlink_test: 25 checks, 0 failures` (the pre-existing count), `BT-HOST-TESTS: PASS`.

- [ ] **Step 4: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add bt/BtLink.h bt/BtLink.cpp && git commit -m "refactor(bt): BtLink::page() -- the Create_Connection half of connect(), with target and attempt count as parameters (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: `BtLink` bonds: key reply, notification upsert, the stored-key rung (M2Radio, TDD)

**Files:**
- Modify: `~/Development/M2Radio/bt/test/btlink_test.cpp` (append five arms before the final `printf`)
- Modify: `~/Development/M2Radio/bt/BtLink.cpp` (the opcode enum, `pairAndEncrypt()`, two `onEvent()` branches)

- [ ] **Step 1: Write the failing tests**

In `bt/test/btlink_test.cpp`, add after the `connComplete()` helper (before `int main()`):

```cpp
static const uint8_t KEY1[16] = { 0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F };
static const uint8_t KEY2[16] = { 0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F };
static void seedBond(BondTable &t, const uint8_t *key, const char *name) {
    Bond b; memset(&b, 0, sizeof b); memcpy(b.bd, BD, 6); memcpy(b.key, key, 16); b.keyType = 4; b.psrm = 1;
    BondTable::copyName(b.name, name); t.upsert(b); t.clearDirty();
}
static std::vector<uint8_t> withBd(std::vector<uint8_t> head, const std::vector<uint8_t> &prm) { head.insert(head.end(), prm.begin(), prm.begin() + 6); return head; }
// The SSP Just-Works dance a controller runs after a NEGATIVE link-key reply, ending in a
// Link_Key_Notification carrying `key` and a successful Authentication_Complete.
static bool sspDance(FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm, const uint8_t *key) {
    std::vector<uint8_t> bd(BD, BD + 6);
    if (op == 0x040C) { f.cc(op, withBd({ 0x00 }, prm)); f.ev(0x31, bd); return true; }                        // -> IO_Capability_Request
    if (op == 0x042B) { f.cc(op, withBd({ 0x00 }, prm));
                        std::vector<uint8_t> rsp = bd; rsp.push_back(0x03); rsp.push_back(0x00); rsp.push_back(0x04); f.ev(0x32, rsp);
                        std::vector<uint8_t> uc = bd; uc.push_back(0x40); uc.push_back(0xE2); uc.push_back(0x01); uc.push_back(0x00); f.ev(0x33, uc); return true; }
    if (op == 0x042C) { f.cc(op, withBd({ 0x00 }, prm));
                        std::vector<uint8_t> spc = { 0x00 }; spc.insert(spc.end(), BD, BD + 6); f.ev(0x36, spc);
                        std::vector<uint8_t> lk = bd; lk.insert(lk.end(), key, key + 16); lk.push_back(0x04); f.ev(0x18, lk);
                        f.ev(0x06, { 0x00, 0x01, 0x00 }); return true; }
    return false;
}
```

Then add these arms inside `main()`, immediately before the final `printf("btlink_test: ...`:

```cpp
    {   // 7. NEW-34: a bonded page (no inquiry, clock offset invalid) + a stored-key authentication:
        //    Link_Key_Request is answered with Link_Key_Request_Reply carrying the EXACT stored key; no
        //    negative reply and no IO-capability dance follow; pairedBy() reads "stored"; encryption comes up.
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); hci.onEvent(evThunk, &link); link.setLog(logFn, nullptr); g_log.clear();
        BondTable bonds; seedBond(bonds, KEY1, "OpenMove by Shokz"); link.setBonds(&bonds);
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) {
            if (preamble(f, op, prm)) return;
            if (op == 0x0405) { f.cs(op); f.ev(0x03, connComplete(0x00)); return; }
            if (op == 0x0411) { f.cs(op); f.ev(0x17, std::vector<uint8_t>(BD, BD + 6)); return; }          // Link_Key_Request
            if (op == 0x040B) { f.cc(op, withBd({ 0x00 }, prm)); f.ev(0x06, { 0x00, 0x01, 0x00 }); return; }   // key matched -> Auth Complete ok
            if (op == 0x0413) { f.cs(op); f.ev(0x08, { 0x00, 0x01, 0x00, 0x01 }); return; }
            f.cc(op, { 0x01 });
        };
        CHECK(link.page(BD, 1, 0, false, "OpenMove by Shokz", 1, fakeNow, idle10) == BtLink::OK);
        CHECK(io.count(0x0401) == 0);                                                     // no inquiry
        const std::vector<uint8_t> *cc = io.last(0x0405);
        CHECK(cc && cc->size() == 13 && memcmp(cc->data(), BD, 6) == 0 && (*cc)[8] == 1 && (*cc)[10] == 0x00 && (*cc)[11] == 0x00);   // psrm R1, clock offset 0 / INVALID
        CHECK(link.pairAndEncrypt(fakeNow, idle10) == BtLink::OK);
        const std::vector<uint8_t> *kr = io.last(0x040B);
        CHECK(kr && kr->size() == 22 && memcmp(kr->data(), BD, 6) == 0 && memcmp(kr->data() + 6, KEY1, 16) == 0);
        CHECK(io.count(0x040B) == 1 && io.count(0x040C) == 0 && io.count(0x042B) == 0 && io.count(0x040D) == 0 && io.count(0x0411) == 1);
        CHECK(strcmp(link.pairedBy(), "stored") == 0 && link.encrypted());
        CHECK(bonds.count() == 1 && !bonds.dirty());                                      // touched, but already at the front
    }
    {   // 8. NEW-34: the peer REJECTS the stored key (Authentication_Complete 0x06, PIN or Key Missing): the bond
        //    is erased, ONE more Authentication_Requested runs with SSP still on, its Link_Key_Request now gets the
        //    negative reply, the SSP dance yields a NEW key, and the table holds that key under the paged name.
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); hci.onEvent(evThunk, &link); link.setLog(logFn, nullptr); g_log.clear();
        BondTable bonds; seedBond(bonds, KEY1, "OpenMove by Shokz"); link.setBonds(&bonds);
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) {
            if (preamble(f, op, prm)) return;
            if (op == 0x0405) { f.cs(op); f.ev(0x03, connComplete(0x00)); return; }
            if (op == 0x0411) { f.cs(op); f.ev(0x17, std::vector<uint8_t>(BD, BD + 6)); return; }
            if (op == 0x040B) { f.cc(op, withBd({ 0x00 }, prm)); f.ev(0x06, { 0x06, 0x01, 0x00 }); return; }   // PIN or Key Missing
            if (sspDance(f, op, prm, KEY2)) return;
            if (op == 0x0413) { f.cs(op); f.ev(0x08, { 0x00, 0x01, 0x00, 0x01 }); return; }
            f.cc(op, { 0x01 });
        };
        CHECK(link.page(BD, 1, 0, false, "OpenMove by Shokz", 1, fakeNow, idle10) == BtLink::OK);
        CHECK(link.pairAndEncrypt(fakeNow, idle10) == BtLink::OK);
        CHECK(io.count(0x0411) == 2 && io.count(0x040B) == 1 && io.count(0x040C) == 1 && io.count(0x042B) == 1);
        CHECK(io.count(0x0C56) == 1);                                                     // SSP mode written ONCE (by page): the SSP-off fallback did not run
        const Bond *b = bonds.find(BD);
        CHECK(b && memcmp(b->key, KEY2, 16) == 0 && b->keyType == 4 && b->psrm == 1 && strcmp(b->name, "OpenMove by Shokz") == 0 && bonds.dirty());
        CHECK(strcmp(link.pairedBy(), "ssp") == 0 && link.encrypted());
        bool sawReject = false; for (auto &l : g_log) if (l.find("bond_rejected: status=0x06 -> erased") != std::string::npos) sawReject = true;
        CHECK(sawReject);
    }
    {   // 9. NEW-34: a stored-key authentication that fails for a TRANSIENT reason (0x08 Connection Timeout) keeps
        //    the bond, runs no second Authentication_Requested and no PIN fallback, and fails by name.
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); hci.onEvent(evThunk, &link); link.setLog(logFn, nullptr); g_log.clear();
        BondTable bonds; seedBond(bonds, KEY1, "OpenMove by Shokz"); link.setBonds(&bonds);
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) {
            if (preamble(f, op, prm)) return;
            if (op == 0x0405) { f.cs(op); f.ev(0x03, connComplete(0x00)); return; }
            if (op == 0x0411) { f.cs(op); f.ev(0x17, std::vector<uint8_t>(BD, BD + 6)); return; }
            if (op == 0x040B) { f.cc(op, withBd({ 0x00 }, prm)); f.ev(0x06, { 0x08, 0x01, 0x00 }); return; }   // Connection Timeout
            f.cc(op, { 0x01 });
        };
        CHECK(link.page(BD, 1, 0, false, "OpenMove by Shokz", 1, fakeNow, idle10) == BtLink::OK);
        CHECK(link.pairAndEncrypt(fakeNow, idle10) == BtLink::PAIRING_FAILED);
        CHECK(io.count(0x0411) == 1 && io.count(0x040C) == 0 && io.count(0x0C56) == 1 && io.count(0x0413) == 0);
        CHECK(bonds.find(BD) != nullptr && !bonds.dirty());
    }
    {   // 10. No table set (the default): Link_Key_Request still gets the negative reply -- byte-identical to
        //     before NEW-34, which is what keeps every existing gate's wire sequence unchanged.
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); hci.onEvent(evThunk, &link); link.setLog(logFn, nullptr); g_log.clear();
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) { f.cc(op, withBd({ 0x00 }, prm)); };
        std::vector<uint8_t> bd(BD, BD + 6);
        link.onEvent(0x17, bd.data(), 6); idle10(); idle10();
        CHECK(io.count(0x040C) == 1 && io.count(0x040B) == 0);
    }
    {   // 11. NEW-34: a FRESH pairing through connect() (inquiry -> page -> SSP) saves the bond with the key from
        //     the notification, the psrm we paged with and the name from the INQUIRY HIT.
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); hci.onEvent(evThunk, &link); link.setLog(logFn, nullptr); g_log.clear();
        BondTable bonds; link.setBonds(&bonds);
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) {
            if (preamble(f, op, prm)) return;
            if (op == 0x0405) { f.cs(op); f.ev(0x03, connComplete(0x00)); return; }
            if (op == 0x0411) { f.cs(op); f.ev(0x17, std::vector<uint8_t>(BD, BD + 6)); return; }
            if (sspDance(f, op, prm, KEY1)) return;
            if (op == 0x0413) { f.cs(op); f.ev(0x08, { 0x00, 0x01, 0x00, 0x01 }); return; }
            f.cc(op, { 0x01 });
        };
        CHECK(link.connect("Shokz", fakeNow, idle10) == BtLink::OK);
        CHECK(link.pairAndEncrypt(fakeNow, idle10) == BtLink::OK);
        CHECK(io.count(0x040B) == 0 && io.count(0x040C) == 1 && strcmp(link.pairedBy(), "ssp") == 0);
        const Bond *b = bonds.find(BD);
        CHECK(b && memcmp(b->key, KEY1, 16) == 0 && b->keyType == 4 && b->psrm == 1 && strcmp(b->name, "OpenMove by Shokz") == 0);
        CHECK(bonds.count() == 1 && bonds.dirty());
        bool sawSaved = false; for (auto &l : g_log) if (l.find("bond=saved") != std::string::npos) sawSaved = true;
        CHECK(sawSaved);
    }
```

- [ ] **Step 2: Run the suite to verify it fails**

Run: `sh ~/Development/M2Radio/bt/test/run.sh`
Expected: the arms COMPILE (Task 3's header already declares `setBonds()` and `page()`) and FAIL at runtime -- `FAIL btlink_test.cpp:<line>: io.count(0x040B) == 1` from arm 7, `sawReject` from arm 8, `sawSaved` from arm 11 -- so `btlink_test` reports a non-zero failure count and the runner exits 1. The arms must be red before the implementation lands.

- [ ] **Step 3: Implement — the opcode**

In `bt/BtLink.cpp`'s first anonymous enum, after `OP_LINK_KEY_REQ_NEG    = 0x040C,` add:

```cpp
    OP_LINK_KEY_REQ_REPLY  = 0x040B,   // bd(6) key(16) -> Command Complete status+bd (NEW-34)
```

- [ ] **Step 4: Implement — `pairAndEncrypt()`**

Replace the whole function (from `// --- pairAndEncrypt(): Authentication_Requested (SSP path)` through its closing brace) with:

```cpp
// --- pairAndEncrypt(): Authentication_Requested; if a STORED key was offered (NEW-34)
// the outcome decides the first rung -- success = a stored-key authentication, a
// 0x05/0x06 rejection = erase the bond and pair afresh on this link, anything else =
// transient, keep the bond and fail; then the pre-existing SSP path with its
// Write_Simple_Pairing_Mode=0 + PIN retry; Set_Connection_Encryption on success.
// Every command that answers via Command Status is guarded the way page() guards
// Create_Connection: a rejected/unaccepted command returns immediately. ---
BtLink::Result BtLink::pairAndEncrypt(uint32_t (*now)(), void (*idle)()) {
    Hci::Reply r;
    uint8_t hp[2] = { (uint8_t)(m_handle & 0xFF), (uint8_t)(m_handle >> 8) };

    // Authentication_Requested -> Link_Key_Request(stored key or neg) -> [SSP] -> [Link_Key_Notification]
    //   -> Authentication_Complete.  Encryption needs the link AUTHENTICATED, so
    //   wait for Auth_Complete (not just Simple_Pairing_Complete) -- else
    //   Set_Connection_Encryption races ahead and fails with 0x2F.
    m_pairDone = false; m_authDone = false; m_haveLinkKey = false; m_keyOffered = false;
    Hci::Error ae = m_hci.run(OP_AUTH_REQUESTED, hp, 2, &r, 2000, idle);
    if (ae != Hci::OK || !r.statusEvent) {
        logf("auth_requested=fail reason=%s status=0x%02X", ae == Hci::OK ? "not_command_status" : Hci::errorName(ae), r.status);
        return PAIRING_FAILED;
    }
    uint32_t t0 = now();
    while (!m_authDone && now() - t0 < 25000) idle();
    logf("pairing=%s auth=%s link_key=%s",
         m_pairDone && m_pairStatus == 0x00 ? "ok" : "incomplete",
         m_authDone && m_authStatus == 0x00 ? "ok" : "fail/timeout",
         m_haveLinkKey ? "stored" : "none");

    if (m_keyOffered) {
        if (m_authDone && m_authStatus == 0x00 && !m_haveLinkKey) {
            // The peer accepted the stored key: authenticated with no pairing at all.
            m_pairedBy = "stored";
            if (m_bonds) m_bonds->touch(m_bd);
        } else if (m_authDone && (m_authStatus == 0x05 || m_authStatus == 0x06)) {
            // The peer holds no matching key for us (it forgot us, or was re-paired elsewhere):
            // the bond is stale.  Erase it and pair afresh on THIS link -- the next
            // Link_Key_Request gets the negative reply and the SSP dance (or the PIN path when
            // SSP is off) follows.  If the peer tears the ACL down first, the command below fails
            // with No Connection and the next attempt pairs fresh: the bond is already gone.
            if (m_bonds) m_bonds->erase(m_bd);
            logf("bond_rejected: status=0x%02X -> erased", m_authStatus);
            m_pairedBy = "none";
            m_pairDone = false; m_authDone = false; m_haveLinkKey = false; m_keyOffered = false;
            Hci::Error ae2 = m_hci.run(OP_AUTH_REQUESTED, hp, 2, &r, 2000, idle);
            if (ae2 != Hci::OK || !r.statusEvent) {
                logf("auth_requested(fresh)=fail reason=%s status=0x%02X", ae2 == Hci::OK ? "not_command_status" : Hci::errorName(ae2), r.status);
                return PAIRING_FAILED;
            }
            t0 = now();
            while (!m_authDone && now() - t0 < 25000) idle();
            logf("pairing(fresh)=%s auth=%s link_key=%s",
                 m_pairDone && m_pairStatus == 0x00 ? "ok" : "incomplete",
                 m_authDone && m_authStatus == 0x00 ? "ok" : "fail/timeout",
                 m_haveLinkKey ? "stored" : "none");
        } else if (!(m_authDone && m_authStatus == 0x00)) {
            // Any other outcome with a key offered (timeout, LMP response timeout, ...) is
            // transient: keep the bond, fail the attempt.  No PIN fallback -- nothing was pairing.
            logf("auth(stored)=fail status=0x%02X -> bond kept", m_authDone ? m_authStatus : 0xFF);
            return PAIRING_FAILED;
        }
        // (success with a key offered AND a new key notified: the peer chose to re-pair; the
        // notification handler already saved the new key and pairedBy() reads ssp/pin.)
    }

    if (!m_authDone || m_authStatus != 0x00) {
        // SSP failed (or the peer never finished it) -- drop to legacy PIN and
        // retry once.  onEvent()'s PIN_Code_Request handler sets m_pairedBy
        // when the peer actually asks for one.
        m_pairedBy = "none";
        uint8_t sspOff = 0x00; Hci::Reply r2;
        m_hci.run(OP_WRITE_SSP_MODE, &sspOff, 1, &r2, 1000, idle);
        m_pairDone = false; m_authDone = false; m_haveLinkKey = false; m_keyOffered = false;
        Hci::Error ae2 = m_hci.run(OP_AUTH_REQUESTED, hp, 2, &r2, 2000, idle);
        if (ae2 != Hci::OK || !r2.statusEvent) {
            logf("auth_requested(pin)=fail reason=%s status=0x%02X", ae2 == Hci::OK ? "not_command_status" : Hci::errorName(ae2), r2.status);
            return PAIRING_FAILED;
        }
        t0 = now();
        while (!m_authDone && now() - t0 < 25000) idle();
        bool sawPin = strcmp(m_pairedBy, "pin") == 0;
        logf("pairing(pin)=%s auth=%s link_key=%s",
             m_pairDone && m_pairStatus == 0x00 ? "ok" : "incomplete",
             m_authDone && m_authStatus == 0x00 ? "ok" : "fail/timeout",
             m_haveLinkKey ? "stored" : "none");
        if (!m_authDone || m_authStatus != 0x00)
            return sawPin ? PIN_FAILED : PAIRING_FAILED;
    }

    // Set_Connection_Encryption -> Encryption_Change (status=0x00 enabled=1).
    m_encDone = false; m_encStatus = 0xFF; m_encrypted = false;
    uint8_t ep[3] = { (uint8_t)(m_handle & 0xFF), (uint8_t)(m_handle >> 8), 0x01 };
    Hci::Error ee = m_hci.run(OP_SET_CONN_ENCRYPTION, ep, 3, &r, 2000, idle);
    if (ee != Hci::OK || !r.statusEvent) {
        logf("set_conn_encryption=fail reason=%s status=0x%02X", ee == Hci::OK ? "not_command_status" : Hci::errorName(ee), r.status);
        return ENCRYPTION_FAILED;
    }
    t0 = now();
    while (!m_encDone && now() - t0 < 10000) idle();
    if (m_encDone && m_encStatus == 0x00 && m_encrypted) {
        logf("connect_secure=ok encryption=on paired_by=%s", m_pairedBy);
        return OK;
    }
    logf("connect_secure=fail status=0x%02X enabled=%u", m_encDone ? m_encStatus : 0xFF, m_encrypted ? 1u : 0u);
    return ENCRYPTION_FAILED;
}
```

- [ ] **Step 5: Implement — the two event handlers**

In `onEvent()`, replace the `EV_LINK_KEY_REQUEST` branch:

```cpp
    } else if (code == EV_LINK_KEY_REQUEST && len >= 6) {
        char bs[18]; hciFormatBd(p, bs);
        const Bond *b = m_bonds ? m_bonds->find(p) : nullptr;
        if (b) {
            // NEW-34: Link_Key_Request_Reply with the stored key -- no pairing follows if the peer agrees.
            uint8_t rp[22]; memcpy(rp, p, 6); memcpy(rp + 6, b->key, 16);
            m_keyOffered = true;
            logf("link_key_req: bd=%s -> reply(stored type=%u)", bs, b->keyType);
            m_hci.submit(OP_LINK_KEY_REQ_REPLY, rp, 22, nullptr, nullptr);
        } else {
            logf("link_key_req: bd=%s -> neg_reply (no stored key)", bs);
            m_hci.submit(OP_LINK_KEY_REQ_NEG, p, 6, nullptr, nullptr);
        }
```

and replace the `EV_LINK_KEY_NOTIFY` branch:

```cpp
    } else if (code == EV_LINK_KEY_NOTIFY && len >= 23) {
        m_haveLinkKey = true;
        char bs[18]; hciFormatBd(p, bs);
        if (m_bonds) {
            // NEW-34: remember the peer.  Name: the inquiry hit's, else the name page() was given
            // (a bonded re-pair after a rejection runs no inquiry), else keep whatever the table holds.
            Bond b; memset(&b, 0, sizeof b);
            memcpy(b.bd, p, 6); memcpy(b.key, p + 6, 16); b.keyType = p[22];
            b.psrm = memcmp(p, m_bd, 6) == 0 ? (uint8_t)m_psrm : (uint8_t)0x01;
            const char *nm = "";
            for (uint8_t i = 0; i < m_nHits; i++) if (m_hits[i].named && memcmp(m_hits[i].bd, p, 6) == 0) { nm = m_hits[i].name; break; }
            if (!nm[0] && memcmp(p, m_bd, 6) == 0) nm = m_pageName;
            BondTable::copyName(b.name, nm);
            bool existed = m_bonds->find(p) != nullptr;
            m_bonds->upsert(b);
            logf("link_key: bd=%s type=%u bond=%s", bs, p[22], existed ? "updated" : "saved");
        } else {
            logf("link_key: bd=%s type=%u", bs, p[22]);
        }
```

- [ ] **Step 6: Run the suites to verify they pass**

Run: `sh ~/Development/M2Radio/bt/test/run.sh`
Expected: `btlink_test: 60 checks, 0 failures` (count approximate; failures 0), every other suite green, `BT-HOST-TESTS: PASS`.

- [ ] **Step 7: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add bt/BtLink.cpp bt/test/btlink_test.cpp && git commit -m "feat(bt): BtLink bonds -- Link_Key_Request_Reply from the table, notification upsert, stored-key rung with erase-on-0x05/0x06 (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: `A2dpSource` bonded-candidate walk (M2Radio)

**Files:**
- Modify: `~/Development/M2Radio/bt/A2dpSource.h`
- Modify: `~/Development/M2Radio/bt/A2dpSource.cpp`

No dedicated host suite exists for A2dpSource; the bt host build compiles it (`run.sh` globs every `bt/*.cpp`), and Task 8's gate is its behavioural test.

- [ ] **Step 1: Edit the header**

In `bt/A2dpSource.h`:
- after `#include "Sbc.h"` add `#include "BondTable.h"`;
- replace `void setLog(BtLink::LogFn fn, void *ctx) { m_link.setLog(fn, ctx); }` with

```cpp
    void setLog(BtLink::LogFn fn, void *ctx) { m_link.setLog(fn, ctx); m_log = fn; m_logCtx = ctx; }
    // NEW-34: bonded devices.  connect() pages them first (most recent first, filtered by the target
    // name when one is given, the first candidate PAGE_ATTEMPTS times and each later one once) and
    // falls back to the inquiry path on every attempt.  Null = today's behaviour.
    void setBonds(BondTable *t) { m_bonds = t; m_link.setBonds(t); }
    BondTable *bonds() { return m_bonds; }
```
- in the `private:` section, after `static void onData(...)` add

```cpp
    void logf(const char *fmt, ...);
    BtLink::LogFn m_log = nullptr; void *m_logCtx = nullptr; char m_lb[96];
    BondTable *m_bonds = nullptr;
```

- [ ] **Step 2: Edit the implementation**

In `bt/A2dpSource.cpp`, change the includes to

```cpp
#include "A2dpSource.h"
#include "HciEvents.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
```

add after `resultName()`:

```cpp
void A2dpSource::logf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(m_lb, sizeof m_lb, fmt, ap); va_end(ap);
    if (m_log) m_log(m_logCtx, m_lb);
}
```

and replace the first line of `connect()` (`if (m_link.connect(name, now, idle) != BtLink::OK) return CONNECT_FAILED;`) with:

```cpp
    // NEW-34: bonded candidates first -- most recent first, filtered by the target name when one
    // is given, the first candidate paged PAGE_ATTEMPTS times and each later one once -- then
    // today's inquiry path as the fallback on every attempt (brainstorm decision 3).
    bool linked = false;
    if (m_bonds && m_bonds->count()) {
        bool first = true;
        for (uint8_t i = 0; i < m_bonds->count() && !linked; i++) {
            Bond b = m_bonds->at(i);                                     // a COPY: the ladder may reorder the table later
            if (name && name[0] && !strstr(b.name, name)) continue;
            char bs[18]; hciFormatBd(b.bd, bs);
            uint8_t attempts = first ? BtLink::PAGE_ATTEMPTS : 1; first = false;
            logf("bond_try: bd=%s name=\"%s\" attempts=%u", bs, b.name, attempts);
            if (m_link.page(b.bd, b.psrm, 0, false, b.name, attempts, now, idle) == BtLink::OK) linked = true;
        }
        if (!linked) logf("bond_page=none -> inquiry");
    }
    if (!linked && m_link.connect(name, now, idle) != BtLink::OK) return CONNECT_FAILED;
```

- [ ] **Step 3: Build check**

Run: `sh ~/Development/M2Radio/bt/test/run.sh`
Expected: `BT-HOST-TESTS: PASS` (A2dpSource.cpp is compiled into every suite).

- [ ] **Step 4: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add bt/A2dpSource.h bt/A2dpSource.cpp && git commit -m "feat(bt): A2dpSource::setBonds -- page bonded candidates most-recent-first (name-filtered) before the inquiry fallback (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: `hci_peer.py` `reconnect` phase (evkb)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/hci_peer.py`

- [ ] **Step 1: Document the phase**

In the module docstring, after the `media` entry add:

```
  reconnect   the avdtp acceptor three times over on one socket (NEW-34 piece 1):
              link 1 pairs by SSP (key #1); link 2 must be paged WITHOUT an inquiry
              and authenticate with Link_Key_Request_Reply carrying key #1 (no IO-cap
              dance); on link 3 the offered key is REJECTED (Authentication_Complete
              0x06) once, so the host must pair afresh and receive key #2.  A page to
              the DECOY address (AA:BB:CC:DD:EE:99) is answered Page Timeout and
              recorded -- the host's target-name filter must never send one.
              Ends when the third link's START is accepted.
```

- [ ] **Step 2: Constants and phase tables**

After the `DEVICES` list add:

```python
DECOY_BD = bytes.fromhex("99EEDDCCBBAA")           # prints AA:BB:CC:DD:EE:99 -- a bond whose name never matches the target
KEY1, KEY2 = bytes(range(16)), bytes(range(16, 32))   # the keys notified on links 1 and 3
```

In `LAST_OPCODE` add `"reconnect": 0x0413` (same terminal command as avdtp; the real end is below).

In `phase_done()` add, before the final `return`:

```python
    if phase == "reconnect": return peer.rc["started_links"] >= 3 and not peer.avdtp["error"]
```

- [ ] **Step 3: Per-link state, reset on every Create_Connection**

In `Peer.__init__`, replace the block that assigns `self.chans`, `self.next_cid`, `self.avdtp = {...}` and `self.rev = {...}` (from `self.chans = {}` through the `self.rev = {...}` literal, keeping its comments) with a call `self.reset_link()`, and add the tally:

```python
        self.reset_link()
        # --- reconnect phase: the tally the gate asserts, and which link we are on ---
        self.rc = {"links": 0, "inquiries": 0, "create_conns": 0, "key_replies": 0, "key_ok": 0, "key_rejected": 0,
                   "neg_replies": 0, "iocap_dances": 0, "notified": 0, "started_links": 0,
                   "reject_on_link": 3, "rejected": False, "keys": {}}   # keys: bd -> the key we last notified for it
```

Then add the method to `Peer` (next to `send`):

```python
    def reset_link(self):
        # L2CAP acceptor + SDP responder + AVDTP acceptor state -- per ACL link.  The avdtp/media
        # phases see one link; the reconnect phase calls this again on every Create_Connection.
        # key: the CID the far end (the firmware) assigned ITSELF and sent us as SCID in its
        # Connection Request (host-owned, not ours) -> value: (the CID we assigned for our side, psm)
        self.chans = {}
        self.next_cid = 0x0340
        self.avdtp = {"config": None, "opened": False, "started": False, "order": [], "error": False,
                      "sig_cid": None, "discover_pending": None, "delay_cfg": False,
                      "delay_sent": False, "delay_acked": False, "open_pending": None}
        self.rev = {"state": "idle", "my_cid": 0x0E85, "their_cid": None, "cfg_req_seen": False, "cfg_rsp_seen": False,
                    "query_sent": False, "answer": None, "done": False, "handle": None}
```

(Move the explanatory comments that sat on those dict literals into `reset_link` so nothing is lost.)

- [ ] **Step 4: Command handling**

In `handle()`:

- In the `OP_INQUIRY` branch, first line: `self.rc["inquiries"] += 1`.
- Replace the `elif opcode == 0x0405:` branch with:

```python
        elif opcode == 0x0405:                                              # Create_Connection -> Command Status, Connection Complete
            bd = params[:6]
            if self.phase == "reconnect" and bd == DECOY_BD:
                # The decoy bond's name never matches the target: a page here means the host's name
                # filter is gone.  Modelled as a Page Timeout so the host moves on and the run still
                # completes -- only the tripwire records it.
                self.log.append("PEER-DECOY-PAGED")
                self.send(cmd_status(opcode)); self.send(event(0x03, b"\x04" + struct.pack("<H", 0x0000) + bd + b"\x01\x00"), 0.1)
                return
            self.peer_bd = bd
            if self.phase == "reconnect":
                self.rc["links"] += 1; self.rc["create_conns"] += 1; self.reset_link()
            self.log.append("PEER-CREATE-CONN role_switch=%d" % params[12])
            self.send(cmd_status(opcode)); self.send(event(0x03, b"\x00" + struct.pack("<H", 0x0001) + params[:6] + b"\x01\x00"), 0.1)
```

- After the `0x0411` branch add:

```python
        elif opcode == 0x040B:                                              # Link_Key_Request_Reply: bd(6) key(16) -- the host offers a STORED key
            bd, key = params[:6], params[6:22]
            self.send(cmd_complete(opcode, b"\x00" + bd))
            self.rc["key_replies"] += 1
            known = self.rc["keys"].get(bd)
            if known is None or key != known:
                self.log.append("PEER-KEY-MISMATCH offered=%s known=%s" % (key.hex(), known.hex() if known else "none"))
                self.send(event(0x06, b"\x05" + struct.pack("<H", 0x0001)), 0.05)          # Authentication Failure
            elif self.rc["links"] == self.rc["reject_on_link"] and not self.rc["rejected"]:
                self.rc["rejected"] = True; self.rc["key_rejected"] += 1
                self.log.append("PEER-KEY-REJECTED link=%d" % self.rc["links"])
                self.send(event(0x06, b"\x06" + struct.pack("<H", 0x0001)), 0.05)          # PIN or Key Missing: the peer forgot us
            elif self.rc["links"] == self.rc["reject_on_link"]:
                self.log.append("PEER-KEY-STALE-REPLAY link=%d" % self.rc["links"])          # the host offered the rejected key AGAIN
                self.send(event(0x06, b"\x06" + struct.pack("<H", 0x0001)), 0.05)
            else:
                self.rc["key_ok"] += 1
                self.send(event(0x06, b"\x00" + struct.pack("<H", 0x0001)), 0.05)          # authenticated, no pairing
```

- In the `0x040C` branch, first line: `self.rc["neg_replies"] += 1`.
- In the `0x042B` branch, first line: `self.rc["iocap_dances"] += 1`.
- Replace the `0x042C` branch with:

```python
        elif opcode == 0x042C:                                              # User_Confirmation_Request_Reply -> pairing complete, link key, auth complete
            self.send(cmd_complete(opcode, b"\x00" + params[:6]))
            key = KEY1 if self.rc["notified"] == 0 else KEY2                # a DIFFERENT key on the re-pair, so key_changed=1 is checkable
            self.rc["keys"][params[:6]] = key; self.rc["notified"] += 1
            self.send(event(0x36, b"\x00" + params[:6]), 0.05)              # Simple_Pairing_Complete
            self.send(event(0x18, params[:6] + key + b"\x04"), 0.1)         # Link_Key_Notification (unauthenticated combination)
            self.send(event(0x06, b"\x00" + struct.pack("<H", 0x0001)), 0.15)      # Authentication_Complete
```

- In `handle_avdtp`, in the `sig == 0x07` START branch, after `self.avdtp["started"] = True` add `self.rc["started_links"] += 1`.

- [ ] **Step 5: The report**

In `__main__`, after the `if phase == "media":` report block add:

```python
    if phase == "reconnect":
        r = peer.rc
        print("PEER-RECONNECT inquiries=%d create_conns=%d key_replies=%d key_ok=%d key_rejected=%d neg_replies=%d iocap_dances=%d notified=%d started_links=%d"
              % (r["inquiries"], r["create_conns"], r["key_replies"], r["key_ok"], r["key_rejected"], r["neg_replies"], r["iocap_dances"], r["notified"], r["started_links"]))
```

Also extend the `if phase in ("avdtp", "media"):` held-stage report to `if phase in ("avdtp", "media", "reconnect"):`.

- [ ] **Step 6: Syntax check and the existing gates**

Run: `python3 -m py_compile ~/Development/rt1170/evkb/examples/networking/m2_hci_probe/hci_peer.py && echo OK`
Expected: `OK`.

Run the two existing peer-driven gates that share this file, from the example directory: `./run_qemu_avdtp.sh` then `./run_qemu_hci.sh`.
Expected: both print their `PASS:` line (the avdtp/media phases pair once, so `KEY1` is what they always notified; `reset_link()` runs once at construction for them).

- [ ] **Step 7: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add examples/networking/m2_hci_probe/hci_peer.py && git commit -m "test(m2_hci_probe): hci_peer.py reconnect phase -- stored-key reply, per-link reset, rejection on link 3, decoy page tripwire (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: The probe's `M2_BT_RECONNECT` build (evkb)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/CMakeLists.txt` (after the `M2_BT_CONNECT` option block, line ~188)
- Modify: `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/m2_hci_probe.cpp`

- [ ] **Step 1: The CMake option**

After the `if(M2_BT_CONNECT) ... endif()` block add:

```cmake
# NEW-34 piece 1: the [reconnect] gate's build.  Replaces the hand-driven probeConnect() with
# A2dpSource + BondTable + BondStoreEeprom and runs THREE connects in one boot: a fresh pairing,
# a cold reload from the EEPROM emulation + a stored-key reconnect past a decoy bond, then a
# modelled rejection -> re-pair.  Requires M2_BT_CONNECT (the bt/ manifest).  Default OFF.
option(M2_BT_RECONNECT "NEW-34: known-device reconnect probe (three connects, bond store round trip)" OFF)
if(M2_BT_RECONNECT)
    if(NOT M2_BT_CONNECT)
        message(FATAL_ERROR "M2_BT_RECONNECT needs M2_BT_CONNECT=ON")
    endif()
    add_definitions(-DM2_BT_RECONNECT=1)
endif()
```

- [ ] **Step 2: Includes**

In `m2_hci_probe.cpp`, change the connect includes block to:

```cpp
#if defined(M2_BT_CONNECT)
#include <L2cap.h>
#include <BtLink.h>
#include <SdpServer.h>
#include <Sdp.h>
#include <Avdtp.h>
#endif
#if defined(M2_BT_RECONNECT)
#include <A2dpSource.h>
#include <BondTable.h>
#include <BondStoreEeprom.h>
#include <avr/eeprom.h>            // eeprom_initialize(): the cold-reload instrument
#endif
```

- [ ] **Step 3: Split the connect globals**

The block that begins `#if defined(M2_BT_CONNECT)` / `static L2cap  l2(hciIo);` currently holds, in order: `l2`, `link`, `avdtp`, `sdpServer`, `nowMs`, `btLog`, `s_sdpDone`, `s_avdtpVer`, the `s_p2*` outcome fields, `onL2capData`, `onAclThunk`. Restructure it as:

```cpp
#if defined(M2_BT_CONNECT)
static uint32_t nowMs() { return millis(); }
static void btLog(void *, const char *s) { CONSOLE.println(s); }
// Phase-2 outcome, latched by the connect probe and echoed in every loop() heartbeat
// so the result is readable from ANY capture -- the one-shot setup() output is
// easily missed across a reset (the VCOM reconnect gap), and this makes the bench
// run deterministic regardless of when the reader attaches.
static const char *s_p2link = "n/a", *s_p2sec = "n/a", *s_p2pair = "-";
static int         s_p2avdtp = -1;
static uint16_t    s_p2mtu   = 0;
#endif

#if defined(M2_BT_CONNECT) && !defined(M2_BT_RECONNECT)
static L2cap  l2(hciIo);
static BtLink link(hci);
static Avdtp  avdtp;
static SdpServer sdpServer;   // answers the peer's SDP queries of US (both headsets make one on AVDTP contact)

// B6/B7 bookkeeping: filled by the L2cap data callback (record only; all TX
// happens from probeConnect()'s main-context loop, never from here).
static volatile bool     s_sdpDone  = false;
static volatile uint16_t s_avdtpVer = 0;

static void onL2capData(void *, L2cap::Channel &ch, const uint8_t *payload, uint16_t len) {
    if (sdpServer.onData(ch, payload, len)) return;        // the PEER's SDP query on its own channel: answered from the main loop
    if (ch.psm == Avdtp::PSM && ch.localCid == 0x0041) {   // signalling channel only -- the media
        avdtp.onSignalling(payload, len);                  // channel (0x0042) shares this PSM
    } else if (ch.psm == Sdp::PSM) {
        s_avdtpVer = Sdp::parseAvdtpVersion(payload, len);
        s_sdpDone  = true;
    }
}

// Hci::AclFn -> L2cap::onAcl thunk (L2cap's RX entry point takes no ctx).
static void onAclThunk(void *, uint16_t handle, const uint8_t *d, uint16_t len) {
    l2.onAcl(handle, d, len);
}
#endif

#if defined(M2_BT_RECONNECT)
// NEW-34 piece 1: the shipped stack (A2dpSource) plus the bond store, driven three times in
// one boot by probeReconnect().  The hand-driven B4/B6/B7 path above is compiled out here so
// exactly ONE BtLink answers each Link_Key_Request.
static A2dpSource src(hci, hciIo);
static BondTable  bonds;
static void onAclThunk(void *, uint16_t handle, const uint8_t *d, uint16_t len) { src.onAcl(handle, d, len); }
#endif
```

- [ ] **Step 4: Exclusive event forwarding**

In the probe's `onEvent()` (the `static void onEvent(void *, uint8_t code, const uint8_t *p, uint8_t len)` function), replace

```cpp
#if defined(M2_BT_CONNECT)
    // BtLink owns inquiry/connect/SSP-pairing/encryption; L2cap owns ACL
    // credit accounting (Number_Of_Completed_Packets, code 0x13).  Forward
    // every event to both, IN ADDITION to the base handling above -- this is
    // not exclusive with it (e.g. EV_INQUIRY_RESULT is still bookkept into
    // s_found by the base path too).
    link.onEvent(code, p, len);
    l2.onEvent(code, p, len);
#endif
```

with

```cpp
#if defined(M2_BT_RECONNECT)
    // NEW-34: A2dpSource forwards to ITS BtLink and L2cap -- the only ones compiled in,
    // so exactly one reply leaves per Link_Key_Request.
    src.onEvent(code, p, len);
#elif defined(M2_BT_CONNECT)
    // BtLink owns inquiry/connect/SSP-pairing/encryption; L2cap owns ACL
    // credit accounting (Number_Of_Completed_Packets, code 0x13).  Forward
    // every event to both, IN ADDITION to the base handling above -- this is
    // not exclusive with it (e.g. EV_INQUIRY_RESULT is still bookkept into
    // s_found by the base path too).
    link.onEvent(code, p, len);
    l2.onEvent(code, p, len);
#endif
```

- [ ] **Step 5: `probeConnect()` compiled out under the new define; `probeReconnect()` added**

Change the `#if defined(M2_BT_CONNECT)` that guards `probeConnect()` (the line just before its header comment `// --- B4+B6+B7: connect + pair/encrypt (BtLink) ...`) to `#if defined(M2_BT_CONNECT) && !defined(M2_BT_RECONNECT)`. Immediately after that function's closing `#endif`, add:

```cpp
#if defined(M2_BT_RECONNECT)
// --- NEW-34 piece 1: three connects in one boot against hci_peer.py's `reconnect` phase.
// Every line below is asserted by run_qemu_reconnect.sh; every number in the peer's tally
// is counted by the PEER, so none can be satisfied by printing.
static const uint8_t RC_FAKE_BD[6]  = { 0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA };   // FAKE-HEADSET-01 (hci_peer.py DEVICES[0])
static const uint8_t RC_DECOY_BD[6] = { 0x99, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA };   // a bond whose name never matches the target
#if defined(M2_BT_TARGET_NAME)
#define RC_TARGET M2_BT_TARGET_NAME
#else
#define RC_TARGET nullptr
#endif
static A2dpSource::Result rcConnect(int phase) {
    A2dpSource::Result r = src.connect(RC_TARGET, s_aclNum, nowMs, idleMs);
    CONSOLE.print("reconnect_phase="); CONSOLE.print(phase);
    CONSOLE.print(" result="); CONSOLE.print(A2dpSource::resultName(r));
    CONSOLE.print(" paired_by="); CONSOLE.println(src.link().pairedBy());
    s_p2link = A2dpSource::resultName(r); s_p2pair = src.link().pairedBy();
    s_p2sec = r == A2dpSource::OK ? "ok" : "fail"; s_p2avdtp = (int)src.avdtp().state(); s_p2mtu = src.mediaMtu();
    return r;
}
static void rcColdReload() {
    // Forget everything in RAM, rebuild the emulation's sector index FROM THE FLASH (the same
    // instrument storage-memory/eeprom_test uses for its cold-start rescan), then load.
    bonds.clear(); eeprom_initialize(); BondStoreEeprom::load(bonds);
}
static void probeReconnect() {
    src.setLog(btLog, nullptr); src.setPin("1234"); src.setBonds(&bonds);
    hci.onAcl(onAclThunk, nullptr);
    BondStoreEeprom::load(bonds);
    CONSOLE.print("bonds_boot="); CONSOLE.println(bonds.count());
    // Phase 1: a fresh pairing (inquiry + SSP).  The notification creates the bond; the store persists it.
    if (rcConnect(1) != A2dpSource::OK) { CONSOLE.println("reconnect=fail phase=1"); return; }
    uint8_t key1[16];
    { const Bond *b = bonds.find(RC_FAKE_BD); if (!b) { CONSOLE.println("reconnect=fail phase=1 no_bond"); return; } memcpy(key1, b->key, 16); }
    BondStoreEeprom::save(bonds);
    src.link().disconnect(nowMs, idleMs);
    // Phase 2: cold reload (proves the round trip), plant a DECOY at the front, save, cold reload
    // again -- the load that matters must recover BOTH.  Then connect: the name filter must skip
    // the decoy and the stored key must authenticate with no inquiry and no pairing.
    rcColdReload();
    { Bond d; memset(&d, 0, sizeof d); memcpy(d.bd, RC_DECOY_BD, 6); memset(d.key, 0xEE, 16); d.keyType = 4; d.psrm = 1;
      BondTable::copyName(d.name, "DECOY"); bonds.upsert(d); }
    BondStoreEeprom::save(bonds);
    rcColdReload();
    CONSOLE.print("bonds_reload="); CONSOLE.println(bonds.count());
    if (rcConnect(2) != A2dpSource::OK) { CONSOLE.println("reconnect=fail phase=2"); return; }
    BondStoreEeprom::save(bonds);
    src.link().disconnect(nowMs, idleMs);
    // Phase 3: the peer rejects the stored key on this link -> erase, fresh pairing, a different key.
    if (rcConnect(3) != A2dpSource::OK) { CONSOLE.println("reconnect=fail phase=3"); return; }
    BondStoreEeprom::save(bonds);
    src.link().disconnect(nowMs, idleMs);
    const Bond *b3 = bonds.find(RC_FAKE_BD);
    CONSOLE.print("bonds_final="); CONSOLE.print(bonds.count());
    CONSOLE.print(" key_changed="); CONSOLE.println(b3 && memcmp(b3->key, key1, 16) != 0 ? 1 : 0);
    CONSOLE.println("reconnect=done");
}
#endif
```

- [ ] **Step 6: `setup()` calls**

Replace

```cpp
        probeInquiry();
#if defined(M2_BT_CONNECT)
        probeConnect();
#endif
```

with

```cpp
#if !defined(M2_BT_RECONNECT)
        probeInquiry();                 // the reconnect probe counts inquiries; BtLink's is the only one allowed
#endif
#if defined(M2_BT_RECONNECT)
        probeReconnect();
#elif defined(M2_BT_CONNECT)
        probeConnect();
#endif
```

- [ ] **Step 7: Build the variant, and confirm the other builds are untouched**

Run:
```bash
cd ~/Development/rt1170/evkb/examples/networking/m2_hci_probe && cmake -S . -B build-reconnect -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RECONNECT=ON && cmake --build build-reconnect 2>&1 | tail -3
```
Expected: `[100%] Built target m2_hci_probe` (or the macros' equivalent final line) with no warnings from `m2_hci_probe.cpp`.

Then: `./run_qemu.sh && ./run_qemu_avdtp.sh`
Expected: both `PASS:` lines (the card-absent build has no bt/ at all; the avdtp build has no `M2_BT_RECONNECT`, so its preprocessing is unchanged).

- [ ] **Step 8: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add examples/networking/m2_hci_probe/CMakeLists.txt examples/networking/m2_hci_probe/m2_hci_probe.cpp && git commit -m "feat(m2_hci_probe): M2_BT_RECONNECT -- probeReconnect(): three A2dpSource connects with a BondStoreEeprom cold reload and a decoy bond (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: The `[reconnect]` gate (evkb)

**Files:**
- Create: `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/run_qemu_reconnect.sh`
- Create: `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/transcript_qemu_reconnect.txt` (captured, not written)

- [ ] **Step 1: Write the gate**

```sh
#!/bin/sh
# run_qemu_reconnect.sh -- the [reconnect] gate for m2_hci_probe (NEW-34 piece 1): a
# bonded device is reconnected with NO inquiry and NO pairing, from a bond that made a
# round trip through the EEPROM store; a bond the peer rejects is erased and replaced.
#
# WHAT THIS PROVES
#   Built with -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RECONNECT=ON,
#   probeReconnect() drives the SHIPPED stack (A2dpSource + BondTable + BondStoreEeprom)
#   three times in one boot against hci_peer.py's `reconnect` phase:
#     1. fresh pairing: inquiry -> SSP -> key #1 notified -> bond saved -> AVDTP STREAMING;
#     2. cold reload from the EEPROM emulation (RAM wiped, sector index rebuilt from the
#        flash, loaded), a DECOY bond planted at the front and reloaded again, then a page
#        of the bonded address with no inquiry, Link_Key_Request_Reply carrying key #1, no
#        IO-capability dance, STREAMING again;
#     3. the peer REJECTS the offered key (Authentication_Complete 0x06): the bond is erased,
#        a fresh pairing yields key #2, the store holds it, STREAMING a third time.
#   Every tally value is counted by the PEER (inquiries, pages, key replies and whether each
#   matched the key it notified), so none can be satisfied by printing; the three re-inits
#   of L2cap/Avdtp on a fresh handle are a piece-2 prerequisite proven for free.
#
# ★ OWNS ITS OWN BUILD DIRECTORY, build-reconnect/ (same convention as build-avdtp/).
#
# WHAT THIS DOES NOT PROVE
#   Persistence across a POWER CYCLE (QEMU has no backing store behind the FlexSPI window --
#   the cold reload is a within-boot rescan, the same instrument storage-memory/eeprom_test
#   uses) and anything about a real headset's willingness to accept a stored key: both are
#   bench claims, recorded in audio/bt_tone_test/transcript_hw_evkb.txt (RECONNECT section).
#
# DEMONSTRATED RED (filled in by Task 9 of the plan -- keep the five entries here).
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }

BUILD_DIR="$DIR/build-reconnect"
ELF="$BUILD_DIR/m2_hci_probe.elf"

if [ "${GATE_VACUITY:-}" = "1" ] && [ -x "$ELF" ]; then
    :
else
    mkdir -p "$BUILD_DIR"
    CONFIGURE_RC=0
    cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$EVKB/toolchain/rt1170-evkb.toolchain.cmake" \
          -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RECONNECT=ON >"$BUILD_DIR/configure.log" 2>&1 || CONFIGURE_RC=$?
    BUILD_RC=0
    if [ "$CONFIGURE_RC" -eq 0 ]; then
        cmake --build "$BUILD_DIR" >"$BUILD_DIR/build.log" 2>&1 || BUILD_RC=$?
    fi
    if [ "$CONFIGURE_RC" -ne 0 ] || [ "$BUILD_RC" -ne 0 ]; then
        fail "build-reconnect/ did not build -- see build-reconnect/configure.log / build.log"
    fi
fi

OUT="$BUILD_DIR/reconnect.uart"; DBG="$BUILD_DIR/reconnect.dbg"; RES="$BUILD_DIR/reconnect.peer"
rm -f "$OUT" "$DBG" "$RES"
SOCK="/tmp/m2recon_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
    -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
PEER_RC=0
python3 "$DIR/hci_peer.py" reconnect "$SOCK" > "$RES" 2>&1 || PEER_RC=$?
# Wait for the LAST line this probe prints, or its named failure -- never an earlier one
# (the m2_rx_demo[irq] mid-line reap is the standing lesson).
for _ in $(seq 1 200); do
    [ -f "$OUT" ] && grep -q "^reconnect=done\|^reconnect=fail" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT" "reconnect phase"
echo "==== captured UART ===="; cat "$OUT"
echo "==== peer ===="; cat "$RES"

grep -q "RT1176 M.2 HCI probe up" "$OUT" || fail "[reconnect] banner missing"

# ★ Tripwires FIRST (the [avdtp] convention): each names a specific violation the peer itself
# detected.  Every positive check below is downstream of STREAMING, so without these a failure
# at any stage collapses onto the same generic message.
if grep -q "PEER-DECOY-PAGED" "$RES";            then fail "[reconnect] a bond whose name does not match the target was paged -- the name filter is gone"; fi
if grep -q "PEER-KEY-MISMATCH" "$RES";           then fail "[reconnect] the host offered a key the peer never notified: $(grep PEER-KEY-MISMATCH "$RES" | head -1)"; fi
if grep -q "PEER-KEY-STALE-REPLAY" "$RES";       then fail "[reconnect] the rejected key was offered again -- the bond was not erased"; fi
if grep -q "PEER-UNKNOWN-OPCODE" "$RES";         then fail "[reconnect] the host sent a command the peer does not model: $(grep PEER-UNKNOWN-OPCODE "$RES" | head -1)"; fi
if grep -q "PEER-AVDTP-START-BEFORE-OPEN" "$RES"; then fail "[reconnect] START was sent before OPEN was acknowledged"; fi
if grep -q "PEER-L2CAP-CFGRSP-BAD-SCID" "$RES";  then fail "[reconnect] our Config Response names the wrong CID"; fi
if grep -q "PEER-EXCEPTION" "$RES";              then fail "[reconnect] the peer hit a malformed frame: $(grep PEER-EXCEPTION "$RES" | head -1)"; fi

# The probe's own lines, in boot order.
grep -q "^bonds_boot=0[[:space:]]*$" "$OUT"                              || fail "[reconnect] no bonds_boot line -- the reconnect probe never ran (or the store was not empty at boot)"
grep -q "^reconnect_phase=1 result=ok paired_by=ssp[[:space:]]*$" "$OUT" || fail "[reconnect] phase 1 (fresh pairing) did not reach STREAMING by SSP"
grep -q "^bonds_reload=2[[:space:]]*$" "$OUT"                            || fail "[reconnect] bond did not survive the cold reload (expected 2 = the paired device + the decoy): $(grep '^bonds_reload=' "$OUT" || echo none)"
grep -q "^reconnect_phase=2 result=ok paired_by=stored[[:space:]]*$" "$OUT" || fail "[reconnect] phase 2 did not authenticate with the stored key"
grep -q "^bond_rejected: status=0x06 -> erased" "$OUT"                   || fail "[reconnect] the rejected bond was not erased by name"
grep -q "^reconnect_phase=3 result=ok paired_by=ssp[[:space:]]*$" "$OUT" || fail "[reconnect] phase 3 (rejection -> fresh pairing) did not reach STREAMING by SSP"
grep -q "^bonds_final=2 key_changed=1[[:space:]]*$" "$OUT"               || fail "[reconnect] the re-pairing did not replace the key"
grep -q "^reconnect=done" "$OUT"                                         || fail "[reconnect] probe did not reach reconnect=done"
NINQ=$(grep -c "^inquiry=started" "$OUT" || true)
[ "$NINQ" -eq 1 ] || fail "[reconnect] expected exactly ONE inquiry (phase 1); saw $NINQ -- a bonded page was replaced by an inquiry"

# The peer's tally, last: every number here was counted on the other side of the socket.
[ "$PEER_RC" -eq 0 ] || fail "[reconnect] peer exited $PEER_RC"
grep -q "^PEER-RECONNECT inquiries=1 create_conns=3 key_replies=2 key_ok=1 key_rejected=1 neg_replies=2 iocap_dances=2 notified=2 started_links=3[[:space:]]*$" "$RES" \
    || fail "[reconnect] peer tally mismatch: $(grep '^PEER-RECONNECT' "$RES" || echo none)"

echo "PASS: a bonded device is paged with no inquiry and authenticates with the stored key after an EEPROM cold reload past a decoy; a rejected bond is erased and re-paired with a new key"
```

Then: `chmod +x run_qemu_reconnect.sh`.

- [ ] **Step 2: Run it**

Run: `cd ~/Development/rt1170/evkb/examples/networking/m2_hci_probe && ./run_qemu_reconnect.sh 2>&1 | tail -25`
Expected: the captured UART shows `bonds_boot=0`, `reconnect_phase=1 result=ok paired_by=ssp`, `bonds_reload=2`, `bond_try: bd=AA:BB:CC:DD:EE:01 name="FAKE-HEADSET-01" attempts=3`, `link_key_req: bd=AA:BB:CC:DD:EE:01 -> reply(stored type=4)`, `reconnect_phase=2 result=ok paired_by=stored`, `bond_rejected: status=0x06 -> erased`, `reconnect_phase=3 result=ok paired_by=ssp`, `bonds_final=2 key_changed=1`, `reconnect=done`; the peer file ends with the exact `PEER-RECONNECT` tally; the script ends with its `PASS:` line. If the first run fails, read the named assertion: it tells you which task's code is wrong.

- [ ] **Step 3: Capture the fixture and check discovery**

```bash
cd ~/Development/rt1170/evkb/examples/networking/m2_hci_probe && cp build-reconnect/reconnect.uart transcript_qemu_reconnect.txt && cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -l | grep -c "" && ./tools/run-all-qemu-gates.sh -l | grep "m2_hci_probe"
```
Expected: the count line reads `130` (129 gates plus the trailing summary line), and the probe lists five ids: `rt1176:networking/m2_hci_probe`, `…[avdtp]`, `…[baud]`, `…[hci]`, `…[reconnect]`.

- [ ] **Step 4: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add examples/networking/m2_hci_probe/run_qemu_reconnect.sh examples/networking/m2_hci_probe/transcript_qemu_reconnect.txt && git commit -m "test(m2_hci_probe): [reconnect] gate -- bonded page without inquiry, EEPROM cold reload past a decoy, rejection -> re-pair; sweep 128 -> 129 (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Demonstrate RED, five ways

**Files:**
- Modify (temporarily, each reverted): `~/Development/M2Radio/bt/BtLink.cpp`, `~/Development/M2Radio/bt/BondStoreEeprom.h`, `~/Development/M2Radio/bt/A2dpSource.cpp`, `~/Development/M2Radio/bt/BondTable.cpp`
- Modify (kept): `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/run_qemu_reconnect.sh` (the `DEMONSTRATED RED` header)

Each mutation is made in the M2Radio checkout, the gate is run (it rebuilds `build-reconnect/` from the local sources), the named failure is confirmed, the mutation is reverted with `git -C ~/Development/M2Radio checkout -- bt/`, and the gate is confirmed GREEN again before the next one. Run from `~/Development/rt1170/evkb/examples/networking/m2_hci_probe`.

- [ ] **Step 1: (a) The host always negative-replies**

In `bt/BtLink.cpp`'s `EV_LINK_KEY_REQUEST` branch change `const Bond *b = m_bonds ? m_bonds->find(p) : nullptr;` to `const Bond *b = nullptr; (void)m_bonds;`.
Run: `./run_qemu_reconnect.sh 2>&1 | tail -2`
Expected: `FAIL: [reconnect] phase 2 did not authenticate with the stored key` (phase 2 prints `paired_by=ssp`; the peer's tally would read `key_replies=0`).
Revert: `git -C ~/Development/M2Radio checkout -- bt/ && ./run_qemu_reconnect.sh 2>&1 | tail -1` → the `PASS:` line.

- [ ] **Step 2: (b) The store never persists**

In `bt/BondStoreEeprom.h`'s `save()` make the first line `return false;`.
Run: `./run_qemu_reconnect.sh 2>&1 | tail -2`
Expected: `FAIL: [reconnect] bond did not survive the cold reload (expected 2 = the paired device + the decoy): bonds_reload=0`.
Revert and confirm green as above.

- [ ] **Step 3: (c) The bond is kept after rejection**

In `bt/BtLink.cpp`'s rejection rung comment out `if (m_bonds) m_bonds->erase(m_bd);`.
Run: `./run_qemu_reconnect.sh 2>&1 | tail -2`
Expected: `FAIL: [reconnect] the rejected key was offered again -- the bond was not erased`.
Revert and confirm green.

- [ ] **Step 4: (d) The target-name filter is removed**

In `bt/A2dpSource.cpp` comment out `if (name && name[0] && !strstr(b.name, name)) continue;`.
Run: `./run_qemu_reconnect.sh 2>&1 | tail -2`
Expected: `FAIL: [reconnect] a bond whose name does not match the target was paged -- the name filter is gone` — note the run still reached phase 2 (the decoy's three pages were answered Page Timeout and the real bond followed); only the tripwire saw it.
Revert and confirm green.

- [ ] **Step 5: (e) The codec skips the CRC (host suite, not the gate)**

In `bt/BondTable.cpp`'s `load()` comment out `if (crc32(in, IMAGE_SIZE - 4) != want) return false;`.
Run: `sh ~/Development/M2Radio/bt/test/run.sh 2>&1 | grep -m1 "FAIL"`
Expected: `FAIL bondtable_test.cpp:<line>: !v.load(bad, sizeof bad) && v.count() == 0` (the flipped-key-byte arm).
Revert: `git -C ~/Development/M2Radio checkout -- bt/ && sh ~/Development/M2Radio/bt/test/run.sh | tail -1` → `BT-HOST-TESTS: PASS`.

- [ ] **Step 6: Record the demonstrations in the gate header and commit (evkb)**

Replace the header line `# DEMONSTRATED RED (filled in by Task 9 of the plan -- keep the five entries here).` with:

```sh
# DEMONSTRATED RED (2026-09-DD), five mutations of M2Radio/bt, each run against this gate,
# each failing BY THE NAMED ASSERTION, each reverted (git -C M2Radio checkout -- bt/) and the
# gate confirmed GREEN again before the next:
#   (a) BtLink answers every Link_Key_Request negatively  -> "phase 2 did not authenticate with the stored key"
#   (b) BondStoreEeprom::save() is a no-op                -> "bond did not survive the cold reload ... bonds_reload=0"
#   (c) the rejection rung keeps the bond                 -> "the rejected key was offered again -- the bond was not erased"
#   (d) A2dpSource ignores the target-name filter         -> "a bond whose name does not match the target was paged"
#       (the run STILL reached phase 2: only the tripwire saw it)
#   (e) BondTable::load() skips the CRC                   -> the host suite's flipped-key-byte arm, not this gate
```

(with today's date), then:

```bash
cd ~/Development/rt1170/evkb && git add examples/networking/m2_hci_probe/run_qemu_reconnect.sh && git commit -m "docs(m2_hci_probe): [reconnect] gate -- record the five RED demonstrations

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: Vacuity cases (evkb)

**Files:**
- Modify: `~/Development/rt1170/evkb/tools/gate-vacuity.test.sh` (inside the `else` branch of the `m2_hci_probe` section, after the `[avdtp]` block's `fi`)

- [ ] **Step 1: Add the three negatives**

```sh
    # [reconnect] (NEW-34 piece 1).  Three negatives, each failing BY NAME: the card-absent
    # capture (the probe never ran), the committed fixture with its bonds_reload line stripped
    # (the store's round trip was not proven), and the fixture with a second inquiry appended
    # (a bonded page was replaced by an inquiry).  NO green replay: like [hci]/[baud]/[avdtp]
    # this gate is peer-driven, and under the fake qemu the peer cannot connect, so even a
    # green capture fails at the peer tally -- by design, not a gap.
    recon_rel="$hci_rel"
    recon_elf="$EVKB/$recon_rel/build-reconnect/m2_hci_probe.elf"
    recon_fixture="$EVKB/$recon_rel/transcript_qemu_reconnect.txt"
    if [ ! -x "$recon_elf" ] || [ ! -f "$recon_fixture" ]; then
        echo "SKIP: reconnect vacuity cases (need build-reconnect/m2_hci_probe.elf and transcript_qemu_reconnect.txt)"
    else
        export GATE_VACUITY=1
        run_gate "$recon_rel" "run_qemu_reconnect.sh" "$hci_absent"; rc=$?
        result=0
        [ "$rc" -ne 0 ] || result=1                                                          # must not pass
        echo "$OUT_TEXT" | grep -q "\[reconnect\] no bonds_boot line" || result=1            # and name it
        report "absent_capture_fails_reconnect_gate" $result

        grep -v "^bonds_reload=" "$recon_fixture" > "$WORK/recon_noreload.txt"
        run_gate "$recon_rel" "run_qemu_reconnect.sh" "$WORK/recon_noreload.txt"; rc=$?
        result=0
        [ "$rc" -ne 0 ] || result=1
        echo "$OUT_TEXT" | grep -q "\[reconnect\] bond did not survive the cold reload" || result=1
        report "stripped_reload_fails_reconnect_gate" $result

        { cat "$recon_fixture"; echo "inquiry=started"; } > "$WORK/recon_twoinq.txt"
        run_gate "$recon_rel" "run_qemu_reconnect.sh" "$WORK/recon_twoinq.txt"; rc=$?
        result=0
        [ "$rc" -ne 0 ] || result=1
        echo "$OUT_TEXT" | grep -q "\[reconnect\] expected exactly ONE inquiry" || result=1
        report "double_inquiry_fails_reconnect_gate" $result
        unset GATE_VACUITY

        rm -f "$EVKB/$recon_rel"/build-reconnect/reconnect.uart "$EVKB/$recon_rel"/build-reconnect/reconnect.peer \
              "$EVKB/$recon_rel"/build-reconnect/reconnect.dbg
    fi
```

- [ ] **Step 2: Run the suite**

Run: `cd ~/Development/rt1170/evkb && ./tools/gate-vacuity.test.sh 2>&1 | grep -E "reconnect|^PASS:|^FAIL:|passed|failed" | tail -8`
Expected: `PASS: absent_capture_fails_reconnect_gate`, `PASS: stripped_reload_fails_reconnect_gate`, `PASS: double_inquiry_fails_reconnect_gate`, and the suite's summary at 35/35 (32 before + 3; re-derive the number from the run, never from this line).

- [ ] **Step 3: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add tools/gate-vacuity.test.sh && git commit -m "test(vacuity): three [reconnect] negatives -- absent capture, stripped bonds_reload, doubled inquiry (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 11: `bt_tone_test` host wiring + `M2_BT_FORGET_BONDS` (evkb)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/CMakeLists.txt`
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/bt_tone_test.cpp`
- Re-capture: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/transcript_qemu.txt` (and `transcript_qemu_media.txt` if the `[media]` gate keeps one)

The gates anchor `^a2dp=connect_failed[[:space:]]*$` and the `hb` line to end-of-line, so the new output goes on lines of its own and those lines stay untouched.

- [ ] **Step 1: The knob**

After the `M2_BT_CONNECT_RETRY` option block in `CMakeLists.txt` add:

```cmake
# Bench knob (NEW-34): wipe the bond store at boot (bonds_forgotten=1), so a run can prove that a
# headset out of pairing mode REFUSES us without a bond -- the control arm of the stored-key claim.
# OFF by default; the gates and the default build load the store as usual.
option(M2_BT_FORGET_BONDS "Bench: wipe the bonded-device store at boot" OFF)
if(M2_BT_FORGET_BONDS)
    add_definitions(-DM2_BT_FORGET_BONDS=1)
endif()
```

- [ ] **Step 2: The wiring**

In `bt_tone_test.cpp`:
- after `#include <A2dpSource.h>` (find the existing include) add `#include <BondTable.h>` and `#include <BondStoreEeprom.h>`;
- after `static A2dpSource src(hci, hciIo);` add `static BondTable bonds;                     // NEW-34: bonded devices, persisted in the EEPROM emulation`;
- in `setup()`, right after `src.setLog(btLog, nullptr); src.setPin("1234");` add:

```cpp
#if defined(M2_BT_FORGET_BONDS)
    BondStoreEeprom::wipe(bonds);
    CONSOLE.println("bonds_forgotten=1");
#else
    BondStoreEeprom::load(bonds);
#endif
    src.setBonds(&bonds);
    CONSOLE.print("bonds_boot="); CONSOLE.println(bonds.count());
```

- after the one-shot `CONSOLE.print("a2dp="); CONSOLE.println(A2dpSource::resultName(r2));` add:

```cpp
    BondStoreEeprom::save(bonds);
    CONSOLE.print("bonds="); CONSOLE.print(bonds.count());
    CONSOLE.print(" paired_by="); CONSOLE.println(src.link().pairedBy());
```

- in the `M2_BT_CONNECT_RETRY` loop block, after `CONSOLE.print("a2dp_try="); CONSOLE.println(A2dpSource::resultName(rr));` add the same three lines.

- [ ] **Step 3: Build, run both gates, re-capture the fixtures**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && cmake --build build 2>&1 | tail -1 && ./run_qemu.sh 2>&1 | tail -1 && ./run_qemu_media.sh 2>&1 | tail -1
```
Expected: two `PASS:` lines. The card-absent capture now carries `bonds_boot=0` and `bonds=0 paired_by=none`; the media run carries `bonds=1 paired_by=ssp`.

Then `cp build/serial.uart transcript_qemu.txt` (check the gate's capture path in `run_qemu.sh` first; use the path it writes) and, if `run_qemu_media.sh` has a committed fixture, re-capture it the same way. Re-run `./tools/gate-vacuity.test.sh` from the repo root and confirm no bt_tone_test case regressed.

- [ ] **Step 4: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add examples/audio/bt_tone_test && git commit -m "feat(bt_tone_test): bond store wiring (load after Hci::begin, save after every connect) + M2_BT_FORGET_BONDS bench knob (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 12: `acid_box` wiring, the flash-routing list, the knob (evkb)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/CMakeLists.txt`
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/acid_box.cpp`

- [ ] **Step 1: The flash-routing list**

In the linker-script `string(REPLACE ...)` block, after the line `*libM2Radio*.a:BtLink.cpp.obj(.text* .fastrun)` add (same indentation, a tab):

```
		*libM2Radio*.a:BondTable.cpp.obj(.text* .fastrun)
```

- [ ] **Step 2: The knob**

Inside the `if(M2_BT_OUT)` block, after the `M2_BT_WAKE_PULSE` option's `endif()`, add:

```cmake
    option(M2_BT_FORGET_BONDS "Bench: wipe the bonded-device store at boot (NEW-34 control arm)" OFF)
    if(M2_BT_FORGET_BONDS)
        add_definitions(-DM2_BT_FORGET_BONDS=1)
    endif()
```

- [ ] **Step 3: The wiring**

In `acid_box.cpp`, inside the `#if defined(M2_BT_OUT)` include block after `#include <A2dpSource.h>` add `#include <BondTable.h>` and `#include <BondStoreEeprom.h>`; after `static AudioOutputBluetooth btout;` add `static BondTable bonds;   // NEW-34: bonded devices, persisted in the EEPROM emulation`; in `setup()` right after `src.setLog(btLog, nullptr); src.setPin("1234");` add:

```cpp
#if defined(M2_BT_FORGET_BONDS)
    BondStoreEeprom::wipe(bonds);
    CONSOLE.println("bonds_forgotten=1");
#else
    BondStoreEeprom::load(bonds);
#endif
    src.setBonds(&bonds);
    CONSOLE.print("bonds_boot="); CONSOLE.println(bonds.count());
```

and in `loop()`, right after `CONSOLE.print("a2dp_try="); CONSOLE.println(A2dpSource::resultName(rr));` add:

```cpp
            BondStoreEeprom::save(bonds);                                  // outside streaming: begin() has not run yet
            CONSOLE.print("bonds="); CONSOLE.print(bonds.count());
            CONSOLE.print(" paired_by="); CONSOLE.println(src.link().pairedBy());
```

- [ ] **Step 4: The default build is byte-identical; the bench build fits**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && OC=/Applications/ARM_10/bin/arm-none-eabi-objcopy && $OC -O binary build/acid_box.elf /tmp/acid_before.bin && cmake --build build 2>&1 | tail -1 && $OC -O binary build/acid_box.elf /tmp/acid_after.bin && cmp /tmp/acid_before.bin /tmp/acid_after.bin && echo IDENTICAL && ./run_qemu.sh 2>&1 | tail -1
```
Expected: `IDENTICAL` and the gate's `PASS:` line (the golden `0x25B30A96` unchanged).

Then the bench build (`build-bt/` already exists with the blobs configured; if not, configure it as `transcript_hw_evkb_bt.txt`'s header records):
```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && /Applications/ARM_10/bin/arm-none-eabi-size -A build-bt/acid_box.elf | grep -E "text.itcm|\.text " ; cmake --build build-bt 2>&1 | tail -1 && /Applications/ARM_10/bin/arm-none-eabi-size -A build-bt/acid_box.elf | grep -E "text.itcm|\.text "
```
Expected: it links, and `.text.itcm` is unchanged or smaller (BondTable went to flash). If it grew, `BondTable.cpp.obj` did not match the routing rule — check the object name in `build-bt/` with `find build-bt -name 'BondTable*'`.

- [ ] **Step 5: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add examples/display/acid_box/CMakeLists.txt examples/display/acid_box/acid_box.cpp && git commit -m "feat(acid_box): bond store wiring for the M2_BT_OUT bench build, BondTable routed to flash, M2_BT_FORGET_BONDS (NEW-34 piece 1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 13: ESP32 sink `forget` command (evkb)

**Files:**
- Modify: `~/Development/rt1170/evkb/tools/esp32-a2dp-sink/esp32-a2dp-sink.ino`

- [ ] **Step 1: Add the command**

Replace `loop()` with:

```cpp
// Serial console (NEW-34): `forget` drops every bond the sink holds, so the EVKB's next
// stored-key connect is REJECTED (Authentication_Complete 0x06) -- the un-fakeable
// rejection-path instrument.  Open the port with dtr=False/rts=False (see the bench notes:
// a reader that asserts them resets the module into its bootloader).
static void forgetBonds() {
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) { Serial.println("bonds_cleared=0"); return; }
    esp_bd_addr_t *list = (esp_bd_addr_t *)malloc(sizeof(esp_bd_addr_t) * n);
    if (!list) { Serial.println("bonds_cleared=alloc_fail"); return; }
    int got = n;
    if (esp_bt_gap_get_bond_device_list(&got, list) != ESP_OK) { free(list); Serial.println("bonds_cleared=list_fail"); return; }
    int cleared = 0;
    for (int i = 0; i < got; i++) {
        char bda[18]; fmtBda(list[i], bda);
        esp_err_t e = esp_bt_gap_remove_bond_device(list[i]);
        Serial.printf("forget peer=%s %s\n", bda, e == ESP_OK ? "ok" : esp_err_to_name(e));
        if (e == ESP_OK) cleared++;
    }
    free(list);
    Serial.printf("bonds_cleared=%d\n", cleared);
}

void loop() {
    static uint32_t n = 0;
    static char cmd[16]; static uint8_t len = 0;
    uint32_t until = millis() + 5000;
    while ((int32_t)(until - millis()) > 0) {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                cmd[len] = 0;
                if (len && strcmp(cmd, "forget") == 0) forgetBonds();
                else if (len) Serial.printf("unknown command \"%s\" (try: forget)\n", cmd);
                len = 0;
            } else if (len < sizeof(cmd) - 1) cmd[len++] = c;
        }
        delay(10);
    }
    uint32_t now = millis();
    uint32_t bytes = s_bytes;
    uint32_t rateBps = (now > s_lastRateMs) ? (uint32_t)((uint64_t)(bytes - s_lastRateBytes) * 1000 / (now - s_lastRateMs)) : 0;
    s_lastRateMs = now; s_lastRateBytes = bytes;
    Serial.printf("hb n=%lu conn=%u play=%u peer=%s pkts=%lu bytes=%lu pcm_bytes_per_s=%lu (expect %lu at %lu Hz) dropped=%lu\n",
                  (unsigned long)n++, s_connected, s_playing, s_peer, (unsigned long)s_pkts, (unsigned long)bytes,
                  (unsigned long)rateBps, (unsigned long)(s_pcmRate * 4), (unsigned long)s_pcmRate, (unsigned long)s_dropped);
}
```

(`esp_bt_gap_get_bond_device_num`, `esp_bt_gap_get_bond_device_list` and `esp_bt_gap_remove_bond_device` are in `esp_gap_bt_api.h`, already included.)

- [ ] **Step 2: Compile with arduino-cli (the sketch's own toolchain; see the file header for the board FQBN) and flash it to the sink when at the bench**

Run: `arduino-cli compile --fqbn esp32:esp32:esp32 ~/Development/rt1170/evkb/tools/esp32-a2dp-sink 2>&1 | tail -2`
Expected: `Sketch uses ... bytes` with no errors. (If the FQBN differs, use the one recorded in the sketch header / the bench notes.)

- [ ] **Step 3: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add tools/esp32-a2dp-sink/esp32-a2dp-sink.ino && git commit -m "tools(esp32-a2dp-sink): 'forget' serial command -- drop every bond so the next stored-key connect is rejected (NEW-34 rejection-path instrument)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 14: Pin bump, fresh-user verification, the sweep, the audit, CLAUDE.md (evkb)

**Files:**
- Modify: `~/Development/rt1170/evkb/evkb.cmake` (the `teensy_declare_library(M2Radio ...` line's SHA and its trailing comment)
- Modify: `~/Development/rt1170/evkb/CLAUDE.md` (three anchors)

- [ ] **Step 1: Push M2Radio and bump the pin**

```bash
cd ~/Development/M2Radio && git push origin master && git rev-parse HEAD
```
Copy the SHA. In `evkb.cmake` replace `7c91cca600b0c55f2ee493cb90086fa7a6e1d33c` on the M2Radio line with it, and append to that line's comment (`DD` = today's date, here and in every later step): ` 2026-09-DD: NEW-34 piece 1 -- bt/BondTable + bt/BondStoreEeprom (header-only), BtLink::setBonds/page()/stored-key rung, A2dpSource::setBonds; networking/m2_hci_probe[reconnect] is the first gate to need it and will not BUILD against anything older (BondTable.h missing).`

- [ ] **Step 2: Fresh-user verification — run the new gate against a GitHub-fetched build**

```bash
cd ~/Development/rt1170/evkb/examples/networking/m2_hci_probe && rm -rf /tmp/rc-fetch && cmake -S . -B /tmp/rc-fetch -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RECONNECT=ON 2>&1 | grep -E "M2Radio|clone|Already at" | head -5 && cmake --build /tmp/rc-fetch 2>&1 | tail -1 && mv build-reconnect build-reconnect.local && ln -s /tmp/rc-fetch build-reconnect && ./run_qemu_reconnect.sh 2>&1 | tail -1; rm build-reconnect && mv build-reconnect.local build-reconnect
```
Expected: the configure log shows M2Radio being cloned at the new SHA, the build completes, and the gate prints its `PASS:` line against the fetched-source ELF. A configure that succeeds proves the subdirectory resolves; only the gate run proves the fetched code behaves.

- [ ] **Step 3: The sweep**

Rebuild every image whose sources changed and that a gate runs unrebuilt: `bt_tone_test/build` (done in Task 11), `acid_box/build` (Task 12); the four probe variants rebuild inside their gates. Then, with nothing else running on the machine:

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh 2>&1 | tee /tmp/sweep-new34.log | tail -5
```
Expected: `gates: 129 passed`, exit 0 — or `128 passed, 1 failed` where the red is one of the documented load-sensitivity class (`cm4_audio_test`, `bt_tone_test[media]`, `m2_uap_lwip[uap]`, …) and PASSES when re-run alone. Any other red is a regression from this work: read the gate NAME.

Then, after (never during) the sweep: `./tools/license-audit.sh 2>&1 | tail -1` → `LICENSE-AUDIT: PASS`. If the GATES drift check names `build-reconnect`, add the entry it asks for.

- [ ] **Step 4: CLAUDE.md**

Three edits:
1. `The sweep covers **128 gates** — the merge of` → `The sweep covers **129 gates** — NEW-34 piece 1's `networking/m2_hci_probe[reconnect]` (a bonded device paged with no inquiry, authenticated with a stored key after an EEPROM cold reload past a decoy bond, then a rejected bond erased and re-paired; five REDs), then the merge of`.
2. `The target is **128 passed, 0 failed, 0 SKIP**, or` / `**127 passed, 1 failed, 0 SKIP** when` → `129` / `128`.
3. Insert, before the paragraph beginning `✅ **Measured 2026-09-05 (evening), TWICE: 128 gates discovered`, a new measured paragraph with the sweep's real numbers, the audit result, the vacuity count, and one ★ line: `★ **`m2_hci_probe[reconnect]` proves the POLICY, not the NVM**: QEMU has no backing store behind the FlexSPI window, so its cold reload is a within-boot sector rescan; persistence across a power cycle is the bench's claim (bt_tone_test RECONNECT section).`

- [ ] **Step 5: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add evkb.cmake CLAUDE.md && git commit -m "build: bump M2Radio pin to <sha> (NEW-34 piece 1); docs: sweep 129, [reconnect] gate recorded

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 15: Silicon acceptance — runs A-E on `bt_tone_test`, the acid_box witness

**Files:**
- Append: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/transcript_hw_evkb.txt` (a `RECONNECT (NEW-34 piece 1)` section)
- Append: `~/Development/rt1170/evkb/examples/display/acid_box/transcript_hw_evkb_bt.txt`

Bench recipe (from CLAUDE.md): `pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink`, then `LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-hw/bt_tone_test.elf` and `... verify`, both with NO console reader attached; then attach `python3 tools/rt1170-console.py /dev/cu.usbmodem<...> 115200 | tee <capture>` and press SW4. A full POWER CYCLE (not SW4) is the reset that makes the NVM claim honest in runs B, D and E. The bench build configuration is the one recorded at the top of the existing transcript (`M2_BT_ASSERT_CTS`/`M2_BT_RTS_FLOW`, the NXP blobs, `M2_BT_FAST_BAUD`, `M2_BT_TARGET_NAME=OpenMove`), plus `-DM2_BT_CONNECT_RETRY=ON` so a missed page retries.

- [ ] **Step 1: Run A — bond creation.** Shokz in pairing mode (hold power until it announces pairing). Boot. Record the capture. Expected lines: `bonds_boot=0`, `inquiry=started`, `inq_name: … "OpenMove by Shokz"`, `connect_secure=ok encryption=on paired_by=ssp`, `link_key: bd=… type=4 bond=saved`, `a2dp=ok`, `bonds=1 paired_by=ssp`, a tone audible on the Shokz, `hb streaming=1 … drops=0`.

- [ ] **Step 2: Run B — the claim.** Power-cycle the EVKB. Turn the Shokz OFF then ON normally (NOT pairing mode). Boot. Expected: `bonds_boot=1`, NO `inquiry=started`, `bond_try: bd=… name="OpenMove by Shokz" attempts=3`, `connect=ok handle=… attempt=N`, `link_key_req: bd=… -> reply(stored type=4)`, `auth_complete: status=0x00 …`, `connect_secure=ok encryption=on paired_by=stored`, `a2dp=ok`, `bonds=1 paired_by=stored`, tone audible. Record the elapsed time from `hci_reset=ok` to `a2dp=ok` (console timestamps or a stopwatch) beside run A's.

- [ ] **Step 3: Run C — the control.** Reconfigure the bench build with `-DM2_BT_FORGET_BONDS=ON`, flash, power-cycle, Shokz still in normal mode. Expected: `bonds_forgotten=1`, `bonds_boot=0`, then EITHER `connect=fail reason=no_inquiry_hit` (the Shokz is not discoverable outside pairing mode) OR an inquiry hit followed by a pairing failure (`pairing_failed` / `auth_complete: status=0x05|0x06|0x0C`) — and no `a2dp=ok` within three retry cycles. **If it DOES stream**, record that the Shokz accepts pairing at any time, that run B therefore proves less than hoped, and let the log evidence plus run D carry the claim. Reconfigure the bench build WITHOUT the knob afterwards.

- [ ] **Step 4: Run D — the rejection path against the ESP32 sink (three boots).** Flash Task 13's sink sketch; open its console with `dtr=False, rts=False` (a fresh boot banner right after connecting means the reader RESET the module — the sketch header says how). ★ `forget` is DEFERRED behind the BT task and, when the EVKB is still linked (it is, after D1), behind an ACL teardown: wait for the sink's `gap_acl_disconn ... ` / `conn=0` heartbeat and read its `bonds_cleared=1 remaining=0` line BEFORE power-cycling the EVKB; a `remaining=1` means the removal has not happened yet. Bench build with `-DM2_BT_TARGET_NAME=EVKB-SINK -DM2_BT_LEGACY_PIN=ON`. D1: pair (`paired_by=pin`, `bond=saved`, type 0), power-cycle, expect `paired_by=stored` on the type-0 key with no inquiry. D2: type `forget` on the sink's console (expect `bonds_cleared=1`), power-cycle the EVKB, expect `bond_try`, `connect=ok`, `link_key_req … reply(stored type=0)`, `auth_complete: status=0x06` (or 0x05 — record which), `bond_rejected: status=0x0N -> erased`, `pin_code_req … -> 1234`, `link_key: … bond=saved`, `a2dp=ok`, `bonds=1 paired_by=pin`. D3: power-cycle once more, expect `paired_by=stored` again.

- [ ] **Step 5: Run E — recency.** Bench build with NO target name. With both bonds present (pair the Shokz last so the table reads [Shokz, sink], then pair the sink so it reads [sink, Shokz] — or check `bonds_boot=2` and the `bond_try` order), power the sink OFF and the Shokz ON. Power-cycle. Expected: `bond_try: bd=<sink> … attempts=3`, three `connect=page_timeout attempt=1..3` (or `connect=timeout … Create_Connection_Cancel`), `bond_try: bd=<shokz> … attempts=1`, `connect=ok`, `paired_by=stored`, `a2dp=ok`. Record the time from `hci_reset=ok` to `a2dp=ok` as the fall-through latency.

- [ ] **Step 6: acid_box witness.** Flash `build-bt/acid_box.elf` (the `M2_BT_OUT` bench build from Task 12), Shokz in normal mode with its bond from run A/B still valid. Expected: the panel renders and responds throughout the connect (idleUi), `bonds_boot=1`, `paired_by=stored`, `bt_streaming frames_per_pkt=…`, `bt_hb … pcmdrops=0`, `ACIDBOX_VSYNC … timeouts=0`. Append the capture and the `.text.itcm` before/after figures from Task 12 to `transcript_hw_evkb_bt.txt`. ★ Two things this run is the ONLY check of (Task 12 review): the example's static initialiser now runs from flash (the routing rule never matched before) — boot, first frame and the local audio must be unchanged; and a FIRST-PAIRING boot on this build writes the bond while the local SAI/vsync path is live, so if the witness has to pair first, expect and record any vsync timeout on THAT boot and assert `timeouts=0` on the stored-key boot that follows.

- [ ] **Step 7: Write the transcript sections and commit (evkb)**

Each run gets its raw capture (trimmed to the relevant lines, with the `hb` lines thinned) under a heading naming the run, the bench configuration, the date, and the verdict against the expected lines above; a prediction that did not hold is written as such, not adjusted.

```bash
cd ~/Development/rt1170/evkb && git add examples/audio/bt_tone_test/transcript_hw_evkb.txt examples/display/acid_box/transcript_hw_evkb_bt.txt && git commit -m "docs: NEW-34 piece 1 silicon -- runs A-E on bt_tone_test (stored-key reconnect, control, rejection path, recency) + acid_box witness

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 16: Close-out

**Files:**
- Modify: `~/Development/rt1170/evkb/docs/superpowers/specs/2026-09-05-bt-known-device-reconnect-design.md` (the `**Status:**` line)

- [ ] **Step 1:** Change the spec's status line to `**Status:** Implemented 2026-09-DD; QEMU gate green (sweep 129), silicon runs A-E recorded in `examples/audio/bt_tone_test/transcript_hw_evkb.txt`.` and note under Risks whether run C streamed.

- [ ] **Step 2:** Commit: `git add docs/superpowers/specs/2026-09-05-bt-known-device-reconnect-design.md && git commit -m "docs: NEW-34 piece 1 spec -- status implemented" ` (with the trailer).

- [ ] **Step 3:** Push evkb (`git push origin master`) and confirm M2Radio's push from Task 14 landed (`git -C ~/Development/M2Radio status -sb` shows no `ahead`).

- [ ] **Step 4:** Report on NEW-34 (a comment: what shipped, the sweep number, the five run verdicts with run C's outcome stated plainly, and that pieces 2-5 remain), and leave the issue's state to the owner.
