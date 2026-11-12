// Checks the structure of the ARM build of the guest corpus: the function
// table at its start must list, for each test, an ARM-state address followed
// by a Thumb-state address, all inside the image.
//
// usage: image_check <image.bin> <load address>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define T(x) #x,
static const char* const names[] = {
#include "tests.def"
};
#undef T

int main(int argc, char** argv) {
  static uint8_t image[1 << 20];
  const int expected = (int)(sizeof names / sizeof names[0]);
  uint32_t base;
  size_t size;
  int i;
  FILE* f;
  if (argc != 3) {
    fprintf(stderr, "usage: image_check <image.bin> <load address>\n");
    return 2;
  }
  base = (uint32_t)strtoul(argv[2], NULL, 0);
  f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  size = fread(image, 1, sizeof image, f);
  fclose(f);
  if (size < (size_t)(expected * 8 + 4)) {
    printf("FAIL: image is too small (%u bytes)\n", (unsigned)size);
    return 1;
  }
  for (i = 0; i <= expected * 2; ++i) {
    const uint8_t* p = image + i * 4;
    const uint32_t entry = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    const int thumb = (i % 2) == 1;
    if (i == expected * 2) {
      if (entry != 0) {
        printf("FAIL: the table is longer than tests.def (%d tests)\n", expected);
        return 1;
      }
      break;
    }
    if ((entry & 1u) != (uint32_t)thumb || (!thumb && (entry & 3u) != 0)) {
      printf("FAIL: %s, %s build: address %08X is in the wrong instruction set\n", names[i / 2],
             thumb ? "Thumb" : "ARM", (unsigned)entry);
      return 1;
    }
    if ((entry & ~1u) < base + (uint32_t)(expected * 8 + 4) || (entry & ~1u) >= base + size) {
      printf("FAIL: %s: address %08X is outside the image\n", names[i / 2], (unsigned)entry);
      return 1;
    }
  }
  printf("ok: %d tests, each in ARM and Thumb state, %u bytes\n", expected, (unsigned)size);
  return 0;
}
