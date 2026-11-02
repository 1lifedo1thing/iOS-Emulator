/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define QEMU_IOS_PORTABLE_CRYPTO
#include "hw/arm/portable-crypto.h"

void reference_cbc(const uint8_t *, int, const uint8_t *, uint8_t *, size_t,
                   uint8_t *, int);
bool reference_product(const uint8_t *, const uint8_t *, const uint8_t *,
                       size_t, unsigned, bool, bool, bool, uint8_t *);

static uint32_t state = 0x4c547643;
static void fill(uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        bytes[i] = state;
    }
}

static void cbc(void)
{
    for (int bits = 128; bits <= 256; bits += 64) {
        uint8_t key[32], input[256], expected[256], actual[256];
        uint8_t initial[16], iv[16], reference_iv[16];
        fill(key, sizeof(key)); fill(input, sizeof(input)); fill(initial, 16);
        for (int encrypt = 0; encrypt <= 1; encrypt++) {
            AES_KEY schedule;
            assert((encrypt ? AES_set_encrypt_key(key, bits, &schedule) :
                              AES_set_decrypt_key(key, bits, &schedule)) == 0);
            for (int inplace = 0; inplace <= 1; inplace++) {
                for (size_t chunk = 16; chunk <= sizeof(input); chunk *= 2) {
                    memcpy(actual, input, sizeof(input));
                    memcpy(expected, input, sizeof(input));
                    memcpy(iv, initial, 16); memcpy(reference_iv, initial, 16);
                    for (size_t offset = 0; offset < sizeof(input); offset += chunk) {
                        portable_aes_cbc(inplace ? actual + offset : input + offset,
                                         actual + offset, chunk, &schedule, iv, encrypt);
                        reference_cbc(key, bits,
                                      inplace ? expected + offset : input + offset,
                                      expected + offset, chunk, reference_iv, encrypt);
                        assert(!memcmp(actual, expected, offset + chunk));
                        assert(!memcmp(iv, reference_iv, 16));
                    }
                    portable_aes_cbc(actual, actual, 0, &schedule, iv, encrypt);
                    assert(!memcmp(iv, reference_iv, 16));
                }
            }
        }
    }
}

static void products(void)
{
    for (size_t size = 64; size <= 256; size *= 2) {
        uint8_t modulus[256], left[256], right[256], actual[256], expected[256];
        for (unsigned sample = 0; sample < 32; sample++) {
            fill(modulus, size); fill(left, size); fill(right, size);
            modulus[0] |= 1; modulus[size - 1] |= 0x80;
            if (!sample) memset(left, 0, size); /* Zero must be padded. */
            for (int signs = 0; signs < 4; signs++) {
                for (int one = 0; one < 2; one++) {
                    unsigned bits = size * 8 + size / 64 * 16;
                    memset(actual, 0xa5, size); memset(expected, 0xa5, size);
                    assert(reference_product(modulus, left, one ? NULL : right,
                        size, bits, signs & 1, signs & 2, one, expected));
                    assert(portable_pke_product(modulus, left, one ? NULL : right,
                        size, bits, signs & 1, signs & 2, one, actual));
                    assert(!memcmp(actual, expected, size));
                }
            }
        }
        for (unsigned invalid = 0; invalid < 3; invalid++) {
            memset(modulus, 0, size); modulus[0] = invalid;
            assert(!portable_pke_product(modulus, left, right, size,
                                        size * 8, false, false, false, actual));
            assert(!reference_product(modulus, left, right, size,
                                      size * 8, false, false, false, expected));
        }
    }
}

int main(void)
{
    assert(gcry_check_version(NULL));
    gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
    cbc(); products();
    puts("PASS: QEMU AES 128/192/256 CBC in-place and chunked IV; 768 signed/one 512/1024/2048-bit Montgomery products match OpenSSL; invalid moduli rejected");
}
