import re

with open("sw/device/tests/qemu/consistency/edn_consistency_test.c", "r") as f:
    text = f.read()

new_text = re.sub(
    r'(busy_spin_micros\(100\);\s*// Verify no fatal EDN_MAIN_SM_ERR is raised\s*uint32_t err_code = abs_mmio_read32\(kEdn1Base \+ EDN_ERR_CODE_REG_OFFSET\);\s*CHECK\(\(err_code & \(1u << EDN_ERR_CODE_EDN_MAIN_SM_ERR_BIT\)\) == 0u, "Expected EDN_MAIN_SM_ERR to NOT be set when writing while busy"\);)',
    r'\1\n  IBEX_SPIN_FOR((abs_mmio_read32(kEdn1Base + EDN_INTR_STATE_REG_OFFSET) & (1u << EDN_INTR_STATE_EDN_CMD_REQ_DONE_BIT)) != 0u, 10000);\n  abs_mmio_write32(kEdn1Base + EDN_INTR_STATE_REG_OFFSET, 0x3u);',
    text
)

with open("sw/device/tests/qemu/consistency/edn_consistency_test.c", "w") as f:
    f.write(new_text)

