BUILD_DIR=build
T3D_INST?=/home/claude/hailtododongo/tiny3d
include $(N64_INST)/include/n64.mk
include $(T3D_INST)/t3d.mk
N64_CFLAGS += -std=gnu2x -O2

all: t3dtest.z64
$(BUILD_DIR)/t3dtest.elf: $(BUILD_DIR)/main.o
t3dtest.z64: N64_ROM_TITLE="T3D test"
clean:
	rm -rf $(BUILD_DIR) t3dtest.z64
-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
