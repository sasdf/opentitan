#include "sw/device/lib/base/macros.h"

/* Instrumented ROM magic value.
 *
 * This value is placed at the very beginning of the instrumented ROM image
 * after vector table. This can be used to check the existence of an
 * instrumented ROM on the Flash.
 */
OT_USED
OT_SECTION(".instrumented_rom_magic")
char instrumented_rom_magic[4] = "IROM";
