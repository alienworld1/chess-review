PRESET ?= dev

.PHONY: help configure build run clean test format format-check lint package-linux package-verify
.DEFAULT_GOAL := help

help:
	@echo "make run            Configure, build, and open the app"
	@echo "make build          Configure and build with CMake + Ninja"
	@echo "make configure      Configure only"
	@echo "make clean          Remove compiled outputs for the selected preset"
	@echo "make format         Format C++ source"
	@echo "make format-check   Check C++ formatting"
	@echo "make lint           Build and run clang-tidy"
	@echo "make test           Build and run CTest"
	@echo "make package-linux  Build Linux x86_64 AppImage in dist/"
	@echo "make package-verify Verify Linux x86_64 AppImage in clean environment"
	@echo "Use PRESET=release or PRESET=sanitized (default: dev)"

configure:
	cmake --preset $(PRESET)

build: configure
	cmake --build --preset $(PRESET)

run: build
	cmake --build --preset $(PRESET) --target run

clean:
	cmake --build --preset $(PRESET) --target clean

test: build
	ctest --preset $(PRESET)

format:
	find src tests -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +

format-check:
	find src tests -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format --dry-run --Werror {} +

lint: build
	find src tests -type f -name '*.cpp' -exec clang-tidy -p build/$(PRESET) {} +

package-linux:
	packaging/linux/build-appimage.sh

package-verify:
	packaging/linux/verify-appimage.sh
