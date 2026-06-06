#include "../mir.h"

#include "scan-sieve.h"

int main (void) {
  FILE *f;
#ifndef _WIN32
  const char *fname = "/tmp/__tmp.mirb";
#else
  const char *fname = "__tmp.mirb";
#endif
  MIR_context_t ctx = MIR_init ();

  create_mir_func_sieve (ctx, NULL, NULL);
  MIR_scan_string (ctx,
                   "\n\
m_v128_data: module\n\
vec_data:    v128   1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16\n\
             endmodule\n\
");
  MIR_output (ctx, stderr);
  f = fopen (fname, "wb");
  mir_assert (f != NULL);
  MIR_write (ctx, f);
  fclose (f);
  f = fopen (fname, "rb");
  mir_assert (f != NULL);
  MIR_read (ctx, f);
  fclose (f);
  fprintf (stderr, "+++++++++++++After reading:\n");
  MIR_output (ctx, stderr);
  remove (fname);
  MIR_finish (ctx);
  return 0;
}
