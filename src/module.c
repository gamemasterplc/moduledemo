#include <ultra64.h>
#include "malloc.h"
#include "libcext.h"
#include "module.h"
#include "rom_read.h"
#include "debug.h"

#define R_MIPS_32 2
#define R_MIPS_26 4
#define R_MIPS_HI16 5
#define R_MIPS_LO16 6
#define R_ULTRA_SEC 100

#define SHN_UNDEF 0

typedef void (*ModuleFunc)();

typedef struct module_section {
	void *ptr;
	u32 align;
	u32 size;
} ModuleSection;

typedef struct reloc_entry {
	u32 offset;
	u8 type;
	u8 pad;
	u16 section;
	u32 sym_ofs;
} RelocEntry;

typedef struct import_module {
	u32 module_id;
	u32 num_relocs;
	RelocEntry *relocs;
} ImportModule;

typedef struct module_header {
	u32 num_sections;
	ModuleSection *section_info;
	u32 num_import_modules;
	ImportModule *import_modules;
	u16 ctor_section;
	u16 dtor_section;
	u16 prolog_section;
	u16 epilog_section;
	u16 unresolved_section;
	u16 pad;
	ModuleFunc prolog;
	ModuleFunc epilog;
	ModuleFunc unresolved;
} ModuleHeader;

struct module_handle {
	char *name;
	u32 module_align;
	u32 module_size;
	u32 rom_ofs;
	u32 noload_align;
	u32 noload_size;
	u32 ref_count;
	ModuleHeader *module;
};

extern u8 __module_romdata[];

static u32 num_modules;
static ModuleHandle *module_handle_data;

static inline u32 AlignValue(u32 value, u32 alignment)
{
	return (value+alignment-1) & ~(alignment-1);
}

static inline void *AlignPtr(void *ptr, u32 alignment)
{
	return (void *)AlignValue((u32)ptr, alignment);
}

static void FixupModuleHandles()
{
	for(u32 i=0; i<num_modules; i++) {
		module_handle_data[i].name += (u32)module_handle_data;
		module_handle_data[i].rom_ofs += (u32)__module_romdata+8;
		module_handle_data[i].ref_count = 0;
		module_handle_data[i].module = NULL;
	}
}

void ModuleInit()
{
	u32 read_size;
	u32 strtab_size __attribute__((aligned(8)));
	RomRead(&num_modules, (u32)__module_romdata, 4);
	RomRead(&strtab_size, (u32)__module_romdata+4, 4);
	//Allocate and read handles
	read_size = (num_modules*sizeof(ModuleHandle))+strtab_size;
	if(read_size != 0) {
		module_handle_data = malloc(read_size);
		debug_assert(module_handle_data != NULL);
		RomRead(module_handle_data, (u32)__module_romdata+8, read_size);
		FixupModuleHandles();
	}
}

static u32 GetModuleRamAlign(ModuleHandle *handle)
{
	//Return bigger of module_align and noload_align
	u32 align_val = handle->module_align;
	if(align_val < handle->noload_align) {
		align_val = handle->noload_align;
	}
	return align_val;
}

static u32 GetModuleRamSize(ModuleHandle *handle)
{
	return AlignValue(handle->module_size, handle->noload_align)+handle->noload_size;
}

static void *GetModuleBssPtr(ModuleHandle *handle)
{
	//Placed immediately after end of ROM data aligned to handle->noload_align
	u32 base = (u32)handle->module;
	u32 ofs = AlignValue(handle->module_size, handle->noload_align);
	return (void *)(base+ofs);
}

ModuleHandle *ModuleFind(char *name)
{
	//Search through module names
	for(u32 i=0; i<num_modules; i++) {
		if(!strcmp(name, module_handle_data[i].name)) {
			//Found module name
			return &module_handle_data[i];
		}
	}
	//Couldn't find module name
	return NULL;
}

static void PatchModuleSections(ModuleHeader *module, void *bss)
{
	u8 *bss_ptr = bss;
	//Patch section info header pointer
	module->section_info = (ModuleSection *)((u32)module+(u32)module->section_info);
	for(u32 i=0; i<module->num_sections; i++) {
		ModuleSection *section = &module->section_info[i];
		if(section->ptr) {
			//Patch non-BSS section pointer
			section->ptr = (void *)((u32)module+(u32)section->ptr);
		} else {
			if(section->size) {
				bss_ptr = AlignPtr(bss_ptr, section->align); //Align BSS section pointet
				section->ptr = bss_ptr;
				//Advance BSS section pointer
				bss_ptr += module->section_info[i].size;
			}
		}
	}
}

