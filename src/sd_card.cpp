#include "sd_card.h"
#include "pins.h"

SdFs sd;

bool sdCardBegin() {
    // SdioConfig(clkPin, cmdPin, dat0Pin) -- DAT1..DAT3 must be (and here
    // are) the next three consecutive GPIOs after dat0Pin.
    return sd.begin(SdioConfig(SDIO_CLK, SDIO_CMD, SDIO_D0));
}
