# Convenience commands for building DeathStrandingDCFix on Linux.
# The build itself is defined in CMakeLists.txt and cmake/; this file only drives cmake/cpack.
# See wiki/Building-on-Arch-Linux.md for setup instructions.

NAME          := DeathStrandingDCFix
MODE          ?= release
VERSION       ?= dev
GAME_DIR      ?=
MINGW_TRIPLET ?= x86_64-w64-mingw32
GENERATOR     ?= $(if $(shell command -v ninja 2>/dev/null),Ninja,Unix Makefiles)

ifeq ($(MODE),release)
  BUILD_TYPE := MinSizeRel
else ifeq ($(MODE),debug)
  BUILD_TYPE := Debug
else
  $(error MODE must be 'release' or 'debug' (got '$(MODE)'))
endif

BUILD_DIR    := build/mingw/$(MODE)
OUT_DIR      := $(BUILD_DIR)/bin
TOOLCHAIN    := cmake/toolchains/mingw-w64-x86_64.cmake
LOADER_CACHE := build/asi-loader/winhttp.dll
DIST_ZIP     := build/$(NAME)_$(VERSION).zip

.PHONY: all build release debug configure submodules check-game-dir install dist update-loader compdb clean distclean help

all: build

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)
	@echo "Release folder: $(OUT_DIR)/"

release:
	@$(MAKE) --no-print-directory MODE=release

debug:
	@$(MAKE) --no-print-directory MODE=debug

$(BUILD_DIR)/CMakeCache.txt:
	@$(MAKE) --no-print-directory configure

configure:
	cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" \
		--toolchain $(TOOLCHAIN) \
		-DMINGW_TRIPLET=$(MINGW_TRIPLET) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

submodules:
	git submodule update --init --recursive

check-game-dir:
	@test -n "$(GAME_DIR)" || { \
		echo 'error: set GAME_DIR, e.g. make install GAME_DIR="$$HOME/.local/share/Steam/steamapps/common/DEATH STRANDING DIRECTORS CUT"'; exit 1; }
	@test -d "$(GAME_DIR)" || { echo "error: GAME_DIR '$(GAME_DIR)' does not exist"; exit 1; }

install: check-game-dir build
	cmake --install $(BUILD_DIR) --component game --prefix "$(GAME_DIR)"

dist: build
	cpack --config $(BUILD_DIR)/CPackConfig.cmake -B $(BUILD_DIR)/package \
		-D CPACK_PACKAGE_FILE_NAME=$(NAME)_$(VERSION)
	cp $(BUILD_DIR)/package/$(NAME)_$(VERSION).zip $(DIST_ZIP)
	@echo "Created $(DIST_ZIP)"

update-loader:
	rm -f $(LOADER_CACHE)
	@$(MAKE) --no-print-directory configure build

compdb: $(BUILD_DIR)/CMakeCache.txt
	ln -sf $(BUILD_DIR)/compile_commands.json compile_commands.json

clean:
	rm -rf build/mingw

distclean: clean
	rm -rf build/asi-loader build/$(NAME)_*.zip

help:
	@echo "Targets:"
	@echo "  all (default)  Configure (first time) and build $(OUT_DIR)/"
	@echo "                 with $(NAME).asi, $(NAME).ini and winhttp.dll"
	@echo "  release|debug  Build with MODE=release or MODE=debug"
	@echo "  configure      Re-run CMake configure for MODE"
	@echo "  submodules     Fetch git submodules (spdlog, inipp)"
	@echo "  install        Install the .asi and winhttp.dll (and .ini if absent) into GAME_DIR"
	@echo "  dist           Create $(DIST_ZIP)"
	@echo "  update-loader  Re-download the latest Ultimate ASI Loader"
	@echo "  compdb         Symlink compile_commands.json into the repo root (clangd)"
	@echo "  clean          Remove build/mingw (keeps the downloaded loader)"
	@echo "  distclean      clean + remove the downloaded loader and release zips"
	@echo ""
	@echo "Variables:"
	@echo "  MODE=$(MODE)  VERSION=$(VERSION)  MINGW_TRIPLET=$(MINGW_TRIPLET)"
	@echo "  GENERATOR=$(GENERATOR)"
	@echo "  GAME_DIR=<path to the game folder>"
