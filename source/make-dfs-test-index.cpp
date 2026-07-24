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
    writer.next(NULL, 0, 0);
  }

  if (fclose(fp) != 0) {
    fprintf(stderr, "error: can't finish \"%s\"\n", argv[1]);
    return 1;
  }
  return 0;
}
