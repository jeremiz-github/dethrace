BUILD_DIR=build
include $(N64_INST)/include/n64.mk

all: rdptest.z64

$(BUILD_DIR)/rdptest.elf: $(BUILD_DIR)/rdptest.o

rdptest.z64: N64_ROM_TITLE="RDP bilinear test"

clean:
	rm -rf $(BUILD_DIR) rdptest.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
