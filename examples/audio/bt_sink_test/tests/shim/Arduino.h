// HOST SHIM (NEW-41).  AudioInputBluetooth.h includes <Arduino.h> only so that AudioStream.h can see
// F_CPU_ACTUAL / IRQ_NUMBER_t on the target; nothing in the node's own code touches the Arduino API.  So the
// host build gets this, which supplies the freestanding headers and nothing else -- and deliberately does NOT
// define __IMXRT1176__, so AudioInputBluetooth.h takes its no-op audioPllTrimPpm() fallback and the node's
// PLL writes become observable-by-absence rather than a link error.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
