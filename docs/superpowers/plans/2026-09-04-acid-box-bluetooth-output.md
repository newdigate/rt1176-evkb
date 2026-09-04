# Acid Box over Bluetooth — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in `M2_BT_OUT` build flag to `examples/display/acid_box` that streams the synth's audio to a Shokz A2DP headset alongside the local WM8962 output, with the SynthUI still rendering.

**Architecture:** `AudioOutputBluetooth` (from `audio/bt_tone_test`) becomes a second sink on the acid voice. Because acid_box's graph is already clocked by the `AudioOutputI2S` SAI ISR, `AudioOutputBluetooth` gains a backward-compatible `setSelfClock(false)` mode: it encodes SBC from the ISR-driven `update()` and only drains from `poll()`. The BT bring-up (firmware download → HCI → `A2dpSource::connect`) runs in `setup()`; `poll()`/`service()` run in `loop()`. Everything is under `#if defined(M2_BT_OUT)`, so the default build and golden gate stay byte-identical.

**Tech Stack:** C++ (Teensyduino AudioStream, the imxrt1176 core), the `M2Radio` `bt/` A2DP stack, the custom QEMU gate harness, the RT1176 EVKB + IW416 + Shokz OpenMove.

**Repos:** `~/Development/rt1170/evkb` only (the `AudioOutputBluetooth` change is in the `bt_tone_test` example, and acid_box is an example — no `M2Radio` change, so no pin bump). Bench builds use the working-tree M2Radio via local-first resolution.

**Execution note:** Tasks 1–3 are subagent-executable (code + QEMU gates + byte-identical checks). **Task 4 is bench work** (real hardware + the Shokz). Task 5 is close-out.

---

### Task 1: `AudioOutputBluetooth::setSelfClock()` — externally-clocked mode

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/AudioOutputBluetooth.h`
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/AudioOutputBluetooth.cpp`

Today `poll()` self-clocks (calls `update_all()`). acid_box's I2S ISR already clocks the graph, so add a flag that makes `poll()` drain-only. Default `true` keeps bt_tone_test's behavior exactly.

- [ ] **Step 1: Add the flag and accessor to the header**

In `AudioOutputBluetooth.h`, in the `public:` section (after `void poll();`), add:

```cpp
    // Self-clock (default): poll() calls AudioStream::update_all() to drive the graph,
    // for a graph with no hardware audio clock (bt_tone_test).  Set false when another
    // sink (e.g. AudioOutputI2S) already clocks the graph from its DMA ISR -- then poll()
    // ONLY drains, and update() still encodes because the shared ISR walks this node.
    // Call before begin().
    void setSelfClock(bool on) { m_selfClock = on; }
```

And in the `private:` members (next to `uint32_t m_lastDrainUs = 0;`), add:

```cpp
    bool m_selfClock = true;                 // false = an external ISR clocks the graph; poll() drains only
```

- [ ] **Step 2: Gate `poll()`'s `update_all()` on the flag**

In `AudioOutputBluetooth.cpp`, replace the `poll()` body's update block. Find:

```cpp
    uint32_t now = micros();
    if ((int32_t)(now - m_nextUpdate) >= 0) {
        m_nextUpdate += m_usPerBlock;
        if ((int32_t)(now - m_nextUpdate) > (int32_t)(4 * m_usPerBlock)) m_nextUpdate = now + m_usPerBlock;
        AudioStream::update_all();
    }
```

Replace with (wrap the existing block in `if (m_selfClock)`):

```cpp
    uint32_t now = micros();
    if (m_selfClock) {
        // No hardware audio clock (bt_tone_test): drive the graph here, paced by micros().
        if ((int32_t)(now - m_nextUpdate) >= 0) {
            m_nextUpdate += m_usPerBlock;
            if ((int32_t)(now - m_nextUpdate) > (int32_t)(4 * m_usPerBlock)) m_nextUpdate = now + m_usPerBlock;
            AudioStream::update_all();
        }
    }
    // else: an external sink's ISR (AudioOutputI2S) already called update_all() this period,
    // so our update() has run and pushed a frame; poll() only drains it below.
```

The drain block below it (`if (m_pk.pending() >= ...)`) is unchanged.

