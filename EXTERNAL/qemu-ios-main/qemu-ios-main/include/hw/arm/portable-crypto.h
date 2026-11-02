/* SPDX-License-Identifier: GPL-2.0-or-later
 * Portable host primitives for the existing Apple device models. macOS keeps
 * its original OpenSSL implementation. Guest registers and arithmetic do not
 * change; the portable engine reuses QEMU AES and the libgcrypt MPI backend.
 */
#ifndef HW_ARM_PORTABLE_CRYPTO_H
#define HW_ARM_PORTABLE_CRYPTO_H

#ifdef QEMU_IOS_PORTABLE_CRYPTO
#include "crypto/aes.h"
#include <gcrypt.h>

#define AES_ENCRYPT 1
#define AES_DECRYPT 0

/* Existing model DMA requests are complete AES blocks. IV mutation matches
 * OpenSSL CBC, including in-place decrypt and segmented DMA continuation. */
static inline void portable_aes_cbc(const uint8_t *in, uint8_t *out,
    size_t size, const AES_KEY *key, uint8_t iv[16], int encrypt)
{
    uint8_t block[16], cipher[16];
    assert(!(size % 16));
    for (size_t offset = 0; offset < size; offset += 16) {
        if (encrypt) {
            for (int i = 0; i < 16; i++) block[i] = in[offset + i] ^ iv[i];
            AES_encrypt(block, out + offset, key);
            memcpy(iv, out + offset, 16);
        } else {
            memcpy(cipher, in + offset, 16);
            AES_decrypt(cipher, block, key);
            for (int i = 0; i < 16; i++) out[offset + i] = block[i] ^ iv[i];
            memcpy(iv, cipher, 16);
        }
    }
}
#define AES_cbc_encrypt portable_aes_cbc

static inline gcry_mpi_t portable_mpi_le(const uint8_t *bytes, size_t size)
{
    uint8_t be[256];
    gcry_mpi_t mpi = NULL;
    if (size > sizeof(be)) return NULL;
    for (size_t i = 0; i < size; i++) be[i] = bytes[size - 1 - i];
    if (gcry_mpi_scan(&mpi, GCRYMPI_FMT_USG, be, size, NULL)) return NULL;
    return mpi;
}

/* The hardware performs A * B / 2^bits mod N, including signed operands.
 * Return big-endian padded output, exactly as the original BN implementation.
 * The existing device owns segment selection, validation and signature policy. */
static inline bool portable_pke_product(const uint8_t *modulus,
    const uint8_t *a, const uint8_t *b, size_t size, unsigned bits,
    bool negative_a, bool negative_b, bool one, uint8_t *out)
{
    gcry_mpi_t mod = portable_mpi_le(modulus, size);
    gcry_mpi_t left = portable_mpi_le(a, size);
    gcry_mpi_t right = one ? gcry_mpi_new(0) : portable_mpi_le(b, size);
    gcry_mpi_t value = gcry_mpi_new(0);
    /*
     * The radix inverse depends only on the modulus and the width, but a
     * modular exponentiation is hundreds of products against one modulus,
     * and libgcrypt's constant-time invm was ~7% of an n72 boot. Keep the
     * last one. Single PKE, operated by the vCPU under the BQL.
     */
    static uint8_t cached_modulus[256];
    static size_t cached_size;
    static unsigned cached_bits;
    static gcry_mpi_t inverse;
    bool ok = mod && left && right && value && size <= sizeof(cached_modulus);
    if (ok) {
        if (negative_a) gcry_mpi_neg(left, left);
        if (one) gcry_mpi_set_ui(right, 1);
        else if (negative_b) gcry_mpi_neg(right, right);
        ok = gcry_mpi_test_bit(mod, 0) && gcry_mpi_cmp_ui(mod, 1) > 0;
        if (ok && !(inverse && cached_size == size && cached_bits == bits &&
                    !memcmp(cached_modulus, modulus, size))) {
            gcry_mpi_t radix = gcry_mpi_new(0);
            gcry_mpi_release(inverse);
            inverse = gcry_mpi_new(0);
            gcry_mpi_set_ui(radix, 0);
            gcry_mpi_set_bit(radix, bits);
            ok = radix && inverse && gcry_mpi_invm(inverse, radix, mod);
            gcry_mpi_release(radix);
            if (ok) {
                memcpy(cached_modulus, modulus, size);
                cached_size = size;
                cached_bits = bits;
            } else {
                gcry_mpi_release(inverse);
                inverse = NULL;
            }
        }
        if (ok) {
            gcry_mpi_mulm(value, left, right, mod);
            gcry_mpi_mulm(value, value, inverse, mod);
            gcry_mpi_mod(value, value, mod);
            size_t written = 0;
            ok = !gcry_mpi_print(GCRYMPI_FMT_USG, out, size, &written, value);
            if (ok && written < size) {
                memmove(out + size - written, out, written);
                memset(out, 0, size - written);
            }
        }
    }
    gcry_mpi_release(mod); gcry_mpi_release(left); gcry_mpi_release(right);
    gcry_mpi_release(value);
    return ok;
}
#else
#include <openssl/aes.h>
#endif
#endif