static void PatchModuleImports(ModuleHeader *module)
{
	//Patch import module list header pointer
	module->import_modules = (ImportModule *)((u32)module+(u32)module->import_modules);
	for(u32 i=0; i<module->num_import_modules; i++) {
		ImportModule *import = &module->import_modules[i];
		//Patch import module relocation pointer
		import->relocs = (RelocEntry *)((u32)module+(u32)import->relocs);
	}
}

static void *GetSectionPtr(ModuleHeader *module, u16 index, u32 offset)
{
	if(module && index < module->num_sections) {
		//Indexing into valid section
		return (char *)module->section_info[index].ptr+offset;
	} else {
		//Indexing into invalid section
		return (void *)offset;
	}
}

static void FlushSection(ModuleHeader *module, u16 section)
{
	//Skip invalid sections
	if(section >= module->num_sections) {
		return;
	}
	void *section_ptr = module->section_info[section].ptr;
	u32 section_size = module->section_info[section].size;
	//Flush only sections with at least some data
	if(section_ptr && section_size) {
		osWritebackDCache(section_ptr, section_size);
		osInvalICache(section_ptr, section_size);
	}
}

static void ApplyModuleImportRelocs(ModuleHeader *module, ImportModule *import)
{
	ModuleHeader *src_module = NULL;
	//Get module pointer
	if(import->module_id != 0) {
		src_module = module_handle_data[import->module_id-1].module;
	}
	if(import->module_id == 0 || src_module) {
		//Module loaded or static module
		u16 cur_section = SHN_UNDEF; //Save section for getting relocation pointer
		for(u32 i=0; i<import->num_relocs; i++) {
			RelocEntry *reloc = &import->relocs[i];
			u32 *reloc_ptr = GetSectionPtr(module, cur_section, reloc->offset);
			switch(reloc->type) {
				case R_MIPS_32:
				//Direct 32-bit pointer relocations
				{
					u32 sym_ptr = (u32)GetSectionPtr(src_module, reloc->section, reloc->sym_ofs);
					*reloc_ptr += sym_ptr;
				}
					break;
					
				case R_MIPS_26:
				//26-bit relative pointer relocations
				//Used for Jumps
				{
					u32 sym_ptr = (u32)GetSectionPtr(src_module, reloc->section, reloc->sym_ofs);
					//Extract original target from instruction and address
					u32 target = ((*reloc_ptr & 0x3FFFFFF) << 2)|((u32)reloc_ptr & 0xF0000000);
					//Hack for unresolved functions
					if(target == (u32)module->unresolved) {
						target -= ((u32)module->unresolved & 0xFFFFFFC);
					}
					target += (sym_ptr & 0xFFFFFFC);
					*reloc_ptr = (*reloc_ptr & 0xFC000000)|((target & 0xFFFFFFC) >> 2);
				}
					break;
					
				case R_MIPS_HI16:
				//High half of direct pointers
				{
					u16 hi_orig = *reloc_ptr & 0xFFFF;
					u32 addr = hi_orig << 16;
					u16 hi = hi_orig;
					//Calculate real hi using next lo
					for(u32 j=i+1; j<import->num_relocs; j++) {
						RelocEntry *new_reloc = &import->relocs[j];
						if(new_reloc->type == R_MIPS_LO16) {
							//Found lo
							u32 sym_ptr = (u32)GetSectionPtr(src_module, new_reloc->section, new_reloc->sym_ofs);
							u32 *lo_ptr = GetSectionPtr(module, cur_section, new_reloc->offset);
							u16 lo = *lo_ptr & 0xFFFF;
							//Calculate effective address with lo and symbol pointer
							addr += lo-((lo & 0x8000) << 1);
							addr += sym_ptr;
							//Calculate hi from effective address
							hi = (addr >> 16)+((addr & 0x8000) >> 15);
							break;
						}
					}
					//Write hi
					*reloc_ptr = (*reloc_ptr & 0xFFFF0000)|hi;
				}
					break;
					
				case R_MIPS_LO16:
				{
					u32 sym_ptr = (u32)GetSectionPtr(src_module, reloc->section, reloc->sym_ofs);
					u16 lo = *reloc_ptr & 0xFFFF;
					lo += sym_ptr;
					*reloc_ptr = (*reloc_ptr & 0xFFFF0000)|lo;
				}
					break;
					
				case R_ULTRA_SEC:
					//Flush cache of previous section
					FlushSection(module, cur_section);
					//Change section
					cur_section = reloc->section;
					break;
					
				default:
					debug_printf("Unknown relocation type %d.\n", reloc->type);
					break;
			}
		}
		//Flush cache of last section
		FlushSection(module, cur_section);
	} else if(!src_module) {
		//Module not loaded
		u16 cur_section = SHN_UNDEF; //Save section for getting relocation pointer
		for(u32 i=0; i<import->num_relocs; i++) {
			RelocEntry *reloc = &import->relocs[i];
			u32 *reloc_ptr = GetSectionPtr(module, cur_section, reloc->offset);
			switch(reloc->type) {
				case R_MIPS_32:
					break;
					
				case R_MIPS_26:
				//Make it target module->unresolved
				{
					u32 sym_ptr = (u32)module->unresolved;
					//Extract original target from instruction and address
					u32 target = ((*reloc_ptr & 0x3FFFFFF) << 2)|((u32)reloc_ptr & 0xF0000000);
					//Only patch calls targeted to 0
					if(target == ((u32)reloc_ptr & 0xF0000000)) {
						target += (sym_ptr & 0xFFFFFFC);
						*reloc_ptr = (*reloc_ptr & 0xFC000000)|((target & 0xFFFFFFC) >> 2);
					}
				}
					break;
					
				case R_MIPS_HI16:
					break;
					
				case R_MIPS_LO16:
					break;
					
				case R_ULTRA_SEC:
					//Flush cache of previous section
					FlushSection(module, cur_section);
					//Change section
					cur_section = reloc->section;
					break;
					
				default:
					debug_printf("Unknown relocation type %d.\n", reloc->type);
					break;
			}
		}
		//Flush cache of last section
		FlushSection(module, cur_section);
	}
}