- [ ] **Step 3: Verify bt_tone_test still self-clocks — rebuild + both gates green**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
cmake --build build >/dev/null 2>&1 && cmake --build build-media >/dev/null 2>&1 && echo BUILT
./run_qemu.sh; echo "card-absent exit=$?"
./run_qemu_media.sh; echo "media exit=$?"
```
Expected: `BUILT`, both gates PASS (`exit=0`). bt_tone_test never calls `setSelfClock`, so it self-clocks exactly as before — the media gate's blocks/packets are unchanged.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/audio/bt_tone_test/AudioOutputBluetooth.h examples/audio/bt_tone_test/AudioOutputBluetooth.cpp
git commit -m "feat(bt_tone_test): AudioOutputBluetooth::setSelfClock() -- ride an external audio clock

poll() self-clocks (update_all) only when set (default), for a graph with no
hardware clock.  With an AudioOutputI2S already clocking the graph from its SAI
ISR (acid_box), setSelfClock(false) makes poll() drain-only while update() still
encodes from the shared ISR walk.  bt_tone_test behaviour and gates unchanged.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: acid_box `CMakeLists.txt` — the `M2_BT_OUT` flag

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/CMakeLists.txt`

Add the flag, the BT import + firmware block (mirroring `bt_tone_test/CMakeLists.txt`), and the shared `AudioOutputBluetooth.cpp` source — all gated so OFF is byte-identical.

- [ ] **Step 1: Add the option and the BT firmware/knob block**

In `acid_box/CMakeLists.txt`, AFTER the `import_evkb_library(TouchPanel gt911)` line and BEFORE `teensy_add_executable(acid_box`, add:

