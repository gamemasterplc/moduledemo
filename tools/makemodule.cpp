#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <map>
#include "elfio/elfio.hpp"
#include "elfio/elf_types.hpp"

#define R_MIPS_32 2
#define R_MIPS_26 4
#define R_MIPS_HI16 5
#define R_MIPS_LO16 6
#define R_ULTRA_SEC 100

struct ELFFile {
    std::string name;
    std::string orig_path;
    ELFIO::elfio* reader;
};

struct RelocRecord {
    uint32_t offset;
    uint8_t type;
    uint16_t section;
    uint32_t sym_ofs;
};

struct SectionInfo {
    uint8_t* data;
    uint32_t align;
    uint32_t size;
};

struct ModuleData {
    uint32_t elf_id;
    std::string name;
    std::map<uint32_t, std::vector<RelocRecord>> imports;
    std::map<uint32_t, uint16_t> import_reloc_section;
    uint16_t ctor_section;
    uint16_t dtor_section;
    uint16_t prolog_section;
    uint16_t epilog_section;
    uint16_t unresolved_section;
    uint32_t prolog_addr;
    uint32_t epilog_addr;
    uint32_t unresolved_addr;
    uint32_t total_size;
};

struct SymbolSearchResult {
    uint16_t section;
    uint32_t module;
    uint32_t addr;
};

std::vector<ELFFile> elf_files;
std::vector<ModuleData> modules_data;

void DeleteELFReaders()
{
    //Delete all of the readers
    for (size_t i = 0; i < elf_files.size(); i++) {
        delete elf_files[i].reader;
    }
}

void TerminateProgram()
{
    DeleteELFReaders();
    exit(1);
}

ELFIO::section* FindELFSection(ELFIO::elfio* reader, std::string name)
{
    for (uint32_t i = 0; i < reader->sections.size(); i++) {
        if (reader->sections[i]->get_name() == name) {
            //Section name found
            return reader->sections[i];
        }
    }
    //Return NULL if no name match is found in ELF
    return NULL;
}

uint32_t FindELFSectionIndex(ELFIO::elfio* reader, std::string name)
{
    for (uint32_t i = 0; i < reader->sections.size(); i++) {
        if (reader->sections[i]->get_name() == name) {
            //Section name found
            return i;
        }
    }
    //Return number of sections
    return reader->sections.size();
}

void LoadELF(char* path, bool relocatable)
{
    ELFFile file;
    std::string temp = path;
    //Construct ELF name
    size_t slash_pos = temp.find_last_of("\\/") + 1;
    size_t dot_pos = temp.find_last_of(".");
    file.orig_path = path;
    file.name = temp.substr(slash_pos, dot_pos - slash_pos);
    file.reader = new ELFIO::elfio;
    if (!file.reader->load(path)) {
        std::cout << "Failed to read ELF file " << path << "." << std::endl;
        delete file.reader;
        TerminateProgram();
    }
    //Verify ELF basics
    if (file.reader->get_class() != ELFIO::ELFCLASS32
        || file.reader->get_encoding() != ELFIO::ELFDATA2MSB
        || file.reader->get_machine() != ELFIO::EM_MIPS
        || file.reader->get_version() != ELFIO::EV_CURRENT) {
        std::cout << "ELF " << path << " is not a valid N64 ELF." << std::endl;
        delete file.reader;
        TerminateProgram();
    }
    //Verify if ELF has correct relocation status
    if (relocatable) {
        if (file.reader->get_type() != ELFIO::ET_REL) {
            std::cout << "ELF " << path << " must be relocatable." << std::endl;
            delete file.reader;
            TerminateProgram();
        }
    }
    else {
        if (file.reader->get_type() != ELFIO::ET_EXEC) {
            std::cout << "ELF " << path << " must not be relocatable." << std::endl;
            delete file.reader;
            TerminateProgram();
        }
    }
    //Check if ELF has symbols
    if (!FindELFSection(file.reader, ".symtab")) {
        std::cout << "ELF " << path << " is stripped." << std::endl;
        delete file.reader;
        TerminateProgram();
    }
    elf_files.push_back(file);
}

