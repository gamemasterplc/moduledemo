#include <ultra64.h>
#include "debug.h"

extern void CounterPrint();

void _prolog()
{
	debug_printf("Entering module1's prolog\n");
	debug_printf("cosf(0) = %f", cosf(0));
	CounterPrint();
}

void _epilog()
{
	debug_printf("Entering module1's epilog\n");
	CounterPrint();
}

void _unresolved()
{
	debug_printf("module1 called an unlinked function\n");
}