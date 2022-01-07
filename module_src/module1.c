#include <ultra64.h>
#include "debug.h"

extern void CounterPrint();

void _prolog()
{
	debug_printf("Entering module1's prolog\n");
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