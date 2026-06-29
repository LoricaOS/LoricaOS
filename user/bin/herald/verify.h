#ifndef HERALD_VERIFY_H
#define HERALD_VERIFY_H
#include <stddef.h>

/* SHA-256 of buf/len into out[32]. */
void herald_sha256(const void *buf, size_t len, unsigned char out[32]);

/* Verify a detached ECDSA P-256 / SHA-256 signature (ASN.1 DER, as produced by
 * `openssl dgst -sha256 -sign`) over msg/msg_len, against an uncompressed
 * P-256 public key (65 bytes: 0x04 || X || Y).
 * Returns 1 if valid, 0 if invalid or on any error. */
int herald_verify_p256_sha256(const unsigned char *pubkey, size_t pubkey_len,
                              const void *msg, size_t msg_len,
                              const unsigned char *sig, size_t sig_len);
#endif
