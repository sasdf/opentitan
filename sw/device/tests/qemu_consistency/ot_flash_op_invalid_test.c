#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_flash_ctrl.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "flash_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

bool test_main(void) {
  mmio_region_t flash_ctrl =
      mmio_region_from_addr(TOP_EARLGREY_FLASH_CTRL_CORE_BASE_ADDR);

  uint32_t regwen =
      mmio_region_read32(flash_ctrl, FLASH_CTRL_CTRL_REGWEN_REG_OFFSET);
  CHECK(regwen == 1);

  // Write OP = 3 (invalid), and set START = 1
  uint32_t ctrl_val = bitfield_field32_write(0, FLASH_CTRL_CONTROL_OP_FIELD, 3);
  ctrl_val = bitfield_bit32_write(ctrl_val, FLASH_CTRL_CONTROL_START_BIT, true);
  mmio_region_write32(flash_ctrl, FLASH_CTRL_CONTROL_REG_OFFSET, ctrl_val);

  for (volatile int i = 0; i < 100; i++) {
  }

  // Check that REGWEN remains 0 (because the operation never finishes and never
  // clears START)
  regwen = mmio_region_read32(flash_ctrl, FLASH_CTRL_CTRL_REGWEN_REG_OFFSET);
  uint32_t ctrl_read =
      mmio_region_read32(flash_ctrl, FLASH_CTRL_CONTROL_REG_OFFSET);
  bool is_started =
      bitfield_bit32_read(ctrl_read, FLASH_CTRL_CONTROL_START_BIT);

  CHECK(regwen == 0, "REGWEN did not remain 0! It is %x", regwen);
  CHECK(is_started == true, "START bit cleared itself!");

  return true;
}
