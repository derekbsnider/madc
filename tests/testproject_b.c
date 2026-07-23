static int secret = 200;        /* must not collide with a.c */
extern int from_a(void);
int from_b(void) { return secret + from_a(); }
