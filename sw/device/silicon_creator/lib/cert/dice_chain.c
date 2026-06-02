// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/silicon_creator/lib/cert/dice_chain.h"

#include <stdbool.h>
#include <stddef.h>

#include "sw/device/lib/base/hardened.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/crypto/drivers/entropy.h"
#include "sw/device/silicon_creator/lib/base/boot_measurements.h"
#include "sw/device/silicon_creator/lib/base/sec_mmio.h"
#include "sw/device/silicon_creator/lib/base/static_dice_cdi_0.h"
#include "sw/device/silicon_creator/lib/base/util.h"
#include "sw/device/silicon_creator/lib/cert/dice.h"
#include "sw/device/silicon_creator/lib/dbg_print.h"
#include "sw/device/silicon_creator/lib/drivers/flash_ctrl.h"
#include "sw/device/silicon_creator/lib/drivers/kmac.h"
#include "sw/device/silicon_creator/lib/error.h"
#include "sw/device/silicon_creator/lib/manifest.h"
#include "sw/device/silicon_creator/lib/otbn_boot_services.h"
#include "sw/device/silicon_creator/lib/ownership/datatypes.h"
#include "sw/device/silicon_creator/manuf/base/perso_tlv_data.h"

#include "flash_ctrl_regs.h"  // Generated.

enum {
  kFlashPageSize = FLASH_CTRL_PARAM_BYTES_PER_PAGE,
  kDiceSlotSize = 1000,
  kDiceCertHeaderSize = 16,
};

typedef struct __attribute__((packed)) dice_cert_header {
  uint16_t object_header;  // Big Endian
  uint16_t cert_header;    // Big Endian
  char name[12];           // "CDI_0" or "CDI_1", padded with 0
} dice_cert_header_t;

static_assert(sizeof(dice_cert_header_t) % 8 == 0,
              "dice_cert_header_t must be a multiple of 8 bytes");

typedef struct dice_cert_layout {
  const flash_ctrl_info_page_t *info_page;
  uint32_t offset;
  dice_cert_header_t header;  // Template
} dice_cert_layout_t;

static const dice_cert_layout_t kLayoutCdi0 = {
    .info_page = &kFlashCtrlInfoPageDiceCerts,
    .offset = 0,
    .header =
        {
            .object_header = TLV_OBJ_HEADER(kPersoObjectTypeX509Cert, 1000),
            .cert_header =
                TLV_CERT_HEADER(12, 0),  // NameSize=12, Size filled at runtime
            .name = "CDI_0",  // C auto-pads the rest of 12 bytes with 0
        },
};

static const dice_cert_layout_t kLayoutCdi1 = {
    .info_page = &kFlashCtrlInfoPageDiceCerts,
    .offset = kDiceSlotSize,
    .header =
        {
            .object_header = TLV_OBJ_HEADER(kPersoObjectTypeX509Cert, 1000),
            .cert_header =
                TLV_CERT_HEADER(12, 0),  // NameSize=12, Size filled at runtime
            .name = "CDI_1",
        },
};

static uint32_t scratch_buffer[kFlashPageSize / sizeof(uint32_t)];

cert_key_id_pair_t dice_chain_cdi_0_key_ids = (cert_key_id_pair_t){
    .endorsement = &static_dice_cdi_0.uds_pubkey_id,
    .cert = &static_dice_cdi_0.cdi_0_pubkey_id,
};

static rom_error_t dice_chain_page_digest(hmac_digest_t *digest_out) {
  size_t bytes_to_hash = kFlashPageSize - sizeof(hmac_digest_t);  // 2016 bytes
  size_t word_count = bytes_to_hash / sizeof(uint32_t);           // 504 words

  RETURN_IF_ERROR(flash_ctrl_info_read_zeros_on_read_error(
      &kFlashCtrlInfoPageDiceCerts, 0, word_count, scratch_buffer));

  hmac_sha256_init();
  hmac_sha256_update(scratch_buffer, bytes_to_hash);
  hmac_sha256_process();
  hmac_sha256_final(digest_out);
  return kErrorOk;
}

static rom_error_t dice_chain_read_meta(dice_page_meta_t *meta) {
  return flash_ctrl_info_read_zeros_on_read_error(
      &kFlashCtrlInfoPageDiceCerts, offsetof(dice_page_t, meta),
      sizeof(dice_page_meta_t) / sizeof(uint32_t), meta);
}

