/* aarch64_char_sign.c — plain char is UNSIGNED on aarch64-linux; signed char
   is not. Every route a signed char value takes: a constant cast folded by the
   compiler (from int and from __int128), a static initializer, a run-time
   conversion, a parameter and return. c2mir folded a cast to signed char
   through the target's PLAIN char, so (signed char)200 read 200; madc spelled
   signed char as plain char, so a signed char variable holding -3 read 253. */
int printf (const char *, ...);
static signed char neg(signed char v) { return (signed char)-v; }
static const signed char tab[3] = { -1, (signed char)200, 100 };
int main(void) {
    volatile int k = 200;
    signed char sc = -3; char c = (char)200; unsigned char uc = 200;
    int fold = (signed char)200;
    int fold128 = (signed char)(__int128)456;
    int run = (signed char)k;
    printf("fold: %d %d %d\n", fold, fold128, (int)(signed char)-129);
    printf("run: %d %d %d\n", run, neg(sc), (int)(signed char)(k + 56));
    printf("var: %d %d %d %d\n", sc, c, uc, c < 0);
    printf("tab: %d %d %d\n", tab[0], tab[1], tab[2]);
    return 0;
}
