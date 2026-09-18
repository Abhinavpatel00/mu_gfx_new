APP_DEBUG := build/app_debug
APP_ASAN  := build/app_asan
TARGET    := build/app
BUILD_DIR := build

CC  := clang
CXX := clang++

# =========================================================
# Sources
# =========================================================

SRC_C := main.c vk.c renderer.c  ext.c src/platform.c src/nuklear.c src/input.c \
         external/mu/offset_allocator.c  \
         external/mu/mu.c

SRC_CPP := vma.cpp \
           $(wildcard external/meshoptimizer/src/*.cpp)

# =========================================================
# Optional Tracy Profiler
# Set USE_TRACY=1 (or TRACY=1) to enable Tracy profiling
# =========================================================
USE_TRACY ?= 0
TRACY ?= 0
TRACY_ENABLE ?= 0

ifneq ($(filter 1, $(USE_TRACY) $(TRACY) $(TRACY_ENABLE)),)
    ifneq ($(wildcard external/tracy/public/TracyClient.cpp),)
        SRC_CPP += external/tracy/public/TracyClient.cpp
        TRACY_FLAGS := -DTRACY_ENABLE
    else
        $(warning Tracy source external/tracy/public/TracyClient.cpp not found. Building without Tracy.)
        TRACY_FLAGS :=
    endif
else
    TRACY_FLAGS :=
endif

OBJ := $(addprefix $(BUILD_DIR)/, \
       $(SRC_C:.c=.o) \
       $(SRC_CPP:.cpp=.o))

# =========================================================
# Includes
# =========================================================

INCLUDES := -Iexternal/vulkan/include

# =========================================================
# Common Warnings
#
# Warnings are future crash spoilers.
# Humans ignore them anyway.
# =========================================================

WARNINGS := \
    -Wall \
    -Wextra \
    -Wshadow \
    -Wconversion \
    -Wstrict-aliasing=2 \
    -Wno-unused-parameter -Wno-sign-conversion  -Wno-unused-function
# =========================================================
# Base Flags
# =========================================================

BASE_CFLAGS := \
    $(INCLUDES) \
    $(WARNINGS)

BASE_CXXFLAGS := \
    -std=c++17 \
    -w \
    -fno-common \
    $(INCLUDES)

# =========================================================
# Debug Build
# =========================================================

DEBUG_FLAGS := \
    -O0 \
    -g \
    -ggdb \
    -fno-omit-frame-pointer \
    -fno-strict-aliasing \
    -DDEBUG \
    $(TRACY_FLAGS)

# =========================================================
# Address Sanitizer Build
#
# IMPORTANT:
# -O1 is ideal for ASAN.
# -O3 hides crimes.
# =========================================================

ASAN_FLAGS := \
    -O1 \
    -g \
    -ggdb \
    -fsanitize=address,undefined \
    -fno-omit-frame-pointer \
    -fno-optimize-sibling-calls \
    -fno-strict-aliasing \
    -DDEBUG \
    $(TRACY_FLAGS)

# =========================================================
# Release Build
# =========================================================

RELEASE_FLAGS := \
    -O3 \
    -march=native \
    -mtune=native \
    -fomit-frame-pointer \
    -fno-math-errno \
    -fno-trapping-math \
    -fno-semantic-interposition \
    -DNDEBUG \
    $(TRACY_FLAGS)

# =========================================================
# Libraries
# =========================================================

LIBS := \
    -lvulkan \
    -lm \
    -lX11 \
    -lXi \
    -lXrandr \
    -lXcursor \
    -lXinerama \
    -ldl \
    -lpthread

# =========================================================
# Default = Debug
# =========================================================

CFLAGS   := $(BASE_CFLAGS) $(DEBUG_FLAGS)
CXXFLAGS := $(BASE_CXXFLAGS) $(DEBUG_FLAGS)
LDFLAGS  :=

# =========================================================
# Build Targets
# =========================================================

all: $(TARGET)

$(TARGET): $(OBJ)
	@echo Linking $@
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

# =========================================================
# Compilation
# =========================================================

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo Compiling C $<
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo Compiling C++ $<
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# =========================================================
# Release
# =========================================================

release: CFLAGS   := $(BASE_CFLAGS) $(RELEASE_FLAGS)
release: CXXFLAGS := $(BASE_CXXFLAGS) $(RELEASE_FLAGS)
release: LDFLAGS  := -O3
release: $(TARGET)

# =========================================================
# ASAN BUILD
#
# This is the build you use to hunt memory corruption,
# use-after-free, OOB writes, UB, etc.
#
# Vulkan renderers without ASAN are basically:
# "trust me bro" engineering.
# =========================================================

asan: TARGET := $(APP_ASAN)

asan: CFLAGS := $(BASE_CFLAGS) $(ASAN_FLAGS)

asan: CXXFLAGS := $(BASE_CXXFLAGS) $(ASAN_FLAGS)

asan: LDFLAGS := \
    -fsanitize=address,undefined

asan: $(TARGET)

# =========================================================
# Run ASAN
#
# detect_leaks=0 because Vulkan/GLFW/drivers often
# intentionally leak process-lifetime allocations.
#
# Otherwise ASAN turns into:
# "everything is dying always"
# =========================================================

run_asan: asan
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=print_stacktrace=1 \
	./$(APP_ASAN)

# =========================================================
# Clean
# =========================================================

clean:
	@echo Cleaning...
	rm -rf $(BUILD_DIR) $(TARGET) $(APP_ASAN)

.PHONY: all clean release asan run_asan



TEST_RENDERER := $(BUILD_DIR)/renderer_test
TEST_INPUT := $(BUILD_DIR)/input_test

$(TEST_RENDERER): $(BUILD_DIR)/tests/renderer_test.o $(filter-out $(BUILD_DIR)/main.o,$(OBJ))
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

$(TEST_INPUT): $(BUILD_DIR)/tests/input_test.o $(BUILD_DIR)/src/input.o $(BUILD_DIR)/src/platform.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

SCENE_SHADERS := compiledshaders/scene3d.vert.spv compiledshaders/scene3d.frag.spv compiledshaders/scene3d.comp.spv
SLANGC ?= /opt/shader-slang-bin/bin/slangc

compiledshaders/scene3d.vert.spv: shaders/scene3d.slang src/scene3d_shared.h
	@mkdir -p $(@D)
	$(SLANGC) $< -target spirv -entry vs_main -stage vertex -O3 -o $@
compiledshaders/scene3d.frag.spv: shaders/scene3d.slang src/scene3d_shared.h
	@mkdir -p $(@D)
	$(SLANGC) $< -target spirv -entry fs_main -stage fragment -O3 -o $@
compiledshaders/scene3d.comp.spv: shaders/scene3d.slang src/scene3d_shared.h
	@mkdir -p $(@D)
	$(SLANGC) $< -target spirv -entry cs_main -stage compute -O3 -o $@

$(TARGET) $(TEST_RENDERER): | $(SCENE_SHADERS)

TEST_3D := $(BUILD_DIR)/renderer3d_test
$(TEST_3D): $(BUILD_DIR)/tests/renderer3d_test.o $(filter-out $(BUILD_DIR)/main.o $(BUILD_DIR)/renderer.o $(BUILD_DIR)/renderer3d.o,$(OBJ)) | $(SCENE_SHADERS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

test-3d: $(TEST_3D)
	$(TEST_3D)

test: $(TEST_INPUT) $(TEST_RENDERER) $(TEST_3D)
	$(TEST_INPUT)
	$(TEST_3D)
	$(TEST_RENDERER)

.PHONY: test test-3d
-include $(OBJ:.o=.d) $(BUILD_DIR)/tests/renderer_test.d $(BUILD_DIR)/tests/input_test.d $(BUILD_DIR)/tests/renderer3d_test.d
