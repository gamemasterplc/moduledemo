TARGET_STRING := moduledemo
TARGET := $(TARGET_STRING)

# Preprocessor definitions
DEFINES := _FINALROM=1 NDEBUG=1 F3DEX_GBI_2=1

SRC_DIRS :=
USE_DEBUG := 0

TOOLS_DIR := tools

# Whether to hide commands or not
VERBOSE ?= 0
ifeq ($(VERBOSE),0)
  V := @
endif

# Whether to colorize build messages
COLOR ?= 1

ifeq ($(filter clean distclean print-%,$(MAKECMDGOALS)),)
  # Make tools if out of date
  $(info Building tools...)
  DUMMY != $(MAKE) -s -C $(TOOLS_DIR) >&2 || echo FAIL
    ifeq ($(DUMMY),FAIL)
      $(error Failed to build tools)
    endif
  $(info Building ROM...)
endif

#==============================================================================#
# Target Executable and Sources                                                #
#==============================================================================#
# BUILD_DIR is the location where all build artifacts are placed
BUILD_DIR := build
MODULE_DIR := $(BUILD_DIR)/modules
TEMP_ROM := $(BUILD_DIR)/$(TARGET_STRING)_temp.z64
FINAL_ROM := $(TARGET_STRING).z64
MAIN_ELF := $(BUILD_DIR)/$(TARGET_STRING).elf
MODULES_DATA := $(BUILD_DIR)/modules.bin
LD_SCRIPT := $(TARGET_STRING).ld
BOOT := /usr/lib/n64/PR/bootcode/boot.6102
BOOT_OBJ := $(BUILD_DIR)/boot.6102.o

# Directories containing source files
SRC_DIRS += src asm

C_FILES := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
S_FILES := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.s))

SRC_OBJECTS := $(foreach file,$(C_FILES),$(BUILD_DIR)/$(file:.c=.o)) \
			$(foreach file,$(S_FILES),$(BUILD_DIR)/$(file:.s=.o))
			
# Object files
O_FILES := $(SRC_OBJECTS) \
		   $(BOOT_OBJ) 
		 
# Automatic dependency files
DEP_FILES := $(SRC_OBJECTS:.o=.d) $(BUILD_DIR)/$(LD_SCRIPT).d

#==============================================================================#
# Compiler Options                                                             #
#==============================================================================#

AS        := mips-n64-as
CC        := mips-n64-gcc
CPP       := cpp
LD        := mips-n64-ld
AR        := mips-n64-ar
OBJDUMP   := mips-n64-objdump
OBJCOPY   := mips-n64-objcopy

INCLUDE_DIRS += /usr/include/n64 /usr/include/n64/nusys include $(BUILD_DIR) src asm .

C_DEFINES := $(foreach d,$(DEFINES),-D$(d))
DEF_INC_CFLAGS := $(foreach i,$(INCLUDE_DIRS),-I$(i)) $(C_DEFINES)

CFLAGS = -Werror=implicit-function-declaration -fno-optimize-sibling-calls -G 0 -Os -mabi=32 -ffreestanding -mfix4300 $(DEF_INC_CFLAGS)
ASFLAGS     := -march=vr4300 -mabi=32 $(foreach i,$(INCLUDE_DIRS),-I$(i)) $(foreach d,$(DEFINES),--defsym $(d))

# C preprocessor flags
CPPFLAGS := -P -Wno-trigraphs $(DEF_INC_CFLAGS)

# tools
PRINT = printf

