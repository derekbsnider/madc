/* Minimal madc C embedding example.
 *
 * Build (after installing libmadc):
 *   cc -o embed_hello_c embed_hello.c $(pkg-config --cflags --libs libmadc)
 *
 * Or without pkg-config:
 *   cc -o embed_hello_c embed_hello.c -lmadc -lasmjit -ldl -lstdc++
 */

#include <stdio.h>
#include <madc_api.h>

int main(void)
{
	madc_value result;
	madc_value_init(&result);

	/* Create an engine and a program from it. */
	madc_engine *eng = madc_engine_create();
	madc_program *pgm = madc_engine_create_program(eng);

	/* Evaluate an expression. */
	if (madc_program_eval_expression(pgm, "6 * 7", &result, NULL) == MADC_OK)
		printf("6 * 7 = %ld\n", (long)result.integer_value);

	/* Run a full script. */
	madc_program_destroy(pgm);
	pgm = madc_engine_create_program(eng);
	madc_program_exec_string(pgm,
		"int main() {\n"
		"    cout << \"hello from madc (C host)\" << endl;\n"
		"    return 0;\n"
		"}\n",
		"hello.mad");

	/* Check for errors. */
	const char *err = madc_program_last_error(pgm);
	if (err && err[0])
		fprintf(stderr, "error: %s\n", err);

	madc_value_clear(&result);
	madc_program_destroy(pgm);
	madc_engine_destroy(eng);
	return 0;
}
