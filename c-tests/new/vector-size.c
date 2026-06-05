typedef int v4si __attribute__ ((vector_size (16)));
typedef double v2df __attribute__ ((__vector_size__ (16)));

static v4si g = {5, 6};

int main (void) {
  v4si a = {1, 2, 3, 4};
  v4si b = a;
  v2df d = {1.25, 2.5};

  if (sizeof (v4si) != 16) return 1;
  if (_Alignof (v4si) != 16) return 2;
  if (g[0] != 5 || g[1] != 6 || g[2] != 0 || g[3] != 0) return 3;
  b[0] = 9;
  if (b[0] != 9 || b[1] != 2 || b[3] != 4) return 4;
  if (d[0] != 1.25 || d[1] != 2.5) return 5;
  return 0;
}
