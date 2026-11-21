// vault.h - sealing small secrets (the saved WiFi network) with a key only this board has.
// This is the only place that knows where the key lives: today a random AES-128 key in
// RP2350 OTP page 60, later possibly a secure element (e.g. ATECC608) on a user board.
#pragma once
#include <stdint.h>

#define VAULT_OVERHEAD 28   // a sealed blob is 12 bytes nonce + the data + 16 bytes tag

// at boot: loads the key if one was made earlier and locks its OTP page until reset.
// Returns 1 if the key is ready, 0 if none was made yet, -1 if the board has no key store.
int vault_boot();
// seals len bytes (aad: authenticated, not secret, may be 0) into out (len + VAULT_OVERHEAD
// bytes); makes the key on first use.  Returns the sealed length, or -1.
int vault_seal(const uint8_t *in, int len, const char *aad, uint8_t *out);
// opens a sealed blob into out (len - VAULT_OVERHEAD bytes).  Returns the data length, or -1
// if there is no key or the blob was not sealed by this board with the same aad.
int vault_open(const uint8_t *in, int len, const char *aad, uint8_t *out);