bool SearchSymbolELF(std::string name, SymbolSearchResult* result, uint32_t elf_id)
{
    ELFIO::elfio* reader = elf_files[elf_id].reader;
    ELFIO::section* sym_section = FindELFSection(reader, ".symtab");
    ELFIO::symbol_section_accessor sym_accessor(*reader, sym_section);
    //Random variables needed for symbol getter to work
    ELFIO::Elf64_Addr addr;
    ELFIO::Elf_Xword size;
    unsigned char bind;
    unsigned char type;
    ELFIO::Elf_Half section_index;
    unsigned char other;
    if (!sym_accessor.get_symbol(name, addr, size, bind, type, section_index, other)) {
        return false;
    }
    //Only accept non-local defined symbols
    if (section_index == ELFIO::SHN_UNDEF || bind == ELFIO::STB_LOCAL) {
        return false;
    }
    //Write result
    result->addr = addr;
    result->section = section_index;
    result->module = elf_id;
    return true;
}

bool SearchSymbolGlobal(std::string name, SymbolSearchResult* result, uint32_t excluded_elf)
{
    //Look for symbol in all elf files but excluded_elf
    for (uint32_t i = 0; i < elf_files.size(); i++) {
        if (i != excluded_elf) {
            if (SearchSymbolELF(name, result, i)) {
                return true;
            }
        }
    }
    return false;
}

void InsertSectionChange(ModuleData* module, uint32_t module_id, uint16_t section)
{
    //Pre-initialize starting import relocation section if never initialized for module
    if (module->import_reloc_section.count(module_id) == 0) {
        module->import_reloc_section[module_id] = ELFIO::SHN_UNDEF;
    }
    //Insert section change if import relocation section has changed
    if (module->import_reloc_section[module_id] != section) {
        RelocRecord reloc_tmp;
        module->import_reloc_section[module_id] = section;
        reloc_tmp.offset = 0;
        reloc_tmp.section = section;
        reloc_tmp.type = R_ULTRA_SEC;
        reloc_tmp.sym_ofs = 0;
        module->imports[module_id].push_back(reloc_tmp);
    }
}

void GenerateImports(ModuleData* module)
{
    ELFIO::elfio* reader = elf_files[module->elf_id].reader;
    //Iterate through relocation sections
    for (ELFIO::Elf_Xword i = 0; i < reader->sections.size(); i++) {
        if (reader->sections[i]->get_type() == ELFIO::SHT_REL) {
            ELFIO::relocation_section_accessor reloc_accessor(*reader, reader->sections[i]);
            std::string target_section_name = reader->sections[i]->get_name().substr(4);
            uint32_t target_section_idx = FindELFSectionIndex(reader, target_section_name);
            if (target_section_idx == reader->sections.size()) {
                std::cout << "Could not find matching section name " << target_section_name << "in ELF." << std::endl;
                TerminateProgram();
            }
            for (ELFIO::Elf_Xword j = 0; j < reloc_accessor.get_entries_num(); j++) {
                //Read relocation
                ELFIO::Elf64_Addr offset;
                ELFIO::Elf_Word symbol;
                unsigned char type;
                ELFIO::Elf_Sxword addend;
                reloc_accessor.get_entry(j, offset, symbol, type, addend);
                {
                    //Read symbol
                    ELFIO::symbol_section_accessor sym_accessor(*reader, FindELFSection(reader, ".symtab"));
                    std::string sym_name;
                    ELFIO::Elf64_Addr sym_addr;
                    ELFIO::Elf_Xword sym_size;
                    unsigned char sym_bind;
                    unsigned char sym_type;
                    ELFIO::Elf_Half sym_section;
                    unsigned char other;
                    sym_accessor.get_symbol(symbol, sym_name, sym_addr, sym_size, sym_bind, sym_type, sym_section, other);
                    if (sym_section != ELFIO::SHN_UNDEF) {
                        //Symbol is defined internally
                        InsertSectionChange(module, module->elf_id, target_section_idx);
                        //Insert Relocation
                        RelocRecord reloc_tmp;
                        reloc_tmp.offset = offset;
                        reloc_tmp.section = sym_section;
                        reloc_tmp.type = type;
                        reloc_tmp.sym_ofs = sym_addr;
                        module->imports[module->elf_id].push_back(reloc_tmp);
                    }
                    else {
                        //Symbol only defined externally
                        SymbolSearchResult search_result;
                        if (!SearchSymbolGlobal(sym_name, &search_result, module->elf_id)) {
                            //Throw undefined reference error
                            std::cout << std::setbase(16);
                            std::cout << elf_files[module->elf_id].orig_path << ":(" << target_section_name << "+0x" << offset << "): ";
                            std::cout << "undefined reference to '" << sym_name << "'" << std::endl;
                            TerminateProgram();
                        }
                        InsertSectionChange(module, search_result.module, target_section_idx);
                        //Insert Relocation
                        RelocRecord reloc_tmp;
                        reloc_tmp.offset = offset;
                        reloc_tmp.section = search_result.section;
                        reloc_tmp.type = type;
                        reloc_tmp.sym_ofs = search_result.addr;
                        module->imports[search_result.module].push_back(reloc_tmp);
                    }
                }
            }
        }
    }
}

