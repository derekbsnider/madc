/* aarch64_nansl.c — __builtin_nansl on aarch64-linux, whose long double is
   IEEE binary128: a signaling NaN is exponent all ones with the quiet bit
   (the fraction's top bit) clear and the next bit set — high word
   0x7fff400000000000, low word 0. madc's macro wrote the x87 80-bit pattern
   (mantissa 0xa000000000000000 in the low word, 0x7fff in the next), which
   binary128 reads as a small finite value, not a NaN. The quiet NaN and the
   infinity are the guards: they fold from arithmetic and were right. */
int printf (const char *, ...);
#define DUMP(tag, expr) do { long double v_ = (expr); unsigned char b_[16]; \
    __builtin_memcpy(b_, &v_, 16); printf("%s=", tag); \
    for (int i_ = 15; i_ >= 0; --i_) printf("%02x", b_[i_]); printf("\n"); } while (0)
int main(void) {
    DUMP("nansl", __builtin_nansl(""));
    DUMP("nanl", __builtin_nanl(""));
    DUMP("infl", __builtin_infl());
    return 0;
}
