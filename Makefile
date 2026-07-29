# Convenience wrapper. CMake is the single source of truth for the build; this
# just saves typing. If you are packaging zhuzhbox, drive CMake directly.

BUILD_DIR ?= build
BUILD_TYPE ?= Release
PREFIX ?= /usr/local
CMAKE ?= cmake
CMAKE_FLAGS ?=

.PHONY: all configure build test test-sanitize install clean rules format help

all: build

configure:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX) -DZB_BUILD_TESTS=ON $(CMAKE_FLAGS)

build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure

# The pass the acceptance list asks for: everything under ASan and UBSan.
test-sanitize:
	$(CMAKE) -B $(BUILD_DIR)-asan -DCMAKE_BUILD_TYPE=Debug \
		-DZB_SANITIZE=ON -DZB_BUILD_TESTS=ON $(CMAKE_FLAGS)
	$(CMAKE) --build $(BUILD_DIR)-asan --parallel
	cd $(BUILD_DIR)-asan && ASAN_OPTIONS=detect_leaks=1 \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		ctest --output-on-failure

install: build
	$(CMAKE) --install $(BUILD_DIR)

# Regenerate the committed content-rules header from a website checkout.
rules:
	tools/gen_rules --source vercel --output src/rules_generated.h

format:
	clang-format -i $$(find src tests -name '*.c' -o -name '*.h')

clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR)-asan

help:
	@echo "make build           build (Release) into $(BUILD_DIR)"
	@echo "make test            build and run the test suite"
	@echo "make test-sanitize   run the suite under ASan + UBSan"
	@echo "make install         install to $(PREFIX)"
	@echo "make rules           regenerate src/rules_generated.h"
	@echo "make clean           remove the build directories"
