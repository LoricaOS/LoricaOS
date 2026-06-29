/*
 * verify.c — detached signature verification for herald packages.
 *
 * Packages are signed with ECDSA P-256 over SHA-256 (the signature is the
 * ASN.1 DER form emitted by `openssl dgst -sha256 -sign key.pem`). We verify
 * against an uncompressed P-256 public key (65 bytes: 0x04 || X || Y) using
 * the in-tree BearSSL. BearSSL 0.6 has no Ed25519, so ECDSA P-256 is the
 * vetted primitive we use rather than vendoring new crypto.
 */

#include "verify.h"

#include <bearssl.h>

void herald_sha256(const void *buf, size_t len, unsigned char out[32])
{
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, buf, len);
    br_sha256_out(&c, out);
}

int herald_verify_p256_sha256(const unsigned char *pubkey, size_t pubkey_len,
                              const void *msg, size_t msg_len,
                              const unsigned char *sig, size_t sig_len)
{
    unsigned char hash[32];
    br_ec_public_key pk;

    /* Uncompressed P-256 point is exactly 65 bytes beginning with 0x04. */
    if (pubkey == NULL || pubkey_len != 65 || pubkey[0] != 0x04) {
        return 0;
    }
    if (sig == NULL || sig_len == 0 || msg == NULL) {
        return 0;
    }

    herald_sha256(msg, msg_len, hash);

    pk.curve = BR_EC_secp256r1;
    pk.q = (unsigned char *)pubkey;
    pk.qlen = pubkey_len;

    /* br_ecdsa_i31_vrfy_asn1 returns 1 on a valid signature, 0 otherwise.
     * (BearSSL has no generic br_ecdsa_vrfy_asn1 symbol — pick a concrete
     * implementation; i31 is the integer impl used on 32/64-bit targets.) */
    if (br_ecdsa_i31_vrfy_asn1(&br_ec_p256_m15, hash, sizeof(hash),
                               &pk, sig, sig_len) != 1) {
        return 0;
    }
    return 1;
}