```cmake
# --- Bluetooth A2DP audio output (opt-in) -----------------------------------
# OFF by default: nothing below compiles in and the ELF is byte-identical, so
# the golden run_qemu.sh gate is untouched.  ON links the M.2 BT stack and adds
# an AudioOutputBluetooth sink alongside the I2S/WM8962 path (see
# docs/superpowers/specs/2026-09-04-acid-box-bluetooth-output-design.md).
option(M2_BT_OUT "Stream the synth audio to a Bluetooth A2DP sink (bench)" OFF)
if(M2_BT_OUT)
    add_definitions(-DM2_BT_OUT=1)
    import_evkb_library(M2Radio sdio iw416 hci bt)

    # The BT bench knobs the headset path needs (same as audio/bt_tone_test).
    option(M2_BT_UART_DNLD "Run the BT-only V3 UART firmware download" ON)
    if(M2_BT_UART_DNLD)
    else()
        add_definitions(-DM2_BT_NO_UART_DNLD=1)
    endif()
    option(M2_BT_WAKE_PULSE "Pulse the boot-sleep wake line before the BT firmware download" ON)
    if(M2_BT_WAKE_PULSE)
        add_definitions(-DM2_BT_WAKE_PULSE=1)
    endif()
    option(M2_BT_RTS_FLOW "Hardware RXRTSE flow control on LPUART2 (Serial2.attachRts)" ON)
    set(M2_BT_RTS_WATER "1" CACHE STRING "MODIR RTSWATER watermark for M2_BT_RTS_FLOW (0..3)")
    if(M2_BT_RTS_FLOW)
        add_definitions(-DM2_BT_RTS_FLOW=1 -DM2_BT_RTS_WATER=${M2_BT_RTS_WATER})
    endif()
    set(M2_BT_TARGET_NAME "" CACHE STRING "Prefer the A2DP sink whose inquiry name contains this substring")
    if(NOT M2_BT_TARGET_NAME STREQUAL "")
        add_definitions(-DM2_BT_TARGET_NAME="${M2_BT_TARGET_NAME}")
    endif()
    option(M2_BT_LEGACY_PIN "Write_Simple_Pairing_Mode=0: legacy PIN pairing (1234)" OFF)
    if(M2_BT_LEGACY_PIN)
        add_definitions(-DM2_BT_LEGACY_PIN=1)
    endif()
    option(M2_BT_CONNECT_RETRY "Retry A2dpSource::connect() from loop() until it succeeds" ON)
    if(M2_BT_CONNECT_RETRY)
        add_definitions(-DM2_BT_CONNECT_RETRY=1)
    endif()
    option(M2_BT_FAST_BAUD "Switch HCI to M2_BT_FAST_BAUD_RATE after identity" ON)
    set(M2_BT_FAST_BAUD_RATE "3000000" CACHE STRING "Rate for M2_BT_FAST_BAUD")
    if(M2_BT_FAST_BAUD)
        add_definitions(-DM2_BT_FAST_BAUD=${M2_BT_FAST_BAUD_RATE})
    endif()

    # The BT-only UART firmware image (NOT vendored), same rule + synthetic fallback
    # as bt_tone_test/m2_hci_probe.
    set(M2RADIO_IW416_BT_FW "" CACHE FILEPATH "IW416 BT-only UART firmware .bin.inc from an NXP SDK")
    if(M2RADIO_IW416_BT_FW AND EXISTS "${M2RADIO_IW416_BT_FW}")
        message(STATUS "IW416 BT firmware: ${M2RADIO_IW416_BT_FW}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp"
"#include <stdint.h>\n"
"extern const uint8_t iw416_bt_fw[];\n"
"extern const uint32_t iw416_bt_fw_len;\n"
"const uint8_t iw416_bt_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_BT_FW}\"\n};\n"
"const uint32_t iw416_bt_fw_len = sizeof(iw416_bt_fw);\n")
        set(M2_BT_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp")
        add_definitions(-DHAVE_IW416_BT_FW=1)
    else()
        message(STATUS "IW416 BT firmware: not supplied -- using a 1 KB SYNTHETIC image")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp"
"#include <stdint.h>\n"
"extern const uint8_t iw416_bt_fw[];\n"
"extern const uint32_t iw416_bt_fw_len;\n"
"const uint8_t iw416_bt_fw[] __attribute__((section(\".progmem\"), used)) = {\n"
"#define R16(n) (uint8_t)(n),(uint8_t)(n+1),(uint8_t)(n+2),(uint8_t)(n+3),(uint8_t)(n+4),(uint8_t)(n+5),(uint8_t)(n+6),(uint8_t)(n+7),(uint8_t)(n+8),(uint8_t)(n+9),(uint8_t)(n+10),(uint8_t)(n+11),(uint8_t)(n+12),(uint8_t)(n+13),(uint8_t)(n+14),(uint8_t)(n+15)\n"
"#define R256(n) R16(n),R16(n+16),R16(n+32),R16(n+48),R16(n+64),R16(n+80),R16(n+96),R16(n+112),R16(n+128),R16(n+144),R16(n+160),R16(n+176),R16(n+192),R16(n+208),R16(n+224),R16(n+240)\n"
"R256(0), R256(1), R256(2), R256(3)\n"
"};\n"
"const uint32_t iw416_bt_fw_len = sizeof(iw416_bt_fw);\n")
        set(M2_BT_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp")
        add_definitions(-DHAVE_IW416_BT_FW=1 -DBT_FW_IS_SYNTHETIC=1)
    endif()
else()
    set(M2_BT_FW_SRC "")
endif()
```

- [ ] **Step 2: Add the shared source + firmware source to the target**

In `acid_box/CMakeLists.txt`, change the `teensy_add_executable(acid_box ...)` call to append the BT sources. Find:

```cmake
teensy_add_executable(acid_box
    acid_box.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
    ${_lvgl_dir}/port/lvgl_gt911_indev.cpp)
```

Replace with:

```cmake
teensy_add_executable(acid_box
    acid_box.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
    ${_lvgl_dir}/port/lvgl_gt911_indev.cpp
    ${M2_BT_OUT_SRCS})
```

And immediately BEFORE that `teensy_add_executable` call, add:

```cmake
# The BT-audio bridge is shared with audio/bt_tone_test (one copy, not a fork).
if(M2_BT_OUT)
    set(M2_BT_OUT_SRCS
        ${CMAKE_CURRENT_LIST_DIR}/../../audio/bt_tone_test/AudioOutputBluetooth.cpp
        ${M2_BT_FW_SRC})
    include_directories(${CMAKE_CURRENT_LIST_DIR}/../../audio/bt_tone_test)
else()
    set(M2_BT_OUT_SRCS "")
endif()
```