static void ApplyRelocs(ModuleHeader *module)
{
	//Apply Import relocations for all import modules
	for(u32 i=0; i<module->num_import_modules; i++) {
		ApplyModuleImportRelocs(module, &module->import_modules[i]);
	}
}

static u32 GetModuleID(ModuleHeader *module)
{
	//Search for module pointer in module handles
	for(u32 i=0; i<num_modules; i++) {
		if(module_handle_data[i].module == module) {
			return i+1;
		}
	}
	return 0;
}

static void FixupExternalModuleReferences(ModuleHeader *module)
{
	u32 module_id = GetModuleID(module);
	//Check for invalid module ID
	if(module_id == 0) {
		return;
	}
	//Loop through all modules
	for(u32 i=0; i<num_modules; i++) {
		//Skip this module
		if(i+1 != module_id) {
			//Check for loaded module
			ModuleHeader *module2 = module_handle_data[i].module;
			if(!module2) {
				continue;
			}
			//Apply import relocations applying to module module_id
			for(u32 j=0; j<module2->num_import_modules; j++) {
				if(module2->import_modules[j].module_id == module_id) { 
					ApplyModuleImportRelocs(module2, &module2->import_modules[j]);
					break;
				}
			}
		}
	}
}

void ModulePrintLoadedList()
{
	u32 num_loaded = 0;
	debug_printf("\nLoaded module list:\n");
	for(u32 i=0; i<num_modules; i++) {
		ModuleHandle *handle = &module_handle_data[i];
		if(handle->module) {
			//Print module information if loaded
			u32 top = (u32)handle->module;
			u32 bottom = top+GetModuleRamSize(handle);
			debug_printf("%s (%08x-%08x)\n", handle->name, top, bottom);
			num_loaded++;
		}
	}
	//Print none if no modules were loaded
	if(num_loaded == 0) {
		debug_printf("None\n");
	}
}

static void DefaultUnresolvedHandler()
{
	u32 call_addr = (u32)__builtin_return_address(0)-8;
	ModuleHandle *handle = ModuleAddrToHandle((void *)call_addr);
	debug_printf("Call to module not loaded from module %s at address %08x.\n", handle->name, call_addr);
	ModulePrintLoadedList();
	while(1);
}

