#include <ultra64.h>
#include "malloc.h"
#include "debug.h"

static int *counter_ptr;

void CounterPrint()
{
	debug_printf("*counter_ptr = %d\n", *counter_ptr);
}

//First thing to run in this module
__attribute__((constructor)) static void InitCounter()
{
	debug_printf("Running global constructor\n");
	counter_ptr = malloc(sizeof(int));
	*counter_ptr = 523;
	CounterPrint();
}

__attribute__((destructor)) static void DestroyCounter()
{
	debug_printf("Running global destructor\n");
	CounterPrint();
	free(counter_ptr);
}

void _prolog()
{
	debug_printf("Entering module2's prolog\n");
	for(int i=0; i<5; i++) {
		(*counter_ptr)++;
	}
}

void _epilog()
{
	debug_printf("Entering module2's epilog\n");
	for(int i=0; i<17; i++) {
		(*counter_ptr)++;
	}
}