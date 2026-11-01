#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md5.h"
#include "checkV4.h"
#include "rjencode.h"

_Static_assert(sizeof(UINT4) == 4, "MD5 requires 32-bit words");

static void to_hex(const unsigned char* bytes, size_t count, char* result) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < count; ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 15];
    }
    result[count * 2] = 0;
}

static int check_md5(const char* input, const char* expected) {
    char actual[33];
    to_hex(ComputeHash((UCHAR*)input, (UINT4)strlen(input)), 16, actual);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "MD5 vector failed: %s != %s\n", actual, expected);
        return 1;
    }
    printf("MD5=%s\n", actual);
    return 0;
}

static int check_long_credentials(void) {
    unsigned char challenge[16];
    unsigned char input[64 + 16];
    unsigned char expected[16];
    char username[65], password[65], digest[33];
    size_t i;
    for (i = 0; i < 64; ++i) {
        username[i] = (char)('a' + i % 26);
        password[i] = (char)('A' + i % 26);
    }
    username[64] = password[64] = 0;
    for (i = 0; i < sizeof(challenge); ++i) challenge[i] = (unsigned char)i;
    memcpy(input, username, 64);
    memcpy(input + 64, challenge, 16);
    memcpy(expected, ComputeHash(input, sizeof(input)), 16);
    for (i = 0; i < 16; ++i) expected[i] ^= (unsigned char)password[i];
    if (memcmp(expected, computePwd(challenge, username, password), 16) != 0) {
        fputs("64-byte credential regression failed\n", stderr);
        return 1;
    }
    to_hex(expected, 16, digest);
    printf("PWD-LONG=%s\n", digest);
    return 0;
}

int main(int argc, char** argv) {
    unsigned char challenge[16] = {0};
    unsigned char encoded[256], original[256];
    char digest[33];
    int i;
    if (argc == 2 && strcmp(argv[1], "--v4-bounds") == 0) {
        uint32_t state = 0x12345678u;
        int sample, byte;
        /* Start with the maximum branch-0 expansion (193 bytes including NUL),
         * then exercise both sides of the old 160-byte boundary and all branches.
         * The previous 0x80..0x8f sample alone does not select branch 0. */
        for (sample = 0; sample < 4096; ++sample) {
            for (byte = 0; byte < 16; ++byte) {
                state = state * 1664525u + 1013904223u;
                challenge[byte] = sample == 0 ? 0x80 : (unsigned char)(state >> 24);
            }
            if (sample > 0 && sample <= 14) {
                memset(challenge, 0, sizeof(challenge));
                for (byte = 0; byte < sample; ++byte) challenge[byte] = 0x80;
                challenge[0] = challenge[3] = 0x80; /* always branch 0 */
            }
            printf("V4-BOUNDS-%d=%.128s\n", sample, computeV4(challenge, 16));
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--long-credentials") == 0) {
        return check_long_credentials();
    }
    if (check_md5("", "d41d8cd98f00b204e9800998ecf8427e") ||
        check_md5("abc", "900150983cd24fb0d6963f7d28e17f72") ||
        check_md5("12345678901234567890123456789012345678901234567890123456789012345678901234567890",
                  "57edf4a22be3c955ac49da2e2107b67a")) {
        return 1;
    }
    /* With s[3] == 0, s[0] == 0..4 selects every computeV4 branch. */
    for (i = 0; i < 5; ++i) {
        challenge[0] = (unsigned char)i;
        printf("V4-%d=%.128s\n", i, computeV4(challenge, sizeof(challenge)));
    }
    /* Preserve the original signed-char selection/formatting behavior. */
    for (i = 0; i < 16; ++i) challenge[i] = (unsigned char)(0x80 + i);
    printf("V4-HIGH=%.128s\n", computeV4(challenge, sizeof(challenge)));
    to_hex((unsigned char*)computePwd(challenge, "student", "password"), 16, digest);
    printf("PWD=%s\n", digest);
    for (i = 0; i < 256; ++i) original[i] = encoded[i] = (unsigned char)i;
    rj_encode(encoded, sizeof(encoded));
    rj_decode(encoded, sizeof(encoded));
    if (memcmp(original, encoded, sizeof(encoded)) != 0) {
        fputs("RJ encode/decode round trip failed\n", stderr);
        return 1;
    }
    puts("RJ-ENCODE=PASS");
    return 0;
}
