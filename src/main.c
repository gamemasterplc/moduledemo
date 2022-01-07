#include <ultra64.h>
#include "module.h"
#include "debug.h"

//Global stack variable
u8 main_stack[0x2000] __attribute__((aligned(8)));

static u8 idle_stack[0x800] __attribute__((aligned(8)));

//Define threads
static OSThread idle_thread;
static OSThread main_thread;

//Define PI Data
static OSMesgQueue pi_msg_queue;
static OSMesg pi_msgs[8];

static void main(void *arg)
{
	ModuleHandle *handle1;
	ModuleHandle *handle2;
	debug_printf("Starting program\n");
	ModuleInit();
	debug_printf("Loading module1\n");
	handle1 = ModuleLoad("module1");
	debug_printf("Loading module2\n");
	handle2 = ModuleLoad("module2");
	debug_printf("Unloading module 1\n");
	ModuleUnload(handle1);
	debug_printf("Unloading module 2\n");
	ModuleUnload(handle2);
	//Intentionally cause crash
	*(volatile int *)0xFEDCBA98 = 0;
	while(1);
}

static void idle(void *arg)
{
	//Start PI manager
	osCreatePiManager((OSPri)OS_PRIORITY_PIMGR, &pi_msg_queue, pi_msgs, 8);
	//Start debug library
	debug_initialize();
	//Start main thread
	osCreateThread(&main_thread, 3, main, NULL, &main_stack[0x2000], 10);
	osStartThread(&main_thread);
	//Make this the idle thread
	osSetThreadPri(NULL, 0);
	//Busy wait
	while(1);
}

void boot()
{
	osInitialize();
	osCreateThread(&idle_thread, 1, idle, NULL, &idle_stack[0x800], 10);
	osStartThread(&idle_thread);
}