#!/bin/bash
set -euo pipefail

COVERAGE_OUTPUT_DIR="/tmp/${USER}/all_coverage/"

BASELINES=(
    "//sw/device/silicon_creator/rom_ext:rom_ext_prod_dice_cwt_spidfu_baseline_coverage"
    "//sw/device/silicon_creator/rom_ext:rom_ext_dice_x509_slot_virtual_baseline_coverage"
    "//sw/device/silicon_creator/rom_ext/imm_section:main_binaries_dice_cwt_slot_virtual_baseline_coverage"
    "//sw/device/silicon_creator/rom_ext/imm_section:main_binaries_dice_x509_slot_virtual_baseline_coverage"
    "//sw/device/silicon_creator/rom:instrumented_mask_rom_baseline_coverage"
)

TARGETS=()

EXTRA_TESTS=(
)

CW310_ROM_EXT_TESTS=(
  '//sw/device/silicon_creator/rom_ext/e2e/dice_chain:no_refresh_dice_cwt_test_fpga_cw310_rom_ext'
)

CW310_ROM_WITH_FAKE_KEYS_TESTS=(
  '//sw/device/silicon_creator/lib/sigverify:spx_verify_functest_fpga_cw310_rom_with_fake_keys'
  '//sw/device/silicon_creator/lib:boot_data_functest_fpga_cw310_rom_with_fake_keys'
  '//sw/device/silicon_creator/lib:otbn_boot_services_functest_fpga_cw310_rom_with_fake_keys'
  '//sw/device/tests:kmac_smoketest_fpga_cw310_rom_with_fake_keys'
  '//sw/device/tests:pwrmgr_normal_sleep_all_wake_ups_fpga_cw310_rom_with_fake_keys'
)

CW340_INSTRUMENTED_ROM_PROD_TESTS=(
  '//sw/device/silicon_creator/rom/e2e/immutable_rom_ext_section:immutable_section_exec_enabled_hash_valid_virtual_b_fpga_cw340_instrumented_rom_prod'
)

CW340_INSTRUMENTED_ROM_TESTS=(
  '//sw/device/silicon_creator/rom/e2e/address_translation:rom_ext_a_flash_a_bad_addr_trans_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/boot_policy_bad_manifest:boot_policy_bad_manifest_dev_rollback_b_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/boot_policy_big_image:boot_policy_big_image_test_unlocked0_bigger_than_64k_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/boot_policy_newer:boot_policy_newer_prod_end_a_1_b_1_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/keymgr:rom_e2e_keymgr_init_otp_meas_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/reset_reason:reset_reason_check_enabled_with_fault_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/retention_ram:rom_e2e_ret_ram_keep_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/rom_e2e_bootstrap_entry:e2e_bootstrap_entry_test_unlocked0_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/shutdown_alert:shutdown_alert_dev_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/sigverify_key_type:sigverify_key_type_dev_fake_spx_test_key_0_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/sigverify_key_type:sigverify_key_type_prod_end_fake_ecdsa_test_key_0_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/sigverify_key_type:sigverify_key_type_prod_fake_spx_dev_key_0_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/sigverify_key_type:sigverify_key_type_rma_fake_spx_dev_key_0_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/sigverify_key_validity:sigverify_key_validity_spx_blank_test_unlocked0_fake_ecdsa_dev_key_0_fake_spx_dev_key_0_fpga_cw340_instrumented_rom'
  '//sw/device/silicon_creator/rom/e2e/sigverify_key_validity:sigverify_key_validity_spx_blank_test_unlocked0_fake_ecdsa_test_key_0_fake_spx_test_key_0_fpga_cw340_instrumented_rom'
)

CW340_ROM_EXT_TESTS=(
  '//sw/device/silicon_creator/rom_ext/e2e/verified_boot:bad_manifest_test_fpga_cw340_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/verified_boot:key_dev_hybrid_spx_prehashed_fpga_cw340_rom_ext'
)

CW340_TEST_ROM_TESTS=(
  '//sw/device/tests/crypto/cryptotest:drbg_kat_fpga_cw340_test_rom'
)

HYPER310_ROM_EXT_TESTS=(
  '//sw/device/silicon_creator/rom_ext/e2e/boot_svc:boot_svc_bad_next_test_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/boot_svc:boot_svc_empty_test_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/boot_svc:boot_svc_min_sec_ver_test_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/flash_ecc_error:a_corrupt_b_valid_manifest_length_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/flash_ecc_error:a_valid_b_corrupt_manifest_entry_point_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/flash_ecc_error:flash_exc_handler_disabled_test_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/ownership:bad_locked_update_no_exec_test_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/ownership:flash_error_test_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/ownership:flash_permission_test_slot_ab_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/ownership:newversion_badlock_test_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/ownership:newversion_update_test_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/ownership:transfer_pq_to_pq_test_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/rescue:next_slot_spidfu_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_enter_on_fail_spidfu_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_inactivity_timeout_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_rate_test_fpga_hyper310_rom_ext'
)

UNIT_TESTS=(
  '//sw/device/lib/base:memory_unittest'
  '//sw/device/lib/crypto/impl:status_unittest'
  '//sw/device/silicon_creator/lib/boot_svc:boot_svc_header_unittest'
  '//sw/device/silicon_creator/lib/drivers:alert_unittest'
  '//sw/device/silicon_creator/lib/drivers:ast_unittest'
  '//sw/device/silicon_creator/lib/drivers:flash_ctrl_unittest'
  '//sw/device/silicon_creator/lib/drivers:keymgr_unittest'
  '//sw/device/silicon_creator/lib/drivers:kmac_unittest'
  '//sw/device/silicon_creator/lib/drivers:lifecycle_unittest'
  '//sw/device/silicon_creator/lib/drivers:otbn_unittest'
  '//sw/device/silicon_creator/lib/drivers:rnd_unittest'
  '//sw/device/silicon_creator/lib/drivers:spi_device_unittest'
  '//sw/device/silicon_creator/lib/drivers:watchdog_unittest'
  '//sw/device/silicon_creator/lib/ownership:ownership_activate_unittest'
  '//sw/device/silicon_creator/lib/ownership:ownership_unlock_unittest'
  '//sw/device/silicon_creator/lib:boot_data_unittest'
  '//sw/device/silicon_creator/lib:boot_log_unittest'
  '//sw/device/silicon_creator/lib:dbg_print_unittest'
  '//sw/device/silicon_creator/lib:epmp_unittest'
  '//sw/device/silicon_creator/lib:manifest_unittest'
  '//sw/device/silicon_creator/rom:boot_policy_unittest'
)


TEST_GROUPS=(
  'EXTRA_TESTS'
  'CW310_ROM_EXT_TESTS'
  'CW310_ROM_WITH_FAKE_KEYS_TESTS'
  'CW340_INSTRUMENTED_ROM_PROD_TESTS'
  'CW340_INSTRUMENTED_ROM_TESTS'
  'CW340_ROM_EXT_TESTS'
  'CW340_TEST_ROM_TESTS'
  'HYPER310_ROM_EXT_TESTS'
  'UNIT_TESTS'
)

BAZEL_ARGS=(
    --test_output=streamed
    # --test_timeout=600
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

source ./run_all_coverage_impl.sh
