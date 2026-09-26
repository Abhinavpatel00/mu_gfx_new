APP_DEBUG := build/app_debug
APP_ASAN  := build/app_asan
TARGET    := build/app
BUILD_DIR := build

.DEFAULT_GOAL := all

CC  := clang
CXX := clang++

# =========================================================
# GLFW (vendored, Wayland-only)
# =========================================================
# GLFW is a CMake project, so it is built once into $(GLFW_BUILD_DIR) and the
# resulting static library is linked into the app. RGFW made the app generate
# its own Wayland protocol bindings (wayland.mk); GLFW ships those XML files
# under deps/wayland and compiles its own bindings, so generating them here as
# well would define the protocol symbols twice.
#
# Switching between debug/release/asan reconfigures the same binary directory,
# so run `make clean` after switching build type.
GLFW_DIR        := external/glfw
GLFW_BUILD_DIR  := $(BUILD_DIR)/glfw
GLFW_LIB        := $(GLFW_BUILD_DIR)/src/libglfw3.a
GLFW_BUILD_TYPE ?= Debug

GLFW_CMAKE_ARGS := \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=$(GLFW_BUILD_TYPE) \
    -DGLFW_BUILD_X11=OFF \
    -DGLFW_BUILD_WAYLAND=ON \
    -DGLFW_BUILD_EXAMPLES=OFF \
    -DGLFW_BUILD_TESTS=OFF \
    -DGLFW_BUILD_DOCS=OFF \
    -DGLFW_INSTALL=OFF

# =========================================================
# Sources
# =========================================================
SRC_C := main.c vk.c ext.c renderer.c src/nuklear.c src/input.c \
         external/mu/offset_allocator.c \
         external/mu/mu.c \
         src/two_d/sprite.c \
         src/two_d/picture.c \
         src/three_d/scene3d.c \
         src/three_d/scene3d_asset.c \


SRC_CPP := vma.cpp

# =========================================================
# Optional Tracy Profiler
# =========================================================
USE_TRACY     ?= 0
TRACY         ?= 0
TRACY_ENABLE  ?= 0

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

# Objects for the C and C++ sources; GLFW is linked as a prebuilt static library.
OBJ := $(addprefix $(BUILD_DIR)/, $(SRC_C:.c=.o)) \
       $(addprefix $(BUILD_DIR)/, $(SRC_CPP:.cpp=.o))

# =========================================================
# Includes
# =========================================================
INCLUDES := -Iexternal/vulkan/include \
            -Iexternal/glfw/include \
            -Iexternal/tree-sitter/lib/include \
            -Iexternal/tree-sitter-c/bindings/c \
            -Iexternal/tree-sitter-c/src

# =========================================================
# Warnings
# =========================================================
WARNINGS := \
    -Wall -Wextra -Wshadow -Wconversion -Wstrict-aliasing=2 \
    -Wno-unused-parameter -Wno-sign-conversion -Wno-unused-function

# =========================================================
# Base flags
# =========================================================
BASE_CFLAGS := \
    -std=gnu99 \
    $(INCLUDES) \
  #  $(WARNINGS)

BASE_CXXFLAGS := \
    -std=c++17 \
    -w \
    -fno-common \
    $(INCLUDES)

# =========================================================
# Debug / ASAN / Release flags
# =========================================================
DEBUG_FLAGS := \
    -O0 -g -ggdb -fno-omit-frame-pointer -fno-strict-aliasing \
    -DDEBUG $(TRACY_FLAGS) \
#    -DEMBED_SHADERS


ASAN_FLAGS := \
    -O1 -g -ggdb -fsanitize=address,undefined \
    -fno-omit-frame-pointer -fno-optimize-sibling-calls \
    -fno-strict-aliasing -DDEBUG $(TRACY_FLAGS)

# FIX: backslash after -DEMBED_SHADERS so $(TRACY_FLAGS) is actually appended.
RELEASE_FLAGS := \
    -O3 -march=native -mtune=native -fomit-frame-pointer \
    -fno-math-errno -fno-trapping-math -fno-semantic-interposition \
    -DNDEBUG -DEMBED_SHADERS \
    $(TRACY_FLAGS)

# =========================================================
# Libraries
# =========================================================
# The Wayland backend of the vendored GLFW uses these at link time.
LIBS := \
    -lwayland-client \
    -lwayland-cursor \
    -lwayland-egl \
    -lxkbcommon \
    -ldl -lpthread -lm

# =========================================================
# Default = Debug
# =========================================================
CFLAGS   := $(BASE_CFLAGS) $(DEBUG_FLAGS)
CXXFLAGS := $(BASE_CXXFLAGS) $(DEBUG_FLAGS)
LDFLAGS  :=

# =========================================================
# Targets
# =========================================================
all: $(TARGET)

# FIX: link with $(CC), not $(CXX). clang++ unconditionally appends
#      -lstdc++, which reintroduces libstdc++.so.6 into NEEDED.
$(TARGET): $(OBJ) $(GLFW_LIB)
	@echo Linking $@
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

# =========================================================
# GLFW
# =========================================================
$(GLFW_LIB):
	@echo Configuring GLFW
	cmake -S $(GLFW_DIR) -B $(GLFW_BUILD_DIR) $(GLFW_CMAKE_ARGS)
	@echo Building GLFW
	cmake --build $(GLFW_BUILD_DIR) --parallel

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

# Upstream code, not ours — silence it.
$(BUILD_DIR)/external/tree-sitter/lib/src/lib.o: CFLAGS += -w
$(BUILD_DIR)/external/tree-sitter-c/src/parser.o: CFLAGS += -w

-include $(OBJ:.o=.d)

# =========================================================
# Release
# =========================================================
release: CFLAGS   := $(BASE_CFLAGS) $(RELEASE_FLAGS)
release: CXXFLAGS := $(BASE_CXXFLAGS) $(RELEASE_FLAGS)
release: LDFLAGS  := -O3
release: GLFW_BUILD_TYPE := Release
release: $(TARGET)

# =========================================================
# ASAN
# =========================================================
asan: TARGET := $(APP_ASAN)
asan: CFLAGS   := $(BASE_CFLAGS) $(ASAN_FLAGS)
asan: CXXFLAGS := $(BASE_CXXFLAGS) $(ASAN_FLAGS)
asan: LDFLAGS  := -fsanitize=address,undefined
asan: $(TARGET)

run_asan: asan
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=print_stacktrace=1 \
	./$(APP_ASAN)

# =========================================================
# Clean
# =========================================================
# $(BUILD_DIR) holds the GLFW build tree as well, so this removes it too.
clean:
	@echo Cleaning...
	rm -rf $(BUILD_DIR) $(TARGET) $(APP_ASAN)

.PHONY: all clean release asan run_asan

