#pragma once
// TinyUSB config, picked up through CFG_TUSB_CONFIG_FILE in platformio.ini.
// Where the Adafruit TinyUSB library is on the include path this is its
// RP2040 config with the USB host stack turned off (this board has no
// MAX3421 or PIO USB host port); device classes are unchanged. Elsewhere
// (the core's own sources) it is the core's usual tusb_config.h.
#if __has_include("arduino/ports/rp2040/tusb_config_rp2040.h")
#include "arduino/ports/rp2040/tusb_config_rp2040.h"
#undef CFG_TUH_ENABLED
#define CFG_TUH_ENABLED 0
#undef CFG_TUH_MAX3421
#undef CFG_TUH_RPI_PIO_USB
#else
#include "tusb_config.h"
#endif
