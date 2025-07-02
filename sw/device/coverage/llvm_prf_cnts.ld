/* Sections for LLVM profiling. */

SECTIONS {
  /* BSS Sections for LLVM coverage runtime. */
  __llvm_prf_noinit (NOLOAD) : ALIGN(8) {
    KEEP(*(__llvm_prf_noinit))

    ASSERT(
      DEFINED(_ot_coverage_enabled) || SIZEOF(__llvm_prf_noinit) == 0,
      "__llvm_prf_noinit section must be empty when coverage is disabled.");
  } > ram_main

  /* LLVM coverage counters. */
  . = ALIGN(8);
  __llvm_prf_cnts : AT(0x0) ALIGN(8) {
    /* Set gap fill pattern to 0xa5 */
    FILL(0xa5a5a5a5);

    __llvm_prf_cnts_start = .;
    KEEP(*(__llvm_prf_cnts_asm))
    KEEP(*(__llvm_prf_cnts))
    __llvm_prf_cnts_values_end = .;
    . = ALIGN(8);
    __llvm_prf_cnts_end = .;

    ASSERT(
      _stack_start > ABSOLUTE(.),
      "Allocated RAM overflow to stack region");

    ASSERT(
      DEFINED(_ot_coverage_enabled) || SIZEOF(__llvm_prf_cnts) == 0,
      "__llvm_prf_cnts section must be empty when coverage is disabled.");
  } > ram_main

  /**
   * This section, normally used for LLVM's compiler-rt coverage runtime
   * initialization, is discarded because a custom coverage runtime has been
   * implemented.
   */
  /DISCARD/ : {
      *(.init_array)
      *(.init_array.*)
  }

  /**
   * Profile data and names are moved to the INFO section due to device-side
   * memory constraints. These data will be used with the counters on the host
   * to reconstruct the LLVM profile.
   */
  __llvm_prf_data 0x0 (INFO) : ALIGN(8) {
    __llvm_prf_data_start = .;
    KEEP(*(__llvm_prf_data))
    __llvm_prf_data_end = .;

    ASSERT(
      DEFINED(_ot_coverage_enabled) || SIZEOF(__llvm_prf_data) == 0,
      "__llvm_prf_data section must be empty when coverage is disabled.");
  }

  __llvm_prf_names 0x0 (INFO) : ALIGN(8) {
    __llvm_prf_names_start = .;
    KEEP(*(__llvm_prf_names))
    __llvm_prf_names_end = .;

    ASSERT(
      DEFINED(_ot_coverage_enabled) || SIZEOF(__llvm_prf_names) == 0,
      "__llvm_prf_names section must be empty when coverage is disabled.");
  }
}
