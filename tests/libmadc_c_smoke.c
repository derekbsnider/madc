#include "madc_api.h"

#include <stdio.h>

int main(void)
{
	madc_program *pgm = madc_program_create();
	madc_value result;
	int rc = 0;

	if ( pgm == NULL )
		return 1;

	madc_value_init(&result);
	rc = madc_program_eval_expression(pgm, "6 * 7", &result, "c_smoke_expr.mad");
	if ( rc != MADC_OK )
	{
		madc_program_destroy(pgm);
		return 2;
	}
	if ( result.type_id != MADC_VALUE_INTEGER || result.integer_value != 42 )
	{
		madc_value_clear(&result);
		madc_program_destroy(pgm);
		return 3;
	}

	madc_value_clear(&result);
	rc = madc_program_eval_unit(pgm,
				    "int __madc_eval() { return 9; }\n",
				    &result,
				    "c_smoke_unit.mad");
	if ( rc != MADC_OK )
	{
		madc_program_destroy(pgm);
		return 4;
	}
	if ( result.type_id != MADC_VALUE_INTEGER || result.integer_value != 9 )
	{
		madc_value_clear(&result);
		madc_program_destroy(pgm);
		return 5;
	}

	puts("c-smoke-ok");
	madc_value_clear(&result);
	madc_program_destroy(pgm);
	return 0;
}