void ReadModule(uint32_t elf_id)
{
    ModuleData module;
    SymbolSearchResult sym_result;
    module.elf_id = elf_id;
    module.name = elf_files[elf_id].name;
    GenerateImports(&module);
    module.ctor_section = FindELFSectionIndex(elf_files[elf_id].reader, ".ctors");
    if (module.ctor_section == elf_files[elf_id].reader->sections.size()) {
        module.ctor_section = ELFIO::SHN_UNDEF;
    }
    module.dtor_section = FindELFSectionIndex(elf_files[elf_id].reader, ".dtors");
    if (module.dtor_section == elf_files[elf_id].reader->sections.size()) {
        module.dtor_section = ELFIO::SHN_UNDEF;
    }
    //Look for prolog symbol
    if (SearchSymbolELF("_prolog", &sym_result, elf_id)) {
        module.prolog_section = sym_result.section;
        module.prolog_addr = sym_result.addr;
    }
    else {
        //Symbol not found
        module.prolog_section = ELFIO::SHN_UNDEF;
        module.prolog_addr = 0;
    }
    //Look for epilog symbol
    if (SearchSymbolELF("_epilog", &sym_result, elf_id)) {
        module.epilog_section = sym_result.section;
        module.epilog_addr = sym_result.addr;
    } else {
        //Symbol not found
        module.epilog_section = ELFIO::SHN_UNDEF;
        module.epilog_addr = 0;
    }
    //Look for unresolved symbol
    if (SearchSymbolELF("_unresolved", &sym_result, elf_id)) {
        module.unresolved_section = sym_result.section;
        module.unresolved_addr = sym_result.addr;
    } else {
        //Symbol not found
        module.unresolved_section = ELFIO::SHN_UNDEF;
        module.unresolved_addr = 0;
    }
    //Add module
    modules_data.push_back(module);
}

std::filesystem::path GetModulePath(uint32_t module_id)
{
    std::filesystem::path path = std::filesystem::temp_directory_path();
    path += "/";
    path += modules_data[module_id].name;
    path += ".rel";
    return path;
}

void WriteU8(FILE* file, uint8_t value)
{
    //Forward value to fwrite
    fwrite(&value, 1, 1, file);
}

void WriteU16(FILE* file, uint16_t value)
{
    //Write in big-endian order
    uint8_t bytes[2];
    bytes[0] = value >> 8;
    bytes[1] = value & 0xFF;
    fwrite(&bytes, 1, 2, file);
}

void WriteU32(FILE* file, uint32_t value)
{
    //Write in big-endian order
    uint8_t bytes[4];
    bytes[0] = value >> 24;
    bytes[1] = (value >> 16) & 0xFF;
    bytes[2] = (value >> 8) & 0xFF;
    bytes[3] = value & 0xFF;
    fwrite(&bytes, 1, 4, file);
}

struct ModuleHeader {
    uint32_t num_sections;
    uint32_t section_info_ofs;
    uint32_t num_import_modules;
    uint32_t import_modules_ofs;
    uint16_t ctor_section;
    uint16_t dtor_section;
    uint16_t prolog_section;
    uint16_t epilog_section;
    uint16_t unresolved_section;
    uint32_t prolog_ofs;
    uint32_t epilog_ofs;
    uint32_t unresolved_ofs;
};

void WriteHeader(FILE* file, ModuleHeader* header)
{
    WriteU32(file, header->num_sections);
    WriteU32(file, header->section_info_ofs);
    WriteU32(file, header->num_import_modules);
    WriteU32(file, header->import_modules_ofs);
    WriteU16(file, header->ctor_section);
    WriteU16(file, header->dtor_section);
    WriteU16(file, header->prolog_section);
    WriteU16(file, header->epilog_section);
    WriteU16(file, header->unresolved_section);
    WriteU16(file, 0);
    WriteU32(file, header->prolog_ofs);
    WriteU32(file, header->epilog_ofs);
    WriteU32(file, header->unresolved_ofs);
}

