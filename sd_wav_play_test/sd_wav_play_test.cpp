#include "Arduino.h"
#include "HardwareSerial.h"
#include <SD.h>
#include "AudioStream.h"
#include "play_sd_wav.h"
#include "output_i2s.h"
#include "control_wm8962.h"

// AudioPlaySdWav (0 in / 2 out) -> AudioOutputI2S (SAI1 TX DMA) -> WM8962 -> J101.
// The SAI1 TX DMA isr pends update_all(), which runs AudioPlaySdWav::update() ->
// the blocking 512-byte SD read -> transmit. In QEMU every SAI1_TDR0 write is
// mirrored to the sai1-tap chardev; check_tap.py asserts it equals TEST.WAV.
AudioPlaySdWav     playWav;
AudioOutputI2S     out;
AudioControlWM8962 wm;                          // I2C/Wire2 codec (HW fidelity)
AudioConnection    cL(playWav, 0, out, 0);      // left
AudioConnection    cR(playWav, 1, out, 1);      // right

void setup() {
  Serial1.begin(115200);
  while (!Serial1) {}
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
