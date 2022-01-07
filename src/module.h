#pragma once

#include "bool.h"

typedef struct module_handle ModuleHandle;

void ModuleInit();
ModuleHandle *ModuleFind(char *name);
bool ModuleIsLoaded(ModuleHandle *handle);
void ModulePrintLoadedList();
ModuleHandle *ModuleLoadHandle(ModuleHandle *handle);
ModuleHandle *ModuleLoad(char *name);
void ModuleUnloadForce(ModuleHandle *handle);
void ModuleUnload(ModuleHandle *handle);
ModuleHandle *ModuleAddrToHandle(void *ptr);