static void LinkModule(ModuleHeader *module, void *bss)
{
	//Fixup header pointers
	PatchModuleSections(module, bss);
	PatchModuleImports(module);
	//Fixup function pointers
	if(module->prolog_section != SHN_UNDEF) {
		module->prolog = (ModuleFunc)GetSectionPtr(module, module->prolog_section, (u32)module->prolog);
	} else {
		module->prolog = NULL;
	}
	if(module->epilog_section != SHN_UNDEF) {
		module->epilog = (ModuleFunc)GetSectionPtr(module, module->epilog_section, (u32)module->epilog);
	} else {
		module->epilog = NULL;
	}
	if(module->unresolved_section != SHN_UNDEF) {
		module->unresolved = (ModuleFunc)GetSectionPtr(module, module->unresolved_section, (u32)module->unresolved);
	} else {
		module->unresolved = DefaultUnresolvedHandler;
	}
	//Relocate
	ApplyRelocs(module);
	FixupExternalModuleReferences(module);
}

bool ModuleIsLoaded(ModuleHandle *handle)
{
	return handle->ref_count != 0 && handle->module;
}

static void RunCtors(ModuleHeader *module)
{
	if(module->ctor_section == SHN_UNDEF) {
		return;
	}
	ModuleFunc *start = module->section_info[module->ctor_section].ptr;
	ModuleFunc *end = start+(module->section_info[module->ctor_section].size/sizeof(ModuleFunc));
	ModuleFunc *curr = start;
	while(curr < end) {
		(*curr)();
		curr++;
	}
}

ModuleHandle *ModuleLoadHandle(ModuleHandle *handle)
{
	debug_assert(handle);
	if(!handle->module) {
		//Load module
		u32 module_align = GetModuleRamAlign(handle);
		if(module_align <= 8) {
			//Malloc guarantees 8-byte alignment on this platform
			handle->module = malloc(GetModuleRamSize(handle));
		} else {
			handle->module = memalign(module_align, GetModuleRamSize(handle));
		}
		debug_assert(handle->module);
		memset(handle->module, 0, GetModuleRamSize(handle)); //Zero out module memory
		RomRead(handle->module, handle->rom_ofs, handle->module_size); //Read Module
		LinkModule(handle->module, GetModuleBssPtr(handle));
		//Run Constructors
		RunCtors(handle->module);
		//Run module prolog
		if(handle->module->prolog) {
			handle->module->prolog();
		}
		//Initialize reference count
		handle->ref_count = 1;
	} else {
		//Increment reference count
		handle->ref_count++;;
	}
	//Return handle
	return handle;
}

ModuleHandle *ModuleLoad(char *name)
{
	ModuleHandle *handle = ModuleFind(name);
	if(!handle) {
		//Module not found
		return NULL;
	}
	return ModuleLoadHandle(handle);
}

static void UndoModuleImportRelocs(ModuleHeader *module, ImportModule *import)
{
	//Get module pointer
	ModuleHeader *src_module = NULL;
	if(import->module_id != 0) {
		src_module = module_handle_data[import->module_id-1].module;
	}
	if(src_module) {
		//Module loaded
		u16 cur_section = SHN_UNDEF; //Save section for getting relocation pointer
		for(u32 i=0; i<import->num_relocs; i++) {
			RelocEntry *reloc = &import->relocs[i];
			u32 *reloc_ptr = GetSectionPtr(module, cur_section, reloc->offset);
			switch(reloc->type) {
				case R_MIPS_32:
				//Direct 32-bit pointer relocations
				{
					u32 sym_ptr = (u32)GetSectionPtr(src_module, reloc->section, reloc->sym_ofs);
					*reloc_ptr -= sym_ptr;
				}
					break;
					
				case R_MIPS_26:
				//26-bit relative pointer relocations
				//Used for Jumps
				{
					u32 sym_ptr = (u32)GetSectionPtr(src_module, reloc->section, reloc->sym_ofs);
					//Extract original target from instruction and address
					u32 target = ((*reloc_ptr & 0x3FFFFFF) << 2)|((u32)reloc_ptr & 0xF0000000);
					target -= (sym_ptr & 0xFFFFFFC);
					//Retarget to unresolved function
					target += ((u32)module->unresolved & 0xFFFFFFC);
					*reloc_ptr = (*reloc_ptr & 0xFC000000)|((target & 0xFFFFFFC) >> 2);
				}
					break;
					
				case R_MIPS_HI16:
				//High half of direct pointers
				{
					u16 hi_orig = *reloc_ptr & 0xFFFF;
					u32 addr = hi_orig << 16;
					u16 hi = hi_orig;
					//Calculate real hi using next lo
					for(u32 j=i+1; j<import->num_relocs; j++) {
						RelocEntry *new_reloc = &import->relocs[j];
						if(new_reloc->type == R_MIPS_LO16) {
							//Found lo
							u32 sym_ptr = (u32)GetSectionPtr(src_module, new_reloc->section, new_reloc->sym_ofs);
							u32 *lo_ptr = GetSectionPtr(module, cur_section, new_reloc->offset);
							u16 lo = *lo_ptr & 0xFFFF;
							//Calculate effective address with lo and symbol pointer
							addr += lo-((lo & 0x8000) << 1);
							addr -= sym_ptr;
							//Calculate hi from effective address
							hi = (addr >> 16)+((addr & 0x8000) >> 15);
							break;
						}
					}
					//Write hi
					*reloc_ptr = (*reloc_ptr & 0xFFFF0000)|hi;
				}
					break;
					
				case R_MIPS_LO16:
				{
					u32 sym_ptr = (u32)GetSectionPtr(src_module, reloc->section, reloc->sym_ofs);
					u16 lo = *reloc_ptr & 0xFFFF;
					lo -= sym_ptr;
					*reloc_ptr = (*reloc_ptr & 0xFFFF0000)|lo;
				}
					break;
					
				case R_ULTRA_SEC:
					//Flush cache of previous section
					FlushSection(module, cur_section);
					//Change section
					cur_section = reloc->section;
					break;
					
				default:
					debug_printf("Unknown relocation type %d.\n", reloc->type);
					break;
			}
		}
		//Need to flush cache of last section
		FlushSection(module, cur_section);
	}
}

static void UnlinkModule(ModuleHeader *module)
{
	u32 module_id = GetModuleID(module);
	//Do not unlink module 0
	if(module_id == 0) {
		return;
	}
	//Go through module list undoing import relocations
	for(u32 i=0; i<num_modules; i++) {
		//Do not undo this module's relocations
		if(i+1 != module_id) {
			//Get other module pointer
			ModuleHeader *module2 = module_handle_data[i].module;
			if(!module2) {
				continue;
			}
			//Undo relocations for every import module matching the ID
			for(u32 j=0; j<module2->num_import_modules; j++) {
				if(module2->import_modules[j].module_id == module_id) { 
					UndoModuleImportRelocs(module2, &module2->import_modules[j]);
					break;
				}
			}
		}
	}
}

static void RunDtors(ModuleHeader *module)
{
	if(module->dtor_section == SHN_UNDEF) {
		return;
	}
	ModuleFunc *start = module->section_info[module->dtor_section].ptr;
	ModuleFunc *end = start+(module->section_info[module->dtor_section].size/sizeof(ModuleFunc));
	//Run in reverse order starting from end destructor
	ModuleFunc *curr = end-1;
	while(curr >= start) {
		(*curr)();
		curr--;
	}
}

void ModuleUnloadForce(ModuleHandle *handle)
{
	debug_assert(handle && handle->module);
	//Run epilog
	if(handle->module->epilog) {
		handle->module->epilog();
	}
	RunDtors(handle->module);
	//Remove module from memory
	UnlinkModule(handle->module);
	free(handle->module);
	handle->ref_count = 0;
	handle->module = NULL;
}

void ModuleUnload(ModuleHandle *handle)
{
	debug_assert(handle);
	//Unload if reference count reaches zero
	if(handle->ref_count == 0 || --handle->ref_count == 0) {
		ModuleUnloadForce(handle);
	}
}

ModuleHandle *ModuleAddrToHandle(void *ptr)
{
	ModuleHandle *handle;
	u32 ptr_val = (u32)ptr;
	for(u32 i=0; i<num_modules; i++) {
		u32 top;
		u32 bottom;
		handle = &module_handle_data[i];
		top = (u32)handle->module;
		if(top != 0) {
			bottom = top+GetModuleRamSize(handle);
		} else {
			bottom = 0;
		}
		if(ptr_val >= top && ptr_val < bottom) {
			return handle;
		}
	}
	return NULL;
}