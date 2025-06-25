CW310_ROM_EXT_TESTS=(
  '//sw/device/silicon_creator/rom_ext/e2e/dice_chain:no_refresh_dice_cwt_test_fpga_cw310_rom_ext'
)

CW310_SIVAL_ROM_EXT_TESTS=(
  '//sw/device/silicon_creator/lib/ownership:owner_verify_functest_fpga_cw310_sival_rom_ext'
)

CW340_INSTRUMENTED_ROM_TESTS=(
  '//sw/device/silicon_creator/rom/e2e/sigverify_usage_constraints:sigverify_usage_constraint_device_id_match_fpga_cw340_instrumented_rom'
)

HYPER310_ROM_EXT_TESTS=(
  '//sw/device/silicon_creator/rom_ext/e2e/flash_ecc_error:a_valid_b_corrupt_manifest_ecdsa_public_key_fpga_hyper310_rom_ext'
  '//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_get_boot_log_spidfu_fpga_hyper310_rom_ext'
)

UNIT_TESTS=(
  '//sw/device/silicon_creator/lib/boot_svc:boot_svc_enter_rescue_unittest'
  '//sw/device/silicon_creator/lib/drivers:alert_unittest'
  '//sw/device/silicon_creator/lib/drivers:ast_unittest'
  '//sw/device/silicon_creator/lib/drivers:flash_ctrl_unittest'
  '//sw/device/silicon_creator/lib/drivers:ibex_unittest'
  '//sw/device/silicon_creator/lib/drivers:keymgr_unittest'
  '//sw/device/silicon_creator/lib/drivers:lifecycle_unittest'
  '//sw/device/silicon_creator/lib/drivers:otp_unittest'
  '//sw/device/silicon_creator/lib/drivers:rnd_unittest'
  '//sw/device/silicon_creator/lib/drivers:sensor_ctrl_unittest'
  '//sw/device/silicon_creator/lib/drivers:watchdog_unittest'
  '//sw/device/silicon_creator/lib/ownership:isfb_unittest'
  '//sw/device/silicon_creator/lib/ownership:owner_block_unittest'
  '//sw/device/silicon_creator/lib/ownership:ownership_activate_unittest'
  '//sw/device/silicon_creator/lib/ownership:ownership_unittest'
  '//sw/device/silicon_creator/lib/ownership:ownership_unlock_unittest'
  '//sw/device/silicon_creator/lib:boot_data_unittest'
  '//sw/device/silicon_creator/lib:dbg_print_unittest'
  '//sw/device/silicon_creator/lib:manifest_unittest'
  '//sw/device/silicon_creator/lib:shutdown_unittest'
  '//sw/device/silicon_creator/manuf/base:perso_tlv_data_unittest'
  '//sw/device/silicon_creator/rom:boot_policy_unittest'
  '//sw/device/silicon_creator/rom:bootstrap_unittest'
  '//sw/device/silicon_creator/rom_ext:rom_ext_boot_policy_unittest'
)


TEST_GROUPS=(
  'EXTRA_TESTS'
  'CW310_ROM_EXT_TESTS'
  'CW310_SIVAL_ROM_EXT_TESTS'
  'CW340_INSTRUMENTED_ROM_TESTS'
  'HYPER310_ROM_EXT_TESTS'
  'UNIT_TESTS'
)