ifeq ($(COLOR),1)
NO_COL  := \033[0m
RED     := \033[0;31m
GREEN   := \033[0;32m
BLUE    := \033[0;34m
YELLOW  := \033[0;33m
BLINK   := \033[33;5m
endif

# Common build print status function
define print
  @$(PRINT) "$(GREEN)$(1) $(YELLOW)$(2)$(GREEN) -> $(BLUE)$(3)$(NO_COL)\n"
endef

#==============================================================================#
# Main Targets                                                                 #
#==============================================================================#

# Default target
default: $(FINAL_ROM)

#Must be below the default target for some reason
MODULE_OBJECTS := 
MODULE_SRCDIRS :=
MODULE_EXTERN_LIST := module_externs.ld

include modulefiles.mak

DEP_FILES += $(MODULE_OBJECTS:.o=.d) 

clean:
	$(RM) -r $(BUILD_DIR) $(FINAL_ROM)

distclean: clean
	$(MAKE) -C $(TOOLS_DIR) clean

ALL_DIRS := $(BUILD_DIR) $(MODULE_DIR) $(addprefix $(BUILD_DIR)/,$(SRC_DIRS)) $(addprefix $(BUILD_DIR)/,$(MODULE_SRCDIRS))

# Make sure build directory exists before compiling anything
DUMMY != mkdir -p $(ALL_DIRS)

#==============================================================================#
# Compilation Recipes                                                          #
#==============================================================================#

# Compile C code
$(BUILD_DIR)/%.o: %.c
	$(call print,Compiling:,$<,$@)
	$(V)$(CC) -c $(CFLAGS) -MMD -MF $(BUILD_DIR)/$*.d  -o $@ $<
	
$(BUILD_DIR)/%.o: $(BUILD_DIR)/%.c
	$(call print,Compiling:,$<,$@)
	$(V)$(CC) -c $(CFLAGS) -MMD -MF $(BUILD_DIR)/$*.d  -o $@ $<

# Assemble assembly code
$(BUILD_DIR)/%.o: %.s
	$(call print,Assembling:,$<,$@)
	$(V)$(AS) $(ASFLAGS) -MD $(BUILD_DIR)/$*.d  -o $@ $<

# Run linker script through the C preprocessor
$(BUILD_DIR)/$(LD_SCRIPT): $(LD_SCRIPT)
	$(call print,Preprocessing linker script:,$<,$@)
	$(V)$(CPP) $(CPPFLAGS) -DBUILD_DIR=$(BUILD_DIR) -MMD -MP -MT $@ -MF $@.d -o $@ $<

$(BOOT_OBJ): $(BOOT)
	$(V)$(OBJCOPY) -I binary -B mips -O elf32-bigmips $< $@

# Link final ELF file
$(MAIN_ELF): $(O_FILES) $(BUILD_DIR)/$(LD_SCRIPT) $(MODULE_EXTERN_LIST)
	@$(PRINT) "$(GREEN)Linking ELF file: $(BLUE)$@ $(NO_COL)\n"
	$(V)$(LD) -L $(BUILD_DIR) -T $(MODULE_EXTERN_LIST) -T $(BUILD_DIR)/$(LD_SCRIPT) -Map $(BUILD_DIR)/$(TARGET_STRING).map --no-check-sections -o $@ $(O_FILES) -L/usr/lib/n64 -lultra_rom -L$(N64_LIBGCCDIR) -lgcc

# Build ROM
$(TEMP_ROM): $(MAIN_ELF)
	$(call print,Building Temporary ROM:,$<,$@)
	$(V)$(OBJCOPY) $< $@ -O binary
	
$(FINAL_ROM): $(TEMP_ROM) $(MODULES_DATA)
	@$(PRINT) "$(GREEN)Creating final ROM: $(BLUE)$(FINAL_ROM) $(NO_COL)\n"
	$(V)cat $(TEMP_ROM) $(MODULES_DATA) > $(FINAL_ROM)
	$(V)makemask $(FINAL_ROM)

$(MODULES_ALL):
	@$(PRINT) "$(GREEN)Linking ELF file: $(BLUE)$@ $(NO_COL)\n"
	$(V)$(LD) -Map $@.map -d -r -T module.ld -o $@ $^
	
$(MODULES_DATA): $(MAIN_ELF) $(MODULES_ALL)
	@$(PRINT) "$(GREEN)Creating module data: $(BLUE)$@ $(NO_COL)\n"
	$(V)tools/makemodule $(MODULES_DATA) $(MAIN_ELF) $(MODULES_ALL)
	
.PHONY: clean distclean default
# with no prerequisites, .SECONDARY causes no intermediate target to be removed
.SECONDARY:

# Remove built-in rules, to improve performance
MAKEFLAGS += --no-builtin-rules

-include $(DEP_FILES)

print-% : ; $(info $* is a $(flavor $*) variable set to [$($*)]) @true