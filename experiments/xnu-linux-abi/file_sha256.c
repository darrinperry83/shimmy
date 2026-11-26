/* Minimal recoveryOS file hashing using Apple's libSystem CommonCrypto API. */
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: sha256 FILE\n");
        return 2;
    }
    FILE *stream = fopen(argv[1], "rb");
    if (!stream) {
        perror(argv[1]);
        return 1;
    }
    CC_SHA256_CTX context;
    unsigned char buffer[65536], digest[CC_SHA256_DIGEST_LENGTH];
    if (CC_SHA256_Init(&context) != 1) return 1;
    size_t count;
    while ((count = fread(buffer, 1, sizeof buffer, stream)) != 0) {
        if (CC_SHA256_Update(&context, buffer, (CC_LONG)count) != 1) return 1;
    }
    int failed = ferror(stream);
    if (fclose(stream) != 0) failed = 1;
    if (failed || CC_SHA256_Final(digest, &context) != 1) return 1;
    for (size_t i = 0; i < sizeof digest; i++) printf("%02x", digest[i]);
    putchar('\n');
    return 0;
}
