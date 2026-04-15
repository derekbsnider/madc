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
#include <asmjit/x86.h>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;

bool madc_verbose = false;

throwstream throwit;

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

    int filearg = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            madc_verbose = true;
            filearg = i + 1;
        } else {
            filearg = i;
            break;
        }
    }

    if ( argc >= 2 && filearg < argc )
    {
	if ( !(tp=prog.tokenize(argv[filearg])) )
	    return 0;
	if ( !prog.parse(tp) )
	    return 0;
	if ( !prog.compile() )
	    return 0;

	struct timeval before, after;

	gettimeofday(&before, NULL);
	prog.execute();
	gettimeofday(&after, NULL);

	DBG(std::cout << "Elapsed time: " << time_diff(before, after) << std::endl);

	return 0;
    }
    std::cout << "Usage: madc [-v|--verbose] <file.mad>" << std::endl;

    return 0;
}
