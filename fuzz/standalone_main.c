/* Standalone driver for the fuzz harness on toolchains without libFuzzer
 * (e.g. Apple clang). With file arguments, replays each file through
 * LLVMFuzzerTestOneInput (for reproducing CI crash inputs). With no
 * arguments, feeds a deterministic pseudo-random smoke corpus. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static unsigned int xs_state = 0x12345u;
static unsigned int xs_next(void) {
  unsigned int x = xs_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  xs_state = x ? x : 0x9e3779b9u;
  return xs_state;
}

int main(int argc, char **argv) {
  if (argc > 1) {
    for (int i = 1; i < argc; i++) {
      FILE *f = fopen(argv[i], "rb");
      if (!f) {
        perror(argv[i]);
        return 1;
      }
      static uint8_t buf[1 << 20];
      size_t n = fread(buf, 1, sizeof buf, f);
      fclose(f);
      LLVMFuzzerTestOneInput(buf, n);
      printf("replayed %s (%zu bytes)\n", argv[i], n);
    }
    return 0;
  }
  /* Alphabet skewed toward SSE-structural bytes, including an embedded NUL,
   * BOM bytes, digits, and terminators. */
  static const unsigned char alphabet[] = {
    'd', 'a', 't', 'e', 'v', 'i', 'y', 'r', ':', ' ',
    '\n', '\r', '\0', 0xEF, 0xBB, 0xBF,
    'c', 'o', 'm', 'n', 'k', '0', '1', '2', '9', ';', 'x'
  };
  static const size_t alphabet_len = sizeof(alphabet) / sizeof(alphabet[0]);
  uint8_t buf[512];
  for (int iter = 0; iter < 200000; iter++) {
    size_t n = xs_next() % sizeof buf;
    for (size_t i = 0; i < n; i++) {
      buf[i] = alphabet[xs_next() % (alphabet_len - 1)];
    }
    LLVMFuzzerTestOneInput(buf, n);
  }
  printf("smoke ok\n");
  return 0;
}
