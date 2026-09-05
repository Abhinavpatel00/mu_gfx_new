APP_DEBUG := build/app_debug
APP_ASAN  := build/app_asan
TARGET    := build/app
BUILD_DIR := build

CC  := clang
CXX := clang++

# =========================================================
# Sources
# =========================================================

SRC_C := main.c  ext.c  \
         external/mu/offset_allocator.c  \
         external/mu/mu.c

SRC_CPP := vma.cpp \
           $(wildcard external/meshoptimizer/src/*.cpp) \
           external/cimgui/cimgui.cpp \
           external/cimgui/cimgui_impl.cpp \
           external/cimgui/imgui/imgui.cpp \
           external/cimgui/imgui/imgui_draw.cpp \
           external/cimgui/imgui/imgui_demo.cpp \
           external/cimgui/imgui/imgui_tables.cpp \
           external/cimgui/imgui/imgui_widgets.cpp \
           external/cimgui/imgui/backends/imgui_impl_glfw.cpp \
           external/cimgui/imgui/backends/imgui_impl_vulkan.cpp \
           external/tracy/public/TracyClient.cpp

OBJ := $(addprefix $(BUILD_DIR)/, \
       $(SRC_C:.c=.o) \
       $(SRC_CPP:.cpp=.o))

# =========================================================
# Includes
# =========================================================

INCLUDES := \
    -Iexternal/cimgui \
    -Iexternal/cimgui/imgui \
    -Iexternal/cimgui/imgui/backends

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
    -Wno-unused-parameter

# =========================================================
# Base Flags
# =========================================================

BASE_CFLAGS := \
    -std=gnu99 \
    $(WARNINGS)

BASE_CXXFLAGS := \
    -std=c++17 \
    -w \
    -fno-common \
    $(INCLUDES) \
    -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES \
    -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS \
    -DIMGUI_IMPL_API='extern "C"'

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
    -DTRACY_ENABLE

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
    -DTRACY_ENABLE

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
    -DTRACY_ENABLE

# =========================================================
# Libraries
# =========================================================

LIBS := \
    -lvulkan \
    -lm \
    -lglfw \
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
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo Compiling C++ $<
	$(CXX) $(CXXFLAGS) -c $< -o $@

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


