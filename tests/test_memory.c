/* Compare with independent host libc, including all overlap directions and
 * unaligned subranges. Custom runtime symbols are renamed at test compile. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
void *dm32_memcpy(void *, const void *, size_t);
void *dm32_memmove(void *, const void *, size_t);
void *dm32_memset(void *, int, size_t);
int main(void)
{
    unsigned char actual[96], expected[96], source[96];
    unsigned long checks = 0;
    for (unsigned i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)(i * 137 + 51);
    for (size_t from = 0; from < 48; ++from)
        for (size_t to = 0; to < 48; ++to)
            for (size_t n = 0; n <= 48; ++n) {
                memcpy(actual, source, sizeof(actual));
                memcpy(expected, source, sizeof(expected));
                assert(dm32_memmove(actual + to, actual + from, n) == actual + to);
                memmove(expected + to, expected + from, n);
                assert(!memcmp(actual, expected, sizeof(actual)));
                assert(dm32_memset(actual + to, 0x1ab, n) == actual + to);
                memset(expected + to, 0x1ab, n);
                assert(!memcmp(actual, expected, sizeof(actual)));
                assert(dm32_memcpy(actual + to, source + from, n) == actual + to);
                memcpy(expected + to, source + from, n);
                assert(!memcmp(actual, expected, sizeof(actual)));
                checks += 3;
            }
    printf("memory runtime vs host libc: %lu checks passed\n", checks);
    return 0;
}
