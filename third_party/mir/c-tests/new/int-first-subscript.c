/* E1[E2] with the integer first (C11 6.5.2.1p2): int, long and struct
   elements, read and written, through an array and through a pointer. */
struct s {
  int a, b;
};
int main (void) {
  int ia[4] = {10, 20, 30, 40};
  long la[3] = {1, 2, 3};
  struct s sa[2] = {{1, 2}, {3, 4}};
  int *p = ia;
  int i = 2;
  1[ia] = 21;
  if (1[ia] != 21 || ia[1] != 21) return 1;
  if ((i - 1)[ia] != 21 || i[p] != 30) return 2;
  if (2[la] != 3) return 3;
  if (1[sa].b != 4) return 4;
  if (3["abcd"] != 'd') return 5;
  return 0;
}
