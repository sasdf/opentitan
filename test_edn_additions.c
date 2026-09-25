static void verify_edn_sw_cmd_req_busy_write(void) {
  LOG_INFO(
      "Verifying EDN SW_CMD_REQ busy write is dropped without fatal alert...");
  reset_entropy_complex_enable_csrng();

  // Enable EDN in SWPortMode
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9996u);

  // Verify CMD_REG_RDY
  uint32_t sw_sts = abs_mmio_read32(kEdn1Base + EDN_SW_CMD_STS_REG_OFFSET);
  CHECK((sw_sts & (1u << EDN_SW_CMD_STS_CMD_REG_RDY_BIT)) != 0u,
        "Expected CMD_REG_RDY");

  // Write INSTANTIATE command
  abs_mmio_write32(kEdn1Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);

  // Immediately write another command while the first is presumably executing /
  // busying the FIFO Since we haven't waited, the state machine or FIFO might
  // be busy (in RTL CMD_REG_RDY goes to 0 instantly)
  abs_mmio_write32(kEdn1Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);

  // Allow time
  busy_spin_micros(100);

  // Verify no fatal EDN_MAIN_SM_ERR is raised
  uint32_t err_code = abs_mmio_read32(kEdn1Base + EDN_ERR_CODE_REG_OFFSET);
  CHECK((err_code & (1u << EDN_ERR_CODE_EDN_MAIN_SM_ERR_BIT)) == 0u,
        "Expected EDN_MAIN_SM_ERR to NOT be set when writing while busy");

  // Restore
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9999u);
}
