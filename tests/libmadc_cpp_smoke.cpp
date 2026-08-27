#include "libmadc/api.h"
#include "libmadc/engine.h"

#include <iostream>
#include <string>

int main()
{
	// Constructing an engine / running guest code must NOT hijack the
	// HOST's iostream buffers: guest capture is invocation-scoped. The
	// old construction-time bind left std::cout/std::cerr pointed into
	// the engine's buffer for the life of the process — every host
	// iostream write after `madc::engine eng;` silently vanished.
	std::streambuf *host_out = std::cout.rdbuf();
	std::streambuf *host_err = std::cerr.rdbuf();
	{
		madc::engine eng;
		if ( std::cout.rdbuf() != host_out
		  || std::cerr.rdbuf() != host_err )
		{
			std::cerr << "engine construction hijacked host iostreams"
				  << std::endl;
			return 10;
		}
		madc::program pgm = eng.create_program();
		pgm.exec_string("int main() { cout << \"guest\" << endl;"
				" return 0; }\n");
		if ( std::cout.rdbuf() != host_out
		  || std::cerr.rdbuf() != host_err )
		{
			std::cerr << "guest run left host iostreams rebound"
				  << std::endl;
			return 11;
		}
	}

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