static rom_error_t dice_chain_load_cdi0_from_flash(void) {
  dice_cert_header_t header;
  RETURN_IF_ERROR(flash_ctrl_info_read(
      &kFlashCtrlInfoPageDiceCerts, kLayoutCdi0.offset,
      sizeof(dice_cert_header_t) / sizeof(uint32_t), &header));

  uint16_t wrapped_cert_size;
  PERSO_TLV_GET_FIELD(Crth, Size, header.cert_header, &wrapped_cert_size);

  if (wrapped_cert_size <= 14) {
    return kErrorDicePageCorrupted;
  }
  uint32_t cert_size = wrapped_cert_size - 14;
  if (cert_size > sizeof(static_dice_cdi_0.cert_data)) {
    return kErrorDicePageCorrupted;
  }

  // Simply read the maximum certificate size.
  RETURN_IF_ERROR(flash_ctrl_info_read(
      &kFlashCtrlInfoPageDiceCerts,
      kLayoutCdi0.offset + sizeof(dice_cert_header_t),
      util_size_to_words(cert_size), static_dice_cdi_0.cert_data));

  static_dice_cdi_0.cert_size = cert_size;
  return kErrorOk;
}

static rom_error_t dice_chain_write_cert_slot(const dice_cert_layout_t *layout,
                                              uint8_t *cert_data,
                                              size_t cert_size) {
  dice_cert_header_t header = layout->header;
  size_t wrapped_size = sizeof(perso_tlv_cert_header_t) + 12 + cert_size;
  PERSO_TLV_SET_FIELD(Crth, Size, header.cert_header, wrapped_size);

  RETURN_IF_ERROR(flash_ctrl_info_write(
      layout->info_page, layout->offset,
      sizeof(dice_cert_header_t) / sizeof(uint32_t), &header));

  uint32_t word_count = util_size_to_words(cert_size);

  RETURN_IF_ERROR(flash_ctrl_info_write(
      layout->info_page, layout->offset + sizeof(dice_cert_header_t),
      word_count, cert_data));

  return kErrorOk;
}

static rom_error_t dice_chain_write_key_ids(
    const hmac_digest_t *cdi1_pubkey_id) {
  uint64_t key_ids[2];
  key_ids[0] = *(uint64_t *)static_dice_cdi_0.cdi_0_pubkey_id.digest;
  key_ids[1] = *(uint64_t *)cdi1_pubkey_id->digest;

  RETURN_IF_ERROR(flash_ctrl_info_write(
      &kFlashCtrlInfoPageDiceCerts, offsetof(dice_page_t, meta.cdi_0_key_id),
      util_size_to_words(sizeof(key_ids)), key_ids));
  return kErrorOk;
}

static rom_error_t dice_chain_seal(void) {
  hmac_digest_t digest;
  RETURN_IF_ERROR(dice_chain_page_digest(&digest));

  RETURN_IF_ERROR(flash_ctrl_info_write(
      &kFlashCtrlInfoPageDiceCerts, offsetof(dice_page_t, meta.digest),
      sizeof(digest) / sizeof(uint32_t), &digest));

  return kErrorOk;
}

static rom_error_t dice_chain_write_page(size_t cdi1_cert_size,
                                         const hmac_digest_t *cdi1_pubkey_id) {
  RETURN_IF_ERROR(dice_chain_write_cert_slot(
      &kLayoutCdi0, static_dice_cdi_0.cert_data, static_dice_cdi_0.cert_size));
  RETURN_IF_ERROR(dice_chain_write_cert_slot(
      &kLayoutCdi1, (uint8_t *)scratch_buffer, cdi1_cert_size));
  RETURN_IF_ERROR(dice_chain_write_key_ids(cdi1_pubkey_id));
  RETURN_IF_ERROR(dice_chain_seal());
  return kErrorOk;
}

