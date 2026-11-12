// vault.cpp - see vault.h.  The key: 8 ECC rows at the start of OTP page 60 (rows 0xf00-0xf07)
// and a marker row (0xf08) written after them, so a half-written key is never used.  After
// the key is read (or made) the page is soft-locked (OTP SW_LOCK) until the next reset, so
// programs cannot read it from OTP.  It stays in kernel RAM, which T2DOS does not protect:
// a toy key store, not a secure one.  Encryption: AES-128-GCM from BearSSL.
#include <Arduino.h>
#include <string.h>
#include "vault.h"

#if defined(PICO_RP2350)
#include <bearssl/bearssl.h>
#include <pico/rand.h>
#include <pico/bootrom.h>
#include <hardware/structs/otp.h>
#include <hardware/regs/addressmap.h>

#define VAULT_PAGE 60
#define KEY_ROW (VAULT_PAGE * 64)       // 0xf00
#define MARK_ROW (KEY_ROW + 8)
#define MARK 0x5432                     // "T2"

static uint8_t key[16];
static bool have_key;

static uint32_t raw_row(int row) { return ((volatile uint32_t *)OTP_DATA_RAW_BASE)[row]; }
static int ecc_read(int row, uint8_t *buf, int rows) { otp_cmd_t c; c.flags = OTP_CMD_ECC_BITS | row; return rom_func_otp_access(buf, rows * 2, c); }
static int ecc_write(int row, const uint8_t *buf, int rows) {
  otp_cmd_t c; c.flags = OTP_CMD_ECC_BITS | OTP_CMD_WRITE_BITS | row;
  return rom_func_otp_access((uint8_t *)buf, rows * 2, c);
}
static void lock_page() { otp_hw->sw_lock[VAULT_PAGE] = 0xf; }   // secure and non-secure: inaccessible

static bool key_present() {
  uint8_t m[2];
  return ecc_read(MARK_ROW, m, 1) == 0 && (m[0] | m[1] << 8) == MARK;
}

int vault_boot() {
  if (key_present()) {
    have_key = ecc_read(KEY_ROW, key, 8) == 0;
    lock_page();
    return have_key ? 1 : -1;
  }
  return 0;
}

// first use: a random key, written only into blank rows, read back before the marker goes in
static bool make_key() {
  for (int r = KEY_ROW; r <= MARK_ROW; r++) if (raw_row(r)) return false;   // not blank: leave it alone
  rng_128_t r; get_rand_128(&r);
  uint8_t k[16]; memcpy(k, &r, 16); memset(&r, 0, sizeof r);
  uint8_t back[16];
  if (ecc_write(KEY_ROW, k, 8) || ecc_read(KEY_ROW, back, 8) || memcmp(k, back, 16)) return false;
  uint8_t m[2] = { MARK & 0xff, MARK >> 8 };
  if (ecc_write(MARK_ROW, m, 1) || !key_present()) return false;
  memcpy(key, k, 16); memset(k, 0, 16); memset(back, 0, 16);
  have_key = true; lock_page();
  return true;
}

static void gcm_start(br_aes_ct_ctr_keys *ctr, br_gcm_context *gc, const uint8_t *nonce, const char *aad) {
  br_aes_ct_ctr_init(ctr, key, 16);
  br_gcm_init(gc, &ctr->vtable, br_ghash_ctmul32);
  br_gcm_reset(gc, nonce, 12);
  if (aad) br_gcm_aad_inject(gc, aad, strlen(aad));
  br_gcm_flip(gc);
}

int vault_seal(const uint8_t *in, int len, const char *aad, uint8_t *out) {
  if (len < 0 || (!have_key && !make_key())) return -1;
  rng_128_t r; get_rand_128(&r);
  memcpy(out, &r, 12);
  memcpy(out + 12, in, len);
  br_aes_ct_ctr_keys ctr; br_gcm_context gc;
  gcm_start(&ctr, &gc, out, aad);
  br_gcm_run(&gc, 1, out + 12, len);
  br_gcm_get_tag(&gc, out + 12 + len);
  memset(&ctr, 0, sizeof ctr);
  return len + VAULT_OVERHEAD;
}

int vault_open(const uint8_t *in, int len, const char *aad, uint8_t *out) {
  if (!have_key || len < VAULT_OVERHEAD) return -1;
  int n = len - VAULT_OVERHEAD;
  memcpy(out, in + 12, n);
  br_aes_ct_ctr_keys ctr; br_gcm_context gc;
  gcm_start(&ctr, &gc, in, aad);
  br_gcm_run(&gc, 0, out, n);
  bool ok = br_gcm_check_tag(&gc, in + 12 + n);
  memset(&ctr, 0, sizeof ctr);
  if (!ok) { memset(out, 0, n); return -1; }
  return n;
}

#else   // no key store on this chip
int vault_boot() { return -1; }
int vault_seal(const uint8_t *, int, const char *, uint8_t *) { return -1; }
int vault_open(const uint8_t *, int, const char *, uint8_t *) { return -1; }
#endif
