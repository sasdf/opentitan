  /**
   * Build ID for coverage profile correlation.
   */
  .note.gnu.build-id : ALIGN(4) {
    _build_id_start = .;
    *(.note.build-id)
    *(.note.gnu.build-id)
    _build_id_end = .;

    ASSERT(
      DEFINED(_ot_coverage_enabled) || SIZEOF(.note.gnu.build-id) == 0,
      "Build ID section must be empty when coverage is disabled.");
  } > coverage_flash
