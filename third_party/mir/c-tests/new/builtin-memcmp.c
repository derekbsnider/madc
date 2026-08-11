int main (void) {
  int a[2] = {1, 2};
  int b[2] = {1, 2};
  int c[2] = {1, 3};

  if (__builtin_memcmp (a, b, sizeof (a)) != 0)
    return 1;
  if (__builtin_memcmp (a, c, sizeof (a)) == 0)
    return 2;
  return 0;
}
