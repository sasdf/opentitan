// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/dif/dif_entropy_src.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/rand_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/rnd.h"
#include "sw/device/silicon_creator/lib/error.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

static dif_entropy_src_t entropy_src;

// Returns `rnd_threshold` if the target `test_id` supports a low configuration
// threshold, otherwise it returns 0.
static uint32_t random_low_threshold(dif_entropy_src_test_t test_id,
                                     uint32_t rnd_threshold) {
  uint32_t result;
  switch (test_id) {
    case kDifEntropySrcTestRepetitionCount:
    case kDifEntropySrcTestRepetitionCountSymbol:
    case kDifEntropySrcTestBucket:
      result = 0;
      break;
    case kDifEntropySrcTestAdaptiveProportion:
    case kDifEntropySrcTestMarkov:
    case kDifEntropySrcTestMailbox:
      result = rnd_threshold;
      break;
    default:
      CHECK(false, "Unexpected dif_entropy_src_test_t %d", test_id);
      return 0;
  }
  return result;
}

// Configures entropy_src with random but harmless health test thresholds. The
// entropy complex is enabled in continuous mode to generate the random values,
// and then re-enabled to run with the configured health thresholds.
static void configure_random_health_checks(void) {
  // Generate random values using entropy complex in continuous mode.
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  dif_entropy_src_health_test_config_t configs[kDifEntropySrcTestNumVariants];
  for (uint32_t test_id = kDifEntropySrcTestRepetitionCount;
       test_id < kDifEntropySrcTestNumVariants; ++test_id) {
    // Randomize threshold values without aiming to disrupt the default
    // entropy_src health test behavior.
    uint32_t meaningless_threshold =
        rand_testutils_gen32_range(/*min=*/0, /*max=*/16);
    configs[test_id] = (dif_entropy_src_health_test_config_t){
        .test_type = (dif_entropy_src_test_t)test_id,
        .high_threshold = 0xFFFFFF00 ^ meaningless_threshold,
        .low_threshold = random_low_threshold(test_id, meaningless_threshold),
    };
  }

  // Configure health values after disabling the entropy complex.
  CHECK_STATUS_OK(entropy_testutils_stop_all());
  for (size_t i = 0; i < ARRAYSIZE(configs); ++i) {
    CHECK_DIF_OK(
        dif_entropy_src_health_test_configure(&entropy_src, configs[i]));
  }
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
}

rom_error_t test_rnd(void) {
  const size_t kTestNumIter = 5;
  uint32_t prev = rnd_uint32();
  uint32_t error_cnt = 0;
  for (size_t i = 0; i < kTestNumIter; ++i) {
    uint32_t got = rnd_uint32();
    if (got == prev) {
      LOG_ERROR("Unexpected duplicate random number: 0x%x.", got);
      error_cnt += 1;
    }
    prev = got;
  }
  if (error_cnt > 0) {
    LOG_ERROR("rnd_test failed, expected: 0 errors, got: %d.", error_cnt);
    return kErrorUnknown;
  }
  return kErrorOk;
}

bool test_main(void) {
  CHECK_DIF_OK(dif_entropy_src_init(
      mmio_region_from_addr(TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR), &entropy_src));

  status_t result = OK_STATUS();
  EXECUTE_TEST(result, test_rnd);
  configure_random_health_checks();
  EXECUTE_TEST(result, test_rnd);
  return status_ok(result);
}
