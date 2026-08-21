#include "sd_card.h"
#include "pins.h"

SdFs sd;

bool sdCardBegin() {
    // SdioConfig(clkPin, cmdPin, dat0Pin, clkDiv) -- DAT1..DAT3 must be
    // (and here are) the next three consecutive GPIOs after dat0Pin.
    //
    // clkDiv is a PIO state-machine divider applied to whatever the
    // CURRENT system clock is, not an absolute SD bus frequency -- the
    // driver does NOT self-adjust it for F_CPU (unlike the very first
    // init handshake, which does compute its own divider from
    // clock_get_hz(clk_sys)). Left at the default 1.0, the SD bus clock
    // scales directly with F_CPU. When F_CPU was raised from 133MHz to
    // 200MHz (see platformio.ini) to fix S3M/XM mixing-throughput
    // underruns, this silently pushed the SD bus ~50% faster too and
    // produced real-hardware symptoms consistent with SD bus errors: a
    // brief run of catastrophically slow reads (each _file.read() taking
    // 18-22ms instead of ~300us) a few seconds into an otherwise-clean
    // playback session, self-recovering after a few seconds -- a classic
    // CRC-error-retry signature, not a mixing-cost problem (see
    // S3mPlayer's own diagnostic instrumentation, which showed this same
    // window's cost concentrated entirely in chunkUs, not mixUs).
    // Scaling clkDiv by F_CPU/133000000 keeps the SD bus at the same
    // effective clock rate it ran at when 133MHz was already known
    // stable, while still letting everything else benefit from the
    // faster CPU.
    return sd.begin(SdioConfig(SDIO_CLK, SDIO_CMD, SDIO_D0, (float)F_CPU / 133000000.0f));
}