uint32_t AlignU32(uint32_t val, uint32_t to)
{
    //Only supports power of 2 alignment
    return (val + to - 1) & ~(to - 1);
}

void AlignFile(FILE* file, uint32_t to)
{
    //Only supports power of 2 alignment
    long pos = ftell(file);
    while (pos & (to - 1)) {
        WriteU8(file, 0);
        pos++;
    }
}

void WriteModuleTemp(uint32_t module_id)
{
    ELFIO::elfio* reader = elf_files[modules_data[module_id].elf_id].reader;
    FILE* file = fopen(GetModulePath(module_id).string().c_str(), "wb");
    ModuleHeader header;

    //Write initial header
    header.num_sections = reader->sections.size();
    header.section_info_ofs = sizeof(ModuleHeader);
    header.num_import_modules = modules_data[module_id].imports.size();
    header.import_modules_ofs = 0; //Will be recalculated later
    header.ctor_section = modules_data[module_id].ctor_section;
    header.dtor_section = modules_data[module_id].dtor_section;
    header.prolog_section = modules_data[module_id].prolog_section;
    header.prolog_ofs = modules_data[module_id].prolog_addr;
    header.epilog_section = modules_data[module_id].epilog_section;
    header.epilog_ofs = modules_data[module_id].epilog_addr;
    header.unresolved_section = modules_data[module_id].unresolved_section;
    header.unresolved_ofs = modules_data[module_id].unresolved_addr;
    WriteHeader(file, &header);

    //Write section headers
    uint32_t data_ofs = header.section_info_ofs + (12 * reader->sections.size());
    for (uint32_t i = 0; i < reader->sections.size(); i++) {
        ELFIO::Elf_Word type = reader->sections[i]->get_type();
        if (type == ELFIO::SHT_PROGBITS) {
            //Stored section header
            uint32_t align = reader->sections[i]->get_addr_align();
            data_ofs = AlignU32(data_ofs, align);
            WriteU32(file, data_ofs);
            WriteU32(file, align);
            WriteU32(file, reader->sections[i]->get_size());
            data_ofs += reader->sections[i]->get_size();
        }
        else if (type == ELFIO::SHT_NOBITS) {
            //BSS section header
            uint32_t align = reader->sections[i]->get_addr_align();
            WriteU32(file, 0);
            WriteU32(file, align);
            WriteU32(file, reader->sections[i]->get_size());
        }
        else {
            //NULL section header
            WriteU32(file, 0);
            WriteU32(file, 0);
            WriteU32(file, 0);
        }
    }
    //Write all SHT_PROGBITS sections to file
    for (uint32_t i = 0; i < reader->sections.size(); i++) {
        if (reader->sections[i]->get_type() == ELFIO::SHT_PROGBITS) {
            uint32_t align = reader->sections[i]->get_addr_align();
            AlignFile(file, align);
            fwrite(reader->sections[i]->get_data(), 1, reader->sections[i]->get_size(), file);
        }
    }
    //Align to 4 bytes for relocation data
    AlignFile(file, 4);
    header.import_modules_ofs = AlignU32(data_ofs, 4);
    //Write import relocation lists
    uint32_t reloc_ofs = header.import_modules_ofs + (12 * header.num_import_modules);
    std::map<uint32_t, std::vector<RelocRecord>>::iterator iter;
    for (iter = modules_data[module_id].imports.begin(); iter != modules_data[module_id].imports.end(); ++iter) {
        WriteU32(file, iter->first);
        WriteU32(file, iter->second.size());
        WriteU32(file, reloc_ofs);
        reloc_ofs += iter->second.size() * 12;
    }
    //Write import relocations
    for (iter = modules_data[module_id].imports.begin(); iter != modules_data[module_id].imports.end(); ++iter) {
        for (uint32_t i = 0; i < iter->second.size(); i++) {
            WriteU32(file, iter->second[i].offset);
            WriteU8(file, iter->second[i].type);
            WriteU8(file, 0);
            WriteU16(file, iter->second[i].section);
            WriteU32(file, iter->second[i].sym_ofs);
        }
    }
    //Rewrite header
    modules_data[module_id].total_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    WriteHeader(file, &header);
    fclose(file);
}

void DeleteTempModules()
{
    for (uint32_t i = 0; i < modules_data.size(); i++) {
        std::filesystem::remove(GetModulePath(i));
    }
}

