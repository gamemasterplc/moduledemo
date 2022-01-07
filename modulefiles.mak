
MODULE_OBJECTS += $(BUILD_DIR)/module_src/module1.o $(BUILD_DIR)/module_src/module2.o
MODULE_SRCDIRS +=  module_src

$(MODULE_DIR)/module1.elf: $(BUILD_DIR)/module_src/module1.o
$(MODULE_DIR)/module2.elf: $(BUILD_DIR)/module_src/module2.o

MODULE_NAMES := module1 module2

MODULES_ALL := $(foreach name,$(MODULE_NAMES),$(MODULE_DIR)/$(name).elf)