- [ ] **Step 3: Link M2Radio into the elf under the flag**

In `acid_box/CMakeLists.txt`, find:

```cmake
teensy_target_link_libraries(acid_box
    cores Audio Wire SPI SdFat SD SerialFlash MipiDisplay PXP TouchPanel)
```

Immediately AFTER it, add:

```cmake
if(M2_BT_OUT)
    teensy_target_link_libraries(acid_box M2Radio)
    target_include_directories(acid_box.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()
```

- [ ] **Step 4: Verify the DEFAULT build is byte-identical and the gate is green**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
cp build/acid_box.elf /tmp/acid_box.before.elf
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/dev/null 2>&1 && cmake --build build >/dev/null 2>&1 && echo BUILT
cmp /tmp/acid_box.before.elf build/acid_box.elf && echo "BYTE-IDENTICAL" || echo "CHANGED (expected identical -- CMake gated wrong)"
./run_qemu.sh; echo "acid_box gate exit=$?"
```
Expected: `BUILT`, `BYTE-IDENTICAL`, gate PASS. (Task 2 adds no source to the default build, so the ELF must not change.)

- [ ] **Step 5: Verify the BT build CONFIGURES (no firmware needed — synthetic)**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
rm -rf build-bt && cmake -B build-bt -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
    -DM2_BT_OUT=ON -DM2_BT_TARGET_NAME=Shokz 2>&1 | grep -iE "IW416 BT firmware|error" | head
echo "configure exit=$?"
```
Expected: `IW416 BT firmware: not supplied -- using a 1 KB SYNTHETIC image`, exit 0. (It will not fully build until Task 3 adds the `#if M2_BT_OUT` code — that is next.)

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/acid_box/CMakeLists.txt
git commit -m "build(acid_box): M2_BT_OUT flag -- opt-in BT A2DP audio output

OFF by default (ELF byte-identical, golden gate untouched).  ON imports the M.2
BT stack, the bench knobs, the IW416 BT firmware block, and the shared
AudioOutputBluetooth.cpp from audio/bt_tone_test.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: acid_box.cpp — the BT objects, wiring, bring-up and loop service

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/acid_box.cpp`

All additions are `#if defined(M2_BT_OUT)`. The BT preamble (board reset, wake, firmware download, HCI identity, fast-baud) is copied VERBATIM from `examples/audio/bt_tone_test/bt_tone_test.cpp` — that code is proven on silicon; do not re-derive it.

- [ ] **Step 1: Add the BT includes and the preamble helpers**

In `acid_box.cpp`, after the existing `#include "control_wm8962.h"` line, add:

```cpp
#if defined(M2_BT_OUT)
#include <HardwareSerial.h>
#include <Hci.h>
#include <HciEvents.h>
#include <HciTransport.h>
#include <HciPump.h>
#include <BtFwLoader.h>
#include <A2dpSource.h>
#include "AudioOutputBluetooth.h"
#endif
```

Then, at file scope (after the `#include`s, before `setup()`), COPY these items VERBATIM from `examples/audio/bt_tone_test/bt_tone_test.cpp`, each wrapped in one `#if defined(M2_BT_OUT)` / `#endif` block:
- the transport objects: `hciIo`, `hci`, `pump`, `btLoader`, `s_btFwSt`, `s_hciSt`, and the `#if defined(HAVE_IW416_BT_FW)` `extern` block;
- the board-reset macros + `m2ReleaseWifiReset()`;
- the HCI opcode constants `OP_RESET`/`OP_READ_LOCAL_VER`/`OP_READ_BUFFER_SIZE`/`OP_READ_BD_ADDR`, `printHex8`/`printHex16`/`printBd`, the counter helpers, `idleMs()`, `s_aclNum`, `probeIdentity()`;
- the `#if defined(M2_BT_FAST_BAUD)` `probeFastBaud()`;
- the CTS/wake macros + `m2WakeFromBootSleep()`, `m2AssertBtCts()`, `m2DumpSerial2()`, `btFirmwareDownload()`.

> DRY note: these are byte-for-byte the bt_tone_test preamble. Keeping them in step is the same discipline the m2_hci_probe/bt_tone_test header comments already call out. `CONSOLE` is `Serial1` in both files, so the copied code compiles unchanged.