rom_error_t dice_chain_attestation_silicon(void) {
  // Initialize the entropy complex and KMAC for key manager operations.
  // Note: `OTCRYPTO_OK.value` is equal to `kErrorOk` but we cannot add a static
  // assertion here since its definition is not an integer constant expression.
  HARDENED_RETURN_IF_ERROR(
      (rom_error_t)entropy_complex_init(kHardenedBoolFalse).value);
  HARDENED_RETURN_IF_ERROR(kmac_keymgr_configure());

  // Set keymgr reseed interval. Start with the maximum value to avoid
  // entropy complex contention during the boot process.
  const uint16_t kScKeymgrEntropyReseedInterval = UINT16_MAX;
  sc_keymgr_entropy_reseed_interval_set(kScKeymgrEntropyReseedInterval);
  SEC_MMIO_WRITE_INCREMENT(kScKeymgrSecMmioEntropyReseedIntervalSet);

  // ROM sets the SW binding values for the first key stage (CreatorRootKey) but
  // does not initialize the key manager. Advance key manager state twice to
  // transition to the CreatorRootKey state.
  RETURN_IF_ERROR(sc_keymgr_state_check(kScKeymgrStateReset));
  sc_keymgr_advance_state();
  RETURN_IF_ERROR(sc_keymgr_state_check(kScKeymgrStateInit));

  // Generate UDS keys.
  sc_keymgr_advance_state();
  HARDENED_RETURN_IF_ERROR(sc_keymgr_state_check(kScKeymgrStateCreatorRootKey));
  HARDENED_RETURN_IF_ERROR(otbn_boot_cert_ecc_p256_keygen(
      kDiceKeyUds, &static_dice_cdi_0.uds_pubkey_id,
      &static_dice_cdi_0.uds_pubkey));

  // Save UDS key for signing next stage cert.
  RETURN_IF_ERROR(otbn_boot_attestation_key_save(
      kDiceKeyUds.keygen_seed_idx, kDiceKeyUds.type,
      *kDiceKeyUds.keymgr_diversifier));

  return kErrorOk;
}

rom_error_t dice_chain_attestation_creator(
    keymgr_binding_value_t *rom_ext_measurement,
    const manifest_t *rom_ext_manifest) {
  // Generate CDI_0 attestation keys and (potentially) update certificate.
  keymgr_binding_value_t seal_binding_value = {
      .data = {rom_ext_manifest->identifier, 0}};
  SEC_MMIO_WRITE_INCREMENT(kScKeymgrSecMmioSwBindingSet +
                           kScKeymgrSecMmioOwnerIntMaxVerSet);
  HARDENED_RETURN_IF_ERROR(sc_keymgr_owner_int_advance(
      /*sealing_binding=*/&seal_binding_value,
      /*attest_binding=*/rom_ext_measurement,
      rom_ext_manifest->max_key_version));
  HARDENED_RETURN_IF_ERROR(otbn_boot_cert_ecc_p256_keygen(
      kDiceKeyCdi0, &static_dice_cdi_0.cdi_0_pubkey_id,
      &static_dice_cdi_0.cdi_0_pubkey));

  // Check if the current CDI_0 cert is valid.
  dice_page_meta_t meta;
  RETURN_IF_ERROR(dice_chain_read_meta(&meta));
  uint64_t expected_cdi0_id =
      *(uint64_t *)static_dice_cdi_0.cdi_0_pubkey_id.digest;

  bool cache_valid = meta.cdi_0_key_id == expected_cdi0_id;

  if (!cache_valid) {
    // Update the cert page buffer.
    static_dice_cdi_0.cert_size = sizeof(static_dice_cdi_0.cert_data);
    HARDENED_RETURN_IF_ERROR(dice_cdi_0_cert_build(
        (hmac_digest_t *)rom_ext_measurement->data,
        rom_ext_manifest->security_version, &dice_chain_cdi_0_key_ids,
        &static_dice_cdi_0.uds_pubkey, &static_dice_cdi_0.cdi_0_pubkey,
        static_dice_cdi_0.cert_data, &static_dice_cdi_0.cert_size));
  } else {
    // Replace UDS with CDI_0 key for endorsing next stage cert.
    HARDENED_RETURN_IF_ERROR(otbn_boot_attestation_key_save(
        kDiceKeyCdi0.keygen_seed_idx, kDiceKeyCdi0.type,
        *kDiceKeyCdi0.keymgr_diversifier));
  }

  sc_keymgr_sw_binding_unlock_wait();

  return kErrorOk;
}

static rom_error_t dice_chain_check_digest(void) {
  dice_page_meta_t meta;
  RETURN_IF_ERROR(dice_chain_read_meta(&meta));

  hmac_digest_t calculated_digest;
  RETURN_IF_ERROR(dice_chain_page_digest(&calculated_digest));

  if (memcmp(&calculated_digest, &meta.digest, sizeof(hmac_digest_t)) != 0) {
    return kErrorDicePageCorrupted;
  }
  return kErrorOk;
}

