#include "libmadc/api.h"

#include <iostream>
#include <string>

int main()
{
	int64_t answer = 0;
	if ( !madc::eval_expression("6 * 7", answer, "cpp_smoke_expr.mad") )
	{
		std::cerr << "eval_expression failed" << std::endl;
		return 1;
	}
	if ( answer != 42 )
	{
		std::cerr << "unexpected integer result" << std::endl;
		return 2;
	}

	std::string text;
	if ( !madc::eval_body("return \"hello\";\n", text, "cpp_smoke_body.mad") )
	{
		std::cerr << "eval_body failed" << std::endl;
		return 3;
	}
	if ( text != "hello" )
	{
		std::cerr << "unexpected string result" << std::endl;
		return 4;
	}

	std::cout << "cpp-smoke-ok" << std::endl;
	return 0;
}