Then add the A2DP + BT-audio objects and connections (acid_box's synth object is `acid`):

```cpp
#if defined(M2_BT_OUT)
static A2dpSource        src(hci, hciIo);
static AudioOutputBluetooth btout;
static AudioConnection   cBtL(acid, 0, btout, 0);
static AudioConnection   cBtR(acid, 0, btout, 1);   // mono acid duplicated to L+R
static uint32_t nowMs() { return millis(); }
static void btLog(void *, const char *s) { CONSOLE.println(s); }
static void onEvt(void *, uint8_t c, const uint8_t *p, uint8_t l) { src.onEvent(c, p, l); }
static void onAclThunk(void *, uint16_t h, const uint8_t *d, uint16_t l) { src.onAcl(h, d, l); }
static bool s_btBegun = false;
#endif
```

- [ ] **Step 2: Add the BT bring-up at the end of `setup()`**

In `acid_box.cpp`, at the very END of `setup()` (after `touch.begin()` and any final diag, immediately before the closing `}` of `setup()`), add:

```cpp
#if defined(M2_BT_OUT)
    // Bring up Bluetooth AFTER the panel/codec/touch are live: the local WM8962
    // audio already plays and the SynthUI is on screen during the ~30 s connect.
    hciIo.begin(115200);
    m2ReleaseWifiReset();
#if defined(M2_BT_WAKE_PULSE)
    m2WakeFromBootSleep();
#endif
#if defined(M2_BT_RTS_FLOW)
    Serial2.attachRts((uint8_t)M2_BT_RTS_WATER);
#endif
    btFirmwareDownload();
    hci.begin();
    pump.attach(hci);
    Hci::Reply r;
    for (uint8_t a = 1; a <= 10; a++) { s_hciSt = hci.run(OP_RESET, nullptr, 0, &r, 500, idleMs); if (s_hciSt == Hci::OK) break; }
    if (s_hciSt == Hci::OK) {
        CONSOLE.println("bt_hci_reset=ok");
        probeIdentity();
#if defined(M2_BT_FAST_BAUD)
        probeFastBaud();
#endif
    } else {
        CONSOLE.println("bt_hci_reset=fail");
    }
    hci.onEvent(onEvt, nullptr);
    hci.onAcl(onAclThunk, nullptr);
    src.setLog(btLog, nullptr); src.setPin("1234");
#if defined(M2_BT_LEGACY_PIN)
    src.setLegacyPin(true);
#endif
#endif
```

- [ ] **Step 3: Add the BT service + connect-retry to `loop()`**

In `acid_box.cpp`, at the TOP of `loop()` (first statements), add:

```cpp
#if defined(M2_BT_OUT)
    yield();                                   // drives the yield-attached HciPump (parses NCP/credits)
    src.service();
    if (s_btBegun) btout.poll();
    {
        static uint32_t lastTry = 0;
        if (!s_btBegun && (lastTry == 0 || millis() - lastTry >= 5000)) {
            lastTry = millis();
#if defined(M2_BT_TARGET_NAME)
            A2dpSource::Result rr = src.connect(M2_BT_TARGET_NAME, s_aclNum, nowMs, idleMs);
#else
            A2dpSource::Result rr = src.connect(nullptr, s_aclNum, nowMs, idleMs);
#endif
            CONSOLE.print("a2dp_try="); CONSOLE.println(A2dpSource::resultName(rr));
            if (rr == A2dpSource::OK) {
                btout.setSelfClock(false);     // the I2S SAI ISR clocks the graph; poll() only drains
                btout.begin(src);
                s_btBegun = true;
                CONSOLE.print("bt_streaming frames_per_pkt="); CONSOLE.print(btout.framesPerPacket());
                CONSOLE.print(" media_mtu="); CONSOLE.println(src.mediaMtu());
            }
        }
    }
    // Heartbeat so a bench capture shows liveness + drops (throttled; acid_box's
    // own loop has no 1 Hz print).
    {
        static uint32_t last = 0;
        if (s_btBegun && millis() - last >= 1000) {
            last = millis();
            CONSOLE.print("bt_hb blocks="); CONSOLE.print(btout.blocks());
            CONSOLE.print(" packets="); CONSOLE.print(btout.packets());
            CONSOLE.print(" drops="); CONSOLE.print(btout.drops());
            CONSOLE.print(" hw="); CONSOLE.print(btout.queueHighWater());
            CONSOLE.print(" audiomax="); CONSOLE.println(AudioMemoryUsageMax());
        }
    }
#endif
```

> The connect is deferred to `loop()` (like bt_tone_test's `M2_BT_CONNECT_RETRY`): the UI renders first, then the first attempt blocks ~30 s, then streaming + UI resume. `A2dpSource::connect` internally calls `idleMs()` (a `delay(1)` that services the pump), so the HCI keeps running during the connect.

- [ ] **Step 4: Verify the DEFAULT build is STILL byte-identical + gate green**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
cmake --build build >/dev/null 2>&1 && cmp /tmp/acid_box.before.elf build/acid_box.elf && echo "BYTE-IDENTICAL" || echo "CHANGED"
./run_qemu.sh; echo "acid_box gate exit=$?"
```
Expected: `BYTE-IDENTICAL` (every addition is under `#if defined(M2_BT_OUT)`, which the default build does not define), gate PASS.

- [ ] **Step 5: Verify the BT build COMPILES clean (synthetic firmware)**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
cmake --build build-bt 2>&1 | grep -iE "error|Built target acid_box.elf" | tail -3
```
Expected: `Built target acid_box.elf`, no errors.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/acid_box/acid_box.cpp
git commit -m "feat(acid_box): M2_BT_OUT -- stream the synth to a Bluetooth headset (dual output)

Under M2_BT_OUT (default OFF, ELF byte-identical): AudioOutputBluetooth is a
second sink on the acid voice, externally clocked by the I2S SAI ISR
(setSelfClock(false)); the BT stack is brought up in setup() and serviced in
loop() with a deferred connect-retry.  Local WM8962 audio + Shokz headset play
the same synth.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: BENCH — silicon acceptance (Shokz + real firmware)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/transcript_hw_evkb.txt` (append, keep the existing capstone transcript)

Interactive bench work. Bench hygiene from the headset sessions: real BT firmware, a full board POWER-CYCLE if `LinkServer run`/`flash` hangs at connect, `flash load`/`verify` then SW4 to free-run, the Shokz factory-reset + explicit pairing mode + Mac Bluetooth OFF per attempt, a reopen-on-drop VCOM reader, and NEVER trace media on the console (the observer-effect crackle) — this example has no ACL trace, good.

- [ ] **Step 1: Build the bench image (real firmware, SSP, Shokz)**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
FW=/Users/nicholasnewdigate/Development/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416/uartIW416_bt.bin.inc
rm -rf build-bt && cmake -B build-bt -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DM2_BT_OUT=ON -DM2_BT_TARGET_NAME=Shokz -DM2RADIO_IW416_BT_FW=$FW 2>&1 | grep -iE "IW416 BT firmware|error"
cmake --build build-bt 2>&1 | grep -iE "error|Built target acid_box.elf" | tail -2
```
Expected: `IW416 BT firmware: …/uartIW416_bt.bin.inc`, `Built target acid_box.elf`.

- [ ] **Step 2: Flash + free-run (VCOM-free flash, then SW4)**

```bash
pkill LinkServer redlinkserv crt_emu_cm_redlink 2>/dev/null; sleep 2
gtimeout 220 /Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-bt/acid_box.elf
gtimeout 150 /Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build-bt/acid_box.elf
pkill LinkServer redlinkserv crt_emu_cm_redlink 2>/dev/null
```
Then start the reopen-on-drop VCOM reader and press **SW4**. If `flash` hangs at connect (0-byte output past ~90 s), a full board POWER-CYCLE clears it (a debug-USB replug does not — it only resets the MCU-Link probe). Do NOT kill `crt_emu` mid-connect (it wedges the SWD wire; another power-cycle needed).

- [ ] **Step 3: Prep the Shokz and verify streaming**

Factory-reset the Shokz, put it in explicit pairing mode, Mac Bluetooth OFF, then observe the VCOM: `a2dp_try=ok`, then `bt_streaming frames_per_pkt=…`, then `bt_hb blocks=… packets=… drops=0 …` climbing. Confirm by EAR the acid bass plays on the Shokz AND (dual) on the local codec, the SynthUI renders, and a touch gesture changes the sound.

- [ ] **Step 4: Acceptance checks + the CPU-headroom reading**

- Clean, continuous audio on the headset for ≥ 2 min, `bt_hb … drops=0` sustained.
- `audiomax=` (AudioMemoryUsageMax) stays below the pool (24). If it pins at 24, bump `AudioMemory(24)` in `setup()` to a value with headroom (e.g. 32) and re-flash.
- The SynthUI still renders and touch still responds (note any frame-rate/latency degradation). If audio drops or the UI stutters badly, the spec's fallback is BT-only: skip `wm.enable()`/`wm.volume()` (or omit the `acid → out` connections) under a sub-flag and re-test — record the outcome either way.

- [ ] **Step 5: Capture the transcript and commit**

Append the bring-up + a streaming heartbeat window (and the audiomax reading) to `examples/display/acid_box/transcript_hw_evkb.txt` under a dated `=== ACID BOX OVER BLUETOOTH (M2_BT_OUT) ===` header.

```bash
cd ~/Development/rt1170/evkb
git add examples/display/acid_box/transcript_hw_evkb.txt
git commit -m "test(acid_box): silicon -- synth audio over Bluetooth to a Shokz headset (dual output)

M2_BT_OUT build: the acid bass streams to a Shokz OpenMove over A2DP while the
SynthUI renders and the local WM8962 plays.  drops=0 sustained; AudioMemory and
render/touch headroom recorded.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Close-out — sweep, audit, records

**Files:**
- Modify: `~/Development/rt1170/evkb/CLAUDE.md` (a dated note; the gate count is unchanged — no new gate)

- [ ] **Step 1: Full sweep + audit (never concurrent)**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh   # via a short-path symlink if the checkout path is long
# after it finishes:
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh 2>&1 | tail -3
```
Expected: `gates: 128 passed`, exit 0, `0 SKIP` (no new gate; acid_box's golden gate and bt_tone_test's gates unchanged); `LICENSE-AUDIT: PASS`. The `acid_box` `build-bt/` bench dir is NOT a gate (no `boards` change, no new `run_qemu*.sh`), so discovery is unaffected — confirm `-l` still reports 128.

- [ ] **Step 2: CLAUDE.md note**

Append a dated `★` note under the BT-3 measured entries: acid_box gained an opt-in `M2_BT_OUT` dual-output path (I2S + A2DP headset); default build byte-identical, no new gate; the one library change is `AudioOutputBluetooth::setSelfClock()`; silicon result (clean tone on the Shokz while the SynthUI runs, drops=0, AudioMemory/CPU headroom reading).

```bash
cd ~/Development/rt1170/evkb
git add CLAUDE.md && git commit -m "docs: acid_box M2_BT_OUT dual-output (synth over Bluetooth) -- silicon result

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git push origin master
```

- [ ] **Step 3: Memory + NEW-9**

Update `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/m2-bluetooth-a2dp-programme.md` (+ the MEMORY.md hook): acid_box streams over BT (dual output), the `setSelfClock` external-clock pattern, and the silicon/CPU-headroom result. Post a NEW-9 comment with the outcome.

---

## Notes for the executor

- **Everything acid_box-side is under `#if defined(M2_BT_OUT)`.** The byte-identical check in Tasks 2 and 3 is the guard that the default build and its golden gate never move — run it, do not skip it.
- **The BT preamble is COPIED VERBATIM from bt_tone_test.cpp** — do not re-derive HCI/firmware code; it is proven on silicon. `CONSOLE` is `Serial1` in both.
- **No M2Radio change, no pin bump.** The only library-level change (`setSelfClock`) lives in the bt_tone_test example's `AudioOutputBluetooth`, shared by reference.
- **CPU headroom is the real risk and is silicon-only** (Task 4). If it fails, the fallback (BT-only) is in the spec; record the measurement regardless.
- **Bench hygiene:** power-cycle (not debug-USB replug) clears a connect wedge; `flash load`+`verify`+SW4 to free-run; never kill `crt_emu` mid-connect; never console-trace media.
