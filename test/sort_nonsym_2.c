#include <stdlib.h>

char aa[] = { 1, 2, 3 };

// CHECK: comparison function is not symmetric
int cmp(const void *pa, const void *pb) {
  char a = *(const char *)pa;
  char b = *(const char *)pb;
  return a < b ? -1 : a == b ? 1 : 1;
}

int main() {
  qsort(aa, sizeof(aa), 1, cmp);
  return 0;
}

