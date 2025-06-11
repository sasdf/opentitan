#!/bin/bash
set -euo pipefail

COVERAGE_OUTPUT_DIR="/tmp/$USER/coverage/"

TARGETS=(

//sw/device/silicon_creator/rom_ext/e2e/rescue/std_utils:xmodem_protocol_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:next_slot_spidfu_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:next_slot_xmodem_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:primary_slot_spidfu_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:primary_slot_xmodem_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_enter_on_fail_spidfu_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_enter_on_fail_xmodem_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_firmware_protocol_0_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_firmware_slot_a_spidfu_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_firmware_slot_a_xmodem_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_firmware_slot_b_spidfu_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_firmware_slot_b_xmodem_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_boot_log_spidfu_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_boot_log_xmodem_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_device_id_spidfu_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_device_id_xmodem_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_rate_test_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_rom_ext_slot_a_update_slot_a_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_rom_ext_slot_a_update_slot_b_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_rom_ext_slot_b_update_slot_a_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_rom_ext_slot_b_update_slot_b_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:spidfu_restricted_commands_fpga_cw340_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:xmodem_restricted_commands_fpga_cw340_rom_ext


# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_inactivity_timeout_fpga_cw340_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_inactivity_timeout_preserved_reset_reason_fpga_cw340_rom_ext

# //sw/device/silicon_creator/rom_ext/e2e/rescue/std_utils:usbdfu_protocol_fpga_cw340_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:next_slot_usbdfu_fpga_cw340_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:primary_slot_usbdfu_fpga_cw340_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_enter_on_fail_usbdfu_fpga_cw340_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_firmware_slot_a_usbdfu_fpga_cw340_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_firmware_slot_b_usbdfu_fpga_cw340_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_boot_log_usbdfu_fpga_cw340_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_device_id_usbdfu_fpga_cw340_rom_ext


# //sw/device/silicon_creator/rom_ext/e2e/rescue/std_utils:xmodem_protocol_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:spidfu_restricted_commands_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:xmodem_restricted_commands_fpga_hyper310_rom_ext

# //sw/device/silicon_creator/rom/e2e/watchdog:watchdog_disable_rma_fpga_cw340_instrumented_rom
# //sw/device/silicon_creator/rom/e2e/watchdog:watchdog_enable_rma_fpga_cw340_instrumented_rom

# //sw/device/silicon_creator/rom_ext/e2e/secver:secver_write_test_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/verified_boot:isfb_boot_test_fpga_hyper310_rom_ext

# //sw/device/tests:flash_ctrl_clock_freqs_test_fpga_cw310_sival_rom_ext
# //sw/device/tests:flash_ctrl_ops_test_fpga_cw310_sival_rom_ext

# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_boot_log_spidfu_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_boot_log_xmodem_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_device_id_spidfu_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_device_id_xmodem_fpga_hyper310_rom_ext


# //sw/device/silicon_creator/rom_ext/e2e/rescue:next_slot_usbdfu_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:primary_slot_usbdfu_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_enter_on_fail_usbdfu_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_firmware_slot_a_usbdfu_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_firmware_slot_b_usbdfu_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_boot_log_usbdfu_fpga_hyper310_rom_ext
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_device_id_usbdfu_fpga_hyper310_rom_ext

# TIMEOUT: Looping
# //sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_inactivity_timeout_preserved_reset_reason_fpga_hyper310_rom_ext


    # "//sw/device/silicon_creator/rom_ext:rom_ext_prod_dice_cwt_spidfu_baseline_coverage"
    # //sw/device/tests:uart_smoketest_fpga_cw340_test_rom
    # //sw/device/tests:uart_smoketest_fpga_cw340_instrumented_rom
    # //sw/device/lib/crypto/drivers:aes_test_fpga_cw310_rom_with_fake_keys
    # //sw/device/lib/base:crc32_unittest
    # //sw/device/tests/crypto/cryptotest:hmac_sha256_kat_fpga_cw340_test_rom
    # //sw/device/tests:uart_smoketest_fpga_cw340_rom_ext
    # //sw/device/silicon_creator/rom_ext/e2e/dice_chain:no_refresh_dice_x509_test_fpga_cw340_rom_ext
    # //sw/device/tests:rv_core_ibex_isa_test_prod_fpga_cw310_rom_with_fake_keys
)

BAZEL_ARGS=(
    --test_output=streamed
    --test_timeout=600
    --copt=-Wno-error
    --copt=-Wno-enum-constexpr-conversion
    # --cache_test_results=no
    --config=ot_coverage
    --local_test_jobs=1
    --notest_runner_fail_fast
    --keep_going
    # --jobs=1
    # --subcommands
)

COVERAGE_DAT="bazel-out/_coverage/_coverage_report.dat"

rm -f "${COVERAGE_DAT}"

./bazelisk.sh coverage "${TARGETS[@]}" "${BAZEL_ARGS[@]}" "$@" || true

GENHTML_ARGS=(
    --prefix "${PWD}"
    --ignore-errors unsupported
    --ignore-errors inconsistent
    --ignore-errors category
    # --ignore-errors corrupt
    --html-epilog sw/device/coverage/report_epilog.html
    --output "${COVERAGE_OUTPUT_DIR}"
    "${COVERAGE_DAT}"
)

genhtml "${GENHTML_ARGS[@]}"
