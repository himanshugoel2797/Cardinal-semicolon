#include "hmac.h"
#include "module_def.h"
#include "sha256.h"
#include <string.h>

uint16_t str2short(const char *val) {

  uint16_t res = 0;

  for (int i = 0; i < 4 && val[i] != 0; i++) {
    uint8_t res0 = 0;

    res = res << 4;

    if (val[i] >= '0' && val[i] <= '9')
      res0 = val[i] - '0';
    else if (val[i] >= 'a' && val[i] <= 'f')
      res0 = val[i] - 'a' + 10;
    else if (val[i] >= 'A' && val[i] <= 'F')
      res0 = val[i] - 'A' + 10;

    res |= res0;
  }

  return res;
}

uint8_t str2byte(const char *val) {

  uint8_t res = 0;

  for (int i = 0; i < 2 && val[i] != 0; i++) {
    uint8_t res0 = 0;

    res = res << 4;

    if (val[i] >= '0' && val[i] <= '9')
      res0 = val[i] - '0';
    else if (val[i] >= 'a' && val[i] <= 'f')
      res0 = val[i] - 'a' + 10;
    else if (val[i] >= 'A' && val[i] <= 'F')
      res0 = val[i] - 'A' + 10;

    res |= res0;
  }

  return res;
}

int VerifyModule(ModuleHeader *hdr, uint8_t *key) {

  ModuleHeader m_hdr;
  memcpy(&m_hdr, hdr, sizeof(ModuleHeader));
  memset(m_hdr.hash, 0, 256 / 8);

  {
    if (strncmp(hdr->magic, MODULE_HEADER_MAGIC, sizeof(MODULE_HEADER_MAGIC)) !=
        0)
      return -1;
  }

  // Compute the key hash first to determine we're working with the right key
  {
    uint8_t key_hash[256 / 8];

    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, key, 256 / 8);
    sha256_final(&ctx, key_hash);

    for (int i = 0; i < 256 / 8; i++)
      if (key_hash[i] != hdr->key_hash[i])
        return -1;
  }

  // Compute the overall hash
  {
    uint8_t hash[256 / 8];

    hmac_ctx ctx;
    hmac_init(&ctx, key);
    hmac_update(&ctx, (uint8_t *)&m_hdr, sizeof(ModuleHeader));
    hmac_update(&ctx, hdr->data, hdr->elf_len);
    hmac_final(&ctx, hash);

    for (int i = 0; i < 256 / 8; i++)
      if (hash[i] != hdr->hash[i])
        return -1;
  }

  return 0;
}

int BuildModuleHeader(ModuleHeader *hdr, const char *module_name,
                      const char *dev_name, const char *dev_name2,
                      const char *min_ver, const char *maj_ver, const char *key,
                      uint8_t *elf, size_t elf_len, size_t uncompressed_len) {

  memset(hdr, 0, sizeof(ModuleHeader));
  strncpy(hdr->magic, MODULE_HEADER_MAGIC, sizeof(MODULE_HEADER_MAGIC));
  strncpy(hdr->module_name, module_name, 256);
  strncpy(hdr->dev_name, dev_name, 256);
  strncpy(hdr->dev_name2, dev_name2, 256);
  hdr->minor_ver = str2short(min_ver);
  hdr->major_ver = str2short(maj_ver);
  hdr->elf_len = elf_len;
  hdr->uncompressed_len = uncompressed_len;

  uint8_t key_i[256 / 8];
  int idx = 0;
  while (*key != 0) {
    key_i[idx] = str2byte(key);
    key += 2;
    idx++;
  }

  // Compute the key hash
  {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, key_i, sizeof(key_i));
    sha256_final(&ctx, hdr->key_hash);
  }

  // Compute the module hash
  {
    uint8_t tmp_hash[256 / 8];

    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (uint8_t *)hdr->module_name, 256);
    sha256_update(&ctx, (uint8_t *)hdr->dev_name, 256);
    sha256_update(&ctx, (uint8_t *)hdr->dev_name2, 256);
    sha256_update(&ctx, (uint8_t *)&hdr->minor_ver, sizeof(hdr->minor_ver));
    sha256_update(&ctx, (uint8_t *)&hdr->major_ver, sizeof(hdr->major_ver));
    sha256_final(&ctx, tmp_hash);

    for (size_t i = 0; i < sizeof(hdr->module_nid); i++) {
      hdr->module_nid = hdr->module_nid << 8;
      hdr->module_nid |= tmp_hash[i];
    }
  }

  // Calculate elf and header hash with key, put them in appropriate field
  // Key hash is 128-bit truncated sha256 hash
  {
    hmac_ctx ctx;
    hmac_init(&ctx, key_i);
    hmac_update(&ctx, (uint8_t *)hdr, sizeof(ModuleHeader));
    hmac_update(&ctx, elf, elf_len);
    hmac_final(&ctx, hdr->hash);
  }

  return 0;
}