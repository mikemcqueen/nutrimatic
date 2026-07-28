#include "index.h"

#include <stdio.h>

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s output.index\n", argv[0]);
    return 2;
  }

  FILE* fp = fopen(argv[1], "w+b");
  if (fp == NULL) {
    fprintf(stderr, "error: can't create \"%s\"\n", argv[1]);
    return 1;
  }

  {
    IndexWriter writer(fp);
    writer.next("ab ", 0, 10);
    writer.next("ab cd ", 0, 70);
    writer.next("ba ", 0, 5);
    writer.next("cd ", 0, 7);
    writer.next("dc ", 0, 2);
    // "f" can only complete "fghij" through the phrase "gh ij".
    writer.next("f ", 0, 10);
    writer.next("gh ij ", 0, 5);
    // "uv" can only complete "qrstuv" through the phrase "qr st".
    writer.next("qr st ", 0, 3);
    writer.next("uv ", 0, 8);
    // Disjoint letters from the a-d fixture above: used by the
    // query-index --require-completable smoke test. "wx" and "yz" tile
    // the full "wxyz" bag exactly (each is the other's remainder), while
    // "xy" leaves "wz" behind, which nothing can complete.
    writer.next("wx ", 0, 6);
    // Low raw frequency, but --word-bonus can move it above the -n floor.
    writer.next("wx yz ", 0, 1);
    writer.next("xy ", 0, 4);
    writer.next("yz ", 0, 5);
    writer.next(NULL, 0, 0);
  }

  if (fclose(fp) != 0) {
    fprintf(stderr, "error: can't finish \"%s\"\n", argv[1]);
    return 1;
  }
  return 0;
}
