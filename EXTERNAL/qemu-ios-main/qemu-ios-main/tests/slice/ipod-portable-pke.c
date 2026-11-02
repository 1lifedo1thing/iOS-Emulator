/* Portable backend runs the original production-device fixture without adding
 * a libgcrypt requirement to the existing macOS/OpenSSL test command.
 *
 * SLICE hw/arm/ipod_touch_pke.c range static const uint8_t sha1_digestinfo | static const MemoryRegionOps
 * PKG openssl libgcrypt gpg-error
 * CFLAGS -I$ROOT/include -DQEMU_IOS_PORTABLE_CRYPTO
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "hw/arm/portable-crypto.h"

static void __attribute__((constructor)) portable_test_init(void)
{
    assert(gcry_check_version(NULL));
    gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
}

#include "ipod-pke.c"
