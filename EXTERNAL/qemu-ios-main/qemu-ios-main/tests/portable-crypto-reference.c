/* SPDX-License-Identifier: GPL-2.0-or-later
 * Independent original OpenSSL operations for portable-model comparisons.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <openssl/aes.h>
#include <openssl/bn.h>

void reference_cbc(const uint8_t *key, int bits, const uint8_t *in,
                   uint8_t *out, size_t size, uint8_t *iv, int encrypt)
{
    AES_KEY schedule;
    assert((encrypt ? AES_set_encrypt_key(key, bits, &schedule) :
                      AES_set_decrypt_key(key, bits, &schedule)) == 0);
    AES_cbc_encrypt(in, out, size, &schedule, iv, encrypt);
}

bool reference_product(const uint8_t *modulus, const uint8_t *a,
                       const uint8_t *b, size_t size, unsigned bits,
                       bool negative_a, bool negative_b, bool one, uint8_t *out)
{
    BIGNUM *mod = BN_lebin2bn(modulus, size, NULL);
    BIGNUM *left = BN_lebin2bn(a, size, NULL);
    BIGNUM *right = one ? BN_new() : BN_lebin2bn(b, size, NULL);
    BIGNUM *radix = BN_new(), *inverse = BN_new(), *value = BN_new();
    BN_CTX *ctx = BN_CTX_new();
    bool ok = mod && left && right && radix && inverse && value && ctx;
    if (ok) {
        if (negative_a) BN_set_negative(left, 1);
        if (one) ok = BN_one(right);
        else if (negative_b) BN_set_negative(right, 1);
        ok = ok && BN_is_odd(mod) && !BN_is_one(mod) && BN_set_bit(radix, bits) &&
             BN_mod_inverse(inverse, radix, mod, ctx) &&
             BN_mod_mul(value, left, right, mod, ctx) &&
             BN_mod_mul(value, value, inverse, mod, ctx) &&
             BN_bn2binpad(value, out, size) == size;
    }
    BN_free(mod); BN_free(left); BN_free(right); BN_free(radix);
    BN_free(inverse); BN_free(value); BN_CTX_free(ctx);
    return ok;
}
