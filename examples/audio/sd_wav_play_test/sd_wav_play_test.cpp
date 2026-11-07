#include "Arduino.h"
#include "HardwareSerial.h"
#include <SD.h>
#include "AudioStream.h"
#include "play_sd_wav.h"
#include "output_i2s.h"
// Board codec. Audio.h pulls in every codec driver EXCEPT the WM8962 (control_wm8960.h is in
// its include list, control_wm8962.h is not), so the EVKB header is named explicitly. Nosh
// carries an SGTL5000 on LPI2C5 instead: its driver defaults to Wire2 under ARDUINO_NOSH and
// declares the 512*Fs / 32*Fs ratios the RT1176 SAI config produces, so only the type changes.
#if defined(ARDUINO_NOSH)
#include "control_sgtl5000.h"
typedef AudioControlSGTL5000 BoardCodec;
#else
#include "control_wm8962.h"
typedef AudioControlWM8962 BoardCodec;
#endif

// AudioPlaySdWav (0 in / 2 out) -> AudioOutputI2S (SAI1 TX DMA) -> WM8962 -> J101.
// The SAI1 TX DMA isr pends update_all(), which runs AudioPlaySdWav::update() ->
// the blocking 512-byte SD read -> transmit. In QEMU every SAI1_TDR0 write is
// mirrored to the sai1-tap chardev; check_tap.py asserts it equals TEST.WAV.
AudioPlaySdWav     playWav;
AudioOutputI2S     out;
BoardCodec         wm;                          // I2C/Wire2 codec (HW fidelity)
AudioConnection    cL(playWav, 0, out, 0);      // left
AudioConnection    cR(playWav, 1, out, 1);      // right

void setup() {
  Serial1.begin(115200);
  while (!Serial1 && millis() < 2000) {}
  Serial1.println("=== BOOT ===");
  Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n",
                 "sd_wav_play_test", 1, __DATE__, __TIME__);
  AudioMemory(30);
  wm.enable();

  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial1.println("SD_WAV_MOUNT=FAIL");
    Serial1.println("SD_WAV_PLAY=FAIL");
    Serial1.println("SD_WAV_DONE=FAIL");
    return;
  }
  Serial1.println("SD_WAV_MOUNT=PASS");

  bool started = playWav.play("TEST.WAV");
  Serial1.print("SD_WAV_PLAY="); Serial1.println(started ? "PASS" : "FAIL");
  if (!started) { Serial1.println("SD_WAV_DONE=FAIL"); return; }

  // Wait for playback to finish. NOTE: right after play() the node is in a
  // header-PARSE state (8-12), for which isPlaying() (state<8) is FALSE -- so
  // do NOT loop on isPlaying(). isStopped() (state==14/STATE_STOP) is only set
  // at EOF (or parse failure), so it correctly spans parse+play until done.
  uint32_t t0 = millis();
  while (!playWav.isStopped() && (millis() - t0) < 15000) {
    yield();
  }
  Serial1.print("SD_WAV_DONE=");
  Serial1.println(playWav.isStopped() ? "PASS" : "FAIL(timeout)");
  Serial1.print("info positionMillis="); Serial1.println(playWav.positionMillis());
  Serial1.print("info lengthMillis=");   Serial1.println(playWav.lengthMillis());
}
void loop() {}