rom_error_t dice_chain_rom_ext_check(void) {
  if (static_dice_cdi_0.cert_size != 0) {
    return kErrorOk;
  }

  rom_error_t status = dice_chain_check_digest();
  if (status != kErrorOk) {
    dbg_printf("warning: corrupted page; erasing\r\n");
    RETURN_IF_ERROR(flash_ctrl_info_erase(&kFlashCtrlInfoPageDiceCerts,
                                          kFlashCtrlEraseTypePage));
    return status;
  }
  return kErrorOk;
}

rom_error_t dice_chain_attestation_owner(
    const manifest_t *owner_manifest, keymgr_binding_value_t *bl0_measurement,
    hmac_digest_t *owner_measurement, hmac_digest_t *owner_history_hash,
    keymgr_binding_value_t *sealing_binding, owner_app_domain_t key_domain) {
  // Local variables for CDI_1 key generation and cert building
  hmac_digest_t subject_pubkey_id;
  ecdsa_p256_public_key_t subject_pubkey;
  cert_key_id_pair_t key_ids = {
      .endorsement = &static_dice_cdi_0.cdi_0_pubkey_id,
      .cert = &subject_pubkey_id,
  };

  // Generate CDI_1 attestation keys.
  SEC_MMIO_WRITE_INCREMENT(kScKeymgrSecMmioSwBindingSet +
                           kScKeymgrSecMmioOwnerIntMaxVerSet);
  hmac_digest_t attest_measurement;
  hmac_sha256_configure(false);
  hmac_sha256_start();
  hmac_sha256_update(bl0_measurement, sizeof(*bl0_measurement));
  hmac_sha256_update(owner_measurement, sizeof(*owner_measurement));
  hmac_sha256_process();
  hmac_sha256_final(&attest_measurement);

  HARDENED_RETURN_IF_ERROR(sc_keymgr_owner_advance(
      /*sealing_binding=*/sealing_binding,
      /*attest_binding=*/(keymgr_binding_value_t *)&attest_measurement,
      owner_manifest->max_key_version));
  HARDENED_RETURN_IF_ERROR(otbn_boot_cert_ecc_p256_keygen(
      kDiceKeyCdi1, &subject_pubkey_id, &subject_pubkey));

  // Read metadata for validation.
  dice_page_meta_t meta;
  RETURN_IF_ERROR(dice_chain_read_meta(&meta));

  uint64_t expected_cdi1_id = *(uint64_t *)subject_pubkey_id.digest;

  bool cache_valid = (static_dice_cdi_0.cert_size == 0 &&
                      meta.cdi_1_key_id == expected_cdi1_id);

  if (!cache_valid) {
    dbg_puts("warning: DICE cache not valid; updating\r\n");

    // Check if cdi0 in static region is empty, if empty fill from flash.
    if (static_dice_cdi_0.cert_size == 0) {
      RETURN_IF_ERROR(dice_chain_load_cdi0_from_flash());
    }

    // Erase the page.
    RETURN_IF_ERROR(flash_ctrl_info_erase(&kFlashCtrlInfoPageDiceCerts,
                                          kFlashCtrlEraseTypePage));

    // Sign cdi1.
    size_t updated_cert_size = sizeof(scratch_buffer);
    HARDENED_RETURN_IF_ERROR(dice_cdi_1_cert_build(
        (hmac_digest_t *)bl0_measurement, owner_measurement, owner_history_hash,
        owner_manifest->security_version, key_domain, &key_ids,
        &static_dice_cdi_0.cdi_0_pubkey, &subject_pubkey,
        (uint8_t *)scratch_buffer, &updated_cert_size));

    // Write both certs and metadata.
    RETURN_IF_ERROR(
        dice_chain_write_page(updated_cert_size, &subject_pubkey_id));
  } else {
    // Replace CDI_0 with CDI_1 key for endorsing next stage cert.
    HARDENED_RETURN_IF_ERROR(otbn_boot_attestation_key_save(
        kDiceKeyCdi1.keygen_seed_idx, kDiceKeyCdi1.type,
        *kDiceKeyCdi1.keymgr_diversifier));
  }

  sc_keymgr_sw_binding_unlock_wait();
  return kErrorOk;
}

rom_error_t dice_chain_init(void) {
  // Configure DICE certificate flash info page and buffer it into RAM.
  flash_ctrl_cert_info_page_creator_cfg(&kFlashCtrlInfoPageDiceCerts);
  flash_ctrl_info_cfg_set(&kFlashCtrlInfoPageFactoryCerts,
                          kCertificateInfoPageCfg);
  flash_ctrl_cert_info_page_owner_restrict(&kFlashCtrlInfoPageFactoryCerts);
  return kErrorOk;
}