uint32_t GetStringTableSize()
{
    uint32_t size = 0;
    //Accumulate all string lengths
    for (uint32_t i = 0; i < modules_data.size(); i++) {
        size += modules_data[i].name.length() + 1;
    }
    //Align it to 2 bytes
    return AlignU32(size, 2);
}

uint32_t GetModuleAlign(uint32_t module_id)
{
    ELFIO::elfio* reader = elf_files[modules_data[module_id].elf_id].reader;
    uint32_t alignment = 4; //Minimum module alignment is 4
    //Calculate maximum alignment of SHT_PROGBITS sections
    for (uint32_t i = 0; i < reader->sections.size(); i++) {
        if (reader->sections[i]->get_type() == ELFIO::SHT_PROGBITS) {
            if (reader->sections[i]->get_addr_align() > alignment) {
                alignment = reader->sections[i]->get_addr_align();
            }
        }
    }
    return alignment;
}

uint32_t GetNoloadAlign(uint32_t module_id)
{
    ELFIO::elfio* reader = elf_files[modules_data[module_id].elf_id].reader;
    uint32_t alignment = 1;
    //Calculate maximum alignment of SHT_NOBITS sections
    for (uint32_t i = 0; i < reader->sections.size(); i++) {
        if (reader->sections[i]->get_type() == ELFIO::SHT_NOBITS) {
            if (reader->sections[i]->get_addr_align() > alignment) {
                alignment = reader->sections[i]->get_addr_align();
            }
        }
    }
    return alignment;
}

uint32_t GetNoloadSize(uint32_t module_id)
{
    ELFIO::elfio* reader = elf_files[modules_data[module_id].elf_id].reader;
    uint32_t size = 0;
    for (uint32_t i = 0; i < reader->sections.size(); i++) {
        if (reader->sections[i]->get_type() == ELFIO::SHT_NOBITS) {
            //Align size to start at section alignment
            size = AlignU32(size, reader->sections[i]->get_addr_align());
            //Add size to section alignment
            size += reader->sections[i]->get_size();
        }
    }
    return size;
}

void WriteOutput(std::string name)
{
    //Open output file
    FILE* file = fopen(name.c_str(), "wb");
    if (!file) {
        std::cout << "Failed to open file " << name << " for writing" << std::endl;
        DeleteTempModules();
        DeleteELFReaders();
        TerminateProgram();
    }
    //Write header of output
    WriteU32(file, modules_data.size());
    WriteU32(file, GetStringTableSize());
    //Write module information
    uint32_t string_ofs = 32 * modules_data.size();
    uint32_t data_ofs = string_ofs + GetStringTableSize();
    for (uint32_t i = 0; i < modules_data.size(); i++) {
        WriteU32(file, string_ofs);
        WriteU32(file, GetModuleAlign(i));
        WriteU32(file, modules_data[i].total_size);
        WriteU32(file, data_ofs);
        WriteU32(file, GetNoloadAlign(i));
        WriteU32(file, GetNoloadSize(i));
        WriteU32(file, 0);
        WriteU32(file, 0);
        data_ofs += modules_data[i].total_size;
        string_ofs += modules_data[i].name.length() + 1;
    }
    //Write strings
    for (uint32_t i = 0; i < modules_data.size(); i++) {
        fwrite(modules_data[i].name.c_str(), 1, modules_data[i].name.length() + 1, file);
    }
    //Align to 2 bytes for ROM
    AlignFile(file, 2);
    //Write module files
    for (uint32_t i = 0; i < modules_data.size(); i++) {
        FILE* file2 = fopen(GetModulePath(i).string().c_str(), "rb");
        uint8_t* temp_buf = new uint8_t[modules_data[i].total_size];
        fread(temp_buf, 1, modules_data[i].total_size, file2);
        fwrite(temp_buf, 1, modules_data[i].total_size, file);
        delete[] temp_buf;
        fclose(file2);
    }
    fclose(file);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " out_file input_files" << std::endl;
        std::cout << "First input file must be non-relocatable and have symbols." << std::endl;
        std::cout << "Other input files must be relocatable." << std::endl;
        return 1;
    }
    LoadELF(argv[2], false);
    for (int i = 0; i < argc - 3; i++) {
        LoadELF(argv[i + 3], true);
    }
    for (uint32_t i = 1; i < elf_files.size(); i++) {
        ReadModule(i);
    }
    for (uint32_t i = 0; i < modules_data.size(); i++) {
        WriteModuleTemp(i);
    }
    WriteOutput(argv[1]);
    DeleteTempModules();
    DeleteELFReaders();
    return 0;
}