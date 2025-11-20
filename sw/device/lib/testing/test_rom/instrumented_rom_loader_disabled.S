/**
 * Skip instrumented ROM loader.
 *
 */

  // NOTE: The "ax" flag below is necessary to ensure that this section
  // is allocated executable space in ROM by the linker.
  .section .crt, "ax"
  .balign 4
  .global _try_instrumented_rom
  .type _try_instrumented_rom, @function

_try_instrumented_rom:
  ret
  .size _try_instrumented_rom, .-_try_instrumented_rom
