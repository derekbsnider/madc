#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <queue>
#include <stack>
#include <asmjit/asmjit.h>
#define DBG(x)
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;

double time_diff(struct timeval x , struct timeval y)
{
	double x_ms , y_ms , diff;
	
	x_ms = (double)x.tv_sec*1000000 + (double)x.tv_usec;
	y_ms = (double)y.tv_sec*1000000 + (double)y.tv_usec;
	
	diff = (double)y_ms - (double)x_ms;
	
	return diff;
}

int main(int argc, char **argv)
{
    stringstream ss;
    Program prog;
    TokenProgram *tp;

    prog.colors = true;

    if ( argc >= 2 )
    {
	asmjit::String sb, hsb;
	DBG(prog.code.init(prog.jit.codeInfo()));
	if ( !(tp=prog.tokenize(argv[1])) )
	    return 0;
	if ( !prog.parse(tp) )
	    return 0;
	if ( !prog.compile() )
	    return 0;
	DBG(puts("Assember:"));
	DBG(prog.cc.dump(sb));
	DBG(puts(sb.data()));

	struct timeval before, after;

	gettimeofday(&before, NULL);
	prog.execute();
	gettimeofday(&after, NULL);

	DBG(std::cout << "Elapsed time: " << time_diff(before, after) << std::endl);

	return 0;
    }
    std::cout << "Usage: madc <file.mad>" << std::endl;

    return 0;
}
