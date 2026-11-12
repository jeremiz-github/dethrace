BUILD_DIR=build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
N64_CFLAGS += $(EXTRA_CFLAGS)

all: t3dtest.z64

$(BUILD_DIR)/t3dtest.elf: $(BUILD_DIR)/t3dtest.o

t3dtest.z64: N64_ROM_TITLE="T3D vs rdpq test"

clean:
	rm -rf $(BUILD_DIR) t3dtest.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
