# ── Detect platform & Qt prefix ─────────────────────────────────────────────
UNAME := $(shell uname -s)
ARCH  := $(shell uname -m)

ifeq ($(UNAME),Darwin)
  # Apple Silicon uses /opt/homebrew; Intel uses /usr/local
  ifeq ($(ARCH),arm64)
    QT_PREFIX ?= /opt/homebrew
  else
    QT_PREFIX ?= /usr/local
  endif
else
  QT_PREFIX ?= /usr
endif

# ── Directories ──────────────────────────────────────────────────────────────
BUILD_DIR        ?= build
BUILD_DIR_STATIC ?= build-static
BUILD_DIR_REL    ?= build-release
BUILD_DIR_RELST  ?= build-release-static
BUILD_DIR_COV    ?= build-coverage
INSTALL_PREFIX   ?= $(PWD)/dist

# Line-coverage floor enforced by `make coverage`, per file.
COVERAGE_MIN     ?= 80

# ── CMake flags ──────────────────────────────────────────────────────────────
CMAKE_BASE_FLAGS := \
  -DCMAKE_PREFIX_PATH=$(QT_PREFIX) \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

CMAKE_DEBUG   := $(CMAKE_BASE_FLAGS) -DCMAKE_BUILD_TYPE=Debug
CMAKE_RELEASE := $(CMAKE_BASE_FLAGS) -DCMAKE_BUILD_TYPE=Release

JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

.PHONY: all build configure static release release-static \
        clean clean-static clean-release clean-release-static clean-all \
        install install-static \
        test coverage lint help

# ── Default ──────────────────────────────────────────────────────────────────
all: build

# ── Debug (dynamic) ──────────────────────────────────────────────────────────
configure:
	cmake -B $(BUILD_DIR) -S . $(CMAKE_DEBUG)

build: configure
	cmake --build $(BUILD_DIR) -- -j$(JOBS)

# ── Debug (static) ───────────────────────────────────────────────────────────
configure-static:
	cmake -B $(BUILD_DIR_STATIC) -S . $(CMAKE_DEBUG) -DSTATIC_BUILD=ON

static: configure-static
	cmake --build $(BUILD_DIR_STATIC) -- -j$(JOBS)

# ── Release (dynamic) ────────────────────────────────────────────────────────
configure-release:
	cmake -B $(BUILD_DIR_REL) -S . $(CMAKE_RELEASE)

release: configure-release
	cmake --build $(BUILD_DIR_REL) -- -j$(JOBS)

# ── Release (static) ─────────────────────────────────────────────────────────
configure-release-static:
	cmake -B $(BUILD_DIR_RELST) -S . $(CMAKE_RELEASE) -DSTATIC_BUILD=ON

release-static: configure-release-static
	cmake --build $(BUILD_DIR_RELST) -- -j$(JOBS)

# ── Install ───────────────────────────────────────────────────────────────────
install: release
	cmake --install $(BUILD_DIR_REL) --prefix $(INSTALL_PREFIX)

install-static: release-static
	cmake --install $(BUILD_DIR_RELST) --prefix $(INSTALL_PREFIX)

# ── Test ─────────────────────────────────────────────────────────────────────
test: configure
	cmake -B $(BUILD_DIR) -S . $(CMAKE_DEBUG) -DBUILD_TESTS=ON
	cmake --build $(BUILD_DIR) -- -j$(JOBS)
	cd $(BUILD_DIR) && ctest --output-on-failure

# ── Coverage ─────────────────────────────────────────────────────────────────
# gcov over the three Qt6::Core-only unit libraries. Its own build dir, because
# the instrumented objects are not the ones `make test` should be running.
coverage:
	BUILD_DIR=$(PWD)/$(BUILD_DIR_COV) QT_PREFIX=$(QT_PREFIX) \
	  COVERAGE_MIN=$(COVERAGE_MIN) bash tests/coverage.sh

# ── Lint / compile-commands symlink ──────────────────────────────────────────
lint: configure
	@if command -v clang-tidy >/dev/null 2>&1; then \
	  find src -name '*.cpp' | xargs clang-tidy -p $(BUILD_DIR); \
	else \
	  echo "clang-tidy not found — skipping"; \
	fi

# ── Clean ────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)

clean-static:
	rm -rf $(BUILD_DIR_STATIC)

clean-release:
	rm -rf $(BUILD_DIR_REL)

clean-release-static:
	rm -rf $(BUILD_DIR_RELST)

clean-all:
	rm -rf $(BUILD_DIR) $(BUILD_DIR_STATIC) $(BUILD_DIR_REL) $(BUILD_DIR_RELST) \
	       $(BUILD_DIR_COV) dist

# ── Help ─────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "anoa-browser build targets"
	@echo ""
	@echo "  make                        Debug build (dynamic)"
	@echo "  make build                  Debug build (dynamic)"
	@echo "  make static                 Debug build (static linking)"
	@echo "  make release                Release build (dynamic)"
	@echo "  make release-static         Release build (static linking)"
	@echo ""
	@echo "  make install                Install release build to INSTALL_PREFIX"
	@echo "  make install-static         Install static release build to INSTALL_PREFIX"
	@echo ""
	@echo "  make test                   Build and run tests"
	@echo "  make coverage               gcov the unit libraries, fail under COVERAGE_MIN%"
	@echo "  make lint                   Run clang-tidy (requires compile_commands.json)"
	@echo ""
	@echo "  make clean                  Remove debug build dir"
	@echo "  make clean-static           Remove static debug build dir"
	@echo "  make clean-release          Remove release build dir"
	@echo "  make clean-release-static   Remove static release build dir"
	@echo "  make clean-all              Remove all build dirs and dist/"
	@echo ""
	@echo "Variables (override on command line):"
	@echo "  QT_PREFIX=$(QT_PREFIX)"
	@echo "  INSTALL_PREFIX=$(INSTALL_PREFIX)"
	@echo "  JOBS=$(JOBS)"
	@echo ""
