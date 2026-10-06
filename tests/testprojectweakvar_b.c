/* The other TU of testprojectweakvar (see testprojectweakvar_a.c). */
int wa = 2;
__attribute__((weak)) int wb = 4;
int wc = 6;
int we[2] = {10, 11};
int wf = 14;
int wg = 15;
int read_wb_from_b(void) { extern int wb; return wb; }
