#include <stdio.h>

#include "miniz.h"
#include "module_def.h"

static tdefl_compressor g_deflator;

int showHelp(char *a) {
  printf("%s %s\r\n", a, "[module_name] [dev_name1] [dev_name2] [maj_ver] "
                         "[min_ver] [key] [elf] -o [output_file]");
  return -1;
}

int main(int argc, char *argv[]) {

  // [module_name] [dev_name1] [dev_name2] [maj_ver] [min_ver] [key] [elf]
  // -o [output_file]

  if (argc < 10) {
    return showHelp(argv[0]); // Not enough arguments
  }

  if (strcmp(argv[8], "-o") != 0) {
    return showHelp(argv[0]);
  }

  char *module_name = argv[1];
  char *dev_name = argv[2];
  char *dev_name2 = argv[3];
  char *major_ver = argv[4];
  char *minor_ver = argv[5];
  char *key = argv[6];
  char *elf_path = argv[7];
  char *output_file = argv[9];

  // Read the elf file
  FILE *elf_file = fopen(elf_path, "rb");
  if (elf_file == NULL) {
    printf("%s\r\n", "Cannot open ELF file.");
    return -1;
  }

  fseek(elf_file, 0, SEEK_END);
  size_t elf_sz = ftell(elf_file);
  fseek(elf_file, 0, SEEK_SET);

  // Compress the elf file using the miniz library
  uint8_t *src_buffer = malloc(elf_sz);
  if (src_buffer == NULL) {
    printf("%s\r\n", "Failed to allocate memory.");
    return -1;
  }
  fread(src_buffer, 1, elf_sz, elf_file);
  fclose(elf_file);

  /*
  size_t cmp_len = elf_sz;
  uint8_t *dst_buffer = malloc(cmp_len);

  if (dst_buffer == NULL) {
    printf("%s\r\n", "Failed to allocate memory.");
    return -1;
  }

  mz_uint comp_flags = TDEFL_WRITE_ZLIB_HEADER | 256;

  // Initialize the low-level compressor.
  mz_uint status = tdefl_init(&g_deflator, NULL, NULL, comp_flags);
  if (status != TDEFL_STATUS_OKAY) {
    printf("tdefl_init() failed!\n");
    return -1;
  }

  size_t rem_elf_sz = elf_sz;
  status = tdefl_compress(&g_deflator, src_buffer, &rem_elf_sz, dst_buffer,
                          &cmp_len, TDEFL_FINISH);
  if (status != TDEFL_STATUS_DONE) {
    printf("tdefl_compress() failed!\n");
    return -1;
  }*/

  // Call into the module library to generate the final output
  ModuleHeader hdr;
  BuildModuleHeader(&hdr, module_name, dev_name, dev_name2, minor_ver,
                    major_ver, key, src_buffer, elf_sz, elf_sz);

  FILE *fd = fopen(output_file, "wb");
  fwrite(&hdr, 1, sizeof(hdr), fd);
  fwrite(src_buffer, 1, elf_sz, fd);
  fclose(fd);

  return 0;
}