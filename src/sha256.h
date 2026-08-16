#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

void sha256_bytes(const void *data, size_t len, uint8_t digest[32]);
int sha256_file(const char *path, uint8_t digest[32]);
void sha256_hex(const uint8_t digest[32], char hex[65]);

#endif
