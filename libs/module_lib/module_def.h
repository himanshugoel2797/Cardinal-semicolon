#ifndef CARDINAL_MODULE_DEF_H
#define CARDINAL_MODULE_DEF_H

#include <stddef.h>
#include <stdint.h>

#define MODULE_HEADER_MAGIC ("CELF")

typedef struct {
  uint64_t **dest_addr; // Pointer to function pointer
  uint32_t idx;
} ImportFunction;

typedef struct {
  uint64_t module_nid;
  uint32_t function_cnt;
  ImportFunction *functions;
} Import;

typedef struct {
  uint16_t min_ver;
  uint64_t *func_ptr; // Function Pointer
} Export;

typedef struct {
  char magic[4]; //'CELF'
  char module_name[256];
  char dev_name[256];
  char dev_name2[256];
  uint16_t major_ver;
  uint16_t minor_ver;
  uint32_t elf_len;
  uint32_t uncompressed_len;
  uint64_t module_nid;
  uint8_t key_hash[256 / 8];
  uint8_t hash[256 / 8];
  uint8_t data[0];
} ModuleHeader;

int VerifyModule(ModuleHeader *hdr, uint8_t *key);

int BuildModuleHeader(ModuleHeader *hdr, const char *module_name,
                      const char *dev_name, const char *dev_name2,
                      const char *min_ver, const char *maj_ver, const char *key,
                      uint8_t *elf, size_t elf_len, size_t uncompressed_len);

#endif