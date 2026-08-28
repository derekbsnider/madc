// Minimal madc embedding example.
//
// Build (after installing libmadc):
//   g++ -std=c++11 -o embed_hello embed_hello.cpp $(pkg-config --cflags --libs libmadc)
//
// Or without pkg-config:
//   g++ -std=c++11 -o embed_hello embed_hello.cpp -lmadc -ldl
//
// madc can also compile this file itself:
//   madc embed_hello.cpp

#include <iostream>
#include <string>

#include <libmadc/engine.h>

// A host callback that scripts can call.
static int64_t host_multiply(int64_t a, int64_t b)
{
	return a * b;
}

int main()
{
	// 1. Create an engine — this owns shared configuration and
	//    registered host callbacks.
	madc::engine eng;

	// 2. Register a host callback on the engine.  Every program
	//    created from this engine will be able to call it.
	eng.register_function("host_multiply", host_multiply);

	// 3. Create a program from the engine.
	madc::program pgm = eng.create_program();

	// 4. Evaluate an expression. The sandboxed expression policy refuses
	//    function calls by default — opt the registered callback in.
	madc::expression_policy expr = pgm.get_expression_policy();
	expr.allow_function_calls = true;
	expr.allowed_functions.push_back("host_multiply");
	pgm.set_expression_policy(expr);
	int64_t result = 0;
	if (pgm.eval_expression("host_multiply(6, 7)", result))
		std::cout << "6 * 7 = " << result << std::endl;

	// 5. Compile and run a full script. (The script's own stdout is
	//    captured by the sandbox for the duration of the run; the host's
	//    streams are untouched.)
	pgm.exec_string(
		"int main() {\n"
		"    cout << \"hello from madc\" << endl;\n"
		"    return 0;\n"
		"}\n");

	// 6. Compile a script with a callable function.
	madc::program pgm2 = eng.create_program();
	pgm2.compile_file(""); // reset for fresh compile
	pgm2.exec_string(
		"int add(int a, int b) { return a + b; }\n"
		"int main() { return 0; }\n",
		"funcs.mad");

	if (pgm2.has_function("add"))
	{
		madc::value sum;
		pgm2.call("add", {madc::value(int64_t(10)), madc::value(int64_t(32))}, &sum);
		std::cout << "10 + 32 = " << sum.as_integer() << std::endl;
	}

	// 7. Check for errors.
	madc::program pgm3 = eng.create_program();
	if (!pgm3.eval_expression("1 / 0"))
	{
		const madc::error *err = pgm3.last_error();
		if (err)
			std::cerr << "error: " << err->message << std::endl;
	}

	return 0;
}
