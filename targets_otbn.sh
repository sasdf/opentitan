# ./bazelisk.sh query 'tests(//sw/otbn/...) except attr("tags", "skip_in_ci|manual|broken", //sw/otbn/...)'
OTBN_TESTS=(
//sw/otbn/crypto/tests:boot_testcase_boot_key_endorse_valid
//sw/otbn/crypto/tests:boot_testcase_boot_key_save_valid
//sw/otbn/crypto/tests:boot_testcase_boot_keygen_valid
//sw/otbn/crypto/tests:boot_testcase_boot_mode_invalid
//sw/otbn/crypto/tests:boot_testcase_boot_sigverify_valid
//sw/otbn/crypto/tests:p256_arithmetic_to_boolean_mod_test
//sw/otbn/crypto/tests:p256_arithmetic_to_boolean_test
//sw/otbn/crypto/tests:p256_base_mult_consttime
//sw/otbn/crypto/tests:p256_base_mult_test
//sw/otbn/crypto/tests:p256_ecdh_shared_key_test
//sw/otbn/crypto/tests:p256_ecdsa_sign_test
//sw/otbn/crypto/tests:p256_ecdsa_verify_test
//sw/otbn/crypto/tests:p256_isoncurve_consttime
//sw/otbn/crypto/tests:p256_isoncurve_test
//sw/otbn/crypto/tests:p256_key_from_seed_consttime
//sw/otbn/crypto/tests:p256_key_from_seed_test
//sw/otbn/crypto/tests:p256_mul_modp_test
//sw/otbn/crypto/tests:p256_proj_add_consttime
//sw/otbn/crypto/tests:p256_proj_add_test
//sw/otbn/crypto/tests:p256_scalar_mult_test
//sw/otbn/crypto/tests:p256_shared_key_consttime
//sw/otbn/crypto/tests:p256_testcase_p256_check_public_key_not_on_curve
//sw/otbn/crypto/tests:p256_testcase_p256_check_public_key_valid
//sw/otbn/crypto/tests:p256_testcase_p256_check_public_key_x_too_large
//sw/otbn/crypto/tests:p256_testcase_p256_check_public_key_y_too_large
//sw/otbn/crypto/tests:p256_testcase_p256_isoncurve_valid
//sw/otbn/crypto/tests:p256_testcase_p256_keygen_valid


# //sw/otbn/crypto/tests:div_consttime
# //sw/otbn/crypto/tests:div_large_test
# //sw/otbn/crypto/tests:div_medium_test
# //sw/otbn/crypto/tests:div_small_test
# //sw/otbn/crypto/tests:ed25519_ext_add_consttime
# //sw/otbn/crypto/tests:ed25519_ext_add_test
# //sw/otbn/crypto/tests:ed25519_scalar_test
# //sw/otbn/crypto/tests:field25519_fe_inv_consttime
# //sw/otbn/crypto/tests:field25519_fe_mul_consttime
# //sw/otbn/crypto/tests:field25519_fe_square_consttime
# //sw/otbn/crypto/tests:field25519_test
# //sw/otbn/crypto/tests:gcd_consttime
# //sw/otbn/crypto/tests:gcd_large_test
# //sw/otbn/crypto/tests:gcd_small_test
# //sw/otbn/crypto/tests:lcm_consttime
# //sw/otbn/crypto/tests:lcm_test
# //sw/otbn/crypto/tests:miller_rabin_consttime_1024
# //sw/otbn/crypto/tests:miller_rabin_consttime_1536
# //sw/otbn/crypto/tests:miller_rabin_consttime_2048
# //sw/otbn/crypto/tests:modinv_f4_consttime_test
# //sw/otbn/crypto/tests:modinv_f4_test
# //sw/otbn/crypto/tests:mul_consttime
# //sw/otbn/crypto/tests:mul_test
# //sw/otbn/crypto/tests:p384_arithmetic_to_boolean_mod_test
# //sw/otbn/crypto/tests:p384_arithmetic_to_boolean_test
# //sw/otbn/crypto/tests:p384_base_mult_consttime
# //sw/otbn/crypto/tests:p384_base_mult_test
# //sw/otbn/crypto/tests:p384_boolean_to_arithmetic_test
# //sw/otbn/crypto/tests:p384_curve_point_valid_test
# //sw/otbn/crypto/tests:p384_ecdh_shared_key_test
# //sw/otbn/crypto/tests:p384_ecdsa_sign_test
# //sw/otbn/crypto/tests:p384_ecdsa_verify_test
# //sw/otbn/crypto/tests:p384_isoncurve_test
# //sw/otbn/crypto/tests:p384_keygen_from_seed_test
# //sw/otbn/crypto/tests:p384_keygen_test
# //sw/otbn/crypto/tests:p384_mulmod448x128_test
# //sw/otbn/crypto/tests:p384_mulmod_n_consttime
# //sw/otbn/crypto/tests:p384_mulmod_p_consttime
# //sw/otbn/crypto/tests:p384_proj_add_test
# //sw/otbn/crypto/tests:p384_scalar_mult_consttime
# //sw/otbn/crypto/tests:p384_scalar_mult_test
# //sw/otbn/crypto/tests:primality_negative_test
# //sw/otbn/crypto/tests:primality_test
# //sw/otbn/crypto/tests:primality_test_witness_negative_test
# //sw/otbn/crypto/tests:primality_test_witness_test
# //sw/otbn/crypto/tests:proj_add_p384_consttime
# //sw/otbn/crypto/tests:relprime_f4_consttime_test
# //sw/otbn/crypto/tests:relprime_f4_test
# //sw/otbn/crypto/tests:rsa_1024_dec_test
# //sw/otbn/crypto/tests:rsa_1024_enc_test
# //sw/otbn/crypto/tests:rsa_2048_dec_test
# //sw/otbn/crypto/tests:rsa_2048_enc_test
# //sw/otbn/crypto/tests:rsa_3072_dec_test
# //sw/otbn/crypto/tests:rsa_3072_enc_test
# //sw/otbn/crypto/tests:rsa_4096_enc_test
# //sw/otbn/crypto/tests:rsa_keygen_checkp_good_test
# //sw/otbn/crypto/tests:rsa_keygen_checkp_not_prime_test
# //sw/otbn/crypto/tests:rsa_keygen_checkp_not_relprime_test
# //sw/otbn/crypto/tests:rsa_keygen_checkq_good_test
# //sw/otbn/crypto/tests:rsa_keygen_checkq_not_prime_test
# //sw/otbn/crypto/tests:rsa_keygen_checkq_not_relprime_test
# //sw/otbn/crypto/tests:rsa_keygen_checkq_too_close_test
# //sw/otbn/crypto/tests:rsa_verify_3072_consts_test
# //sw/otbn/crypto/tests:rsa_verify_3072_test
# //sw/otbn/crypto/tests:rsa_verify_exp3_test
# //sw/otbn/crypto/tests:rsa_verify_test
# //sw/otbn/crypto/tests:sha256_consttime
# //sw/otbn/crypto/tests:sha256_test
# //sw/otbn/crypto/tests:sha384_test
# //sw/otbn/crypto/tests:sha512_compact_test
# //sw/otbn/crypto/tests:sha512_test
# //sw/otbn/crypto/tests:x25519_consttime
# //sw/otbn/crypto/tests:x25519_test1
# //sw/otbn/crypto/tests:x25519_test2
# //sw/otbn/crypto/tests/generated:mul_test0
# //sw/otbn/crypto/tests/generated:mul_test1
# //sw/otbn/crypto/tests/generated:mul_test2
# //sw/otbn/crypto/tests/generated:mul_test3
# //sw/otbn/crypto/tests/generated:mul_test4
# //sw/otbn/crypto/tests/generated:mul_test5
# //sw/otbn/crypto/tests/generated:mul_test6
# //sw/otbn/crypto/tests/generated:mul_test7
# //sw/otbn/crypto/tests/generated:mul_test8
# //sw/otbn/crypto/tests/generated:mul_test9
)

TEST_GROUPS=(
    "OTBN_TESTS"
)
