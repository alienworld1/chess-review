PRESET ?= dev

.PHONY: help configure build run clean format format-check lint
.DEFAULT_GOAL := help

help:
	@echo "make run          Configure, build, and open the empty Qt window"
	@echo "make build        Configure and build with CMake + Ninja"
	@echo "make configure    Configure only"
	@echo "make clean        Remove compiled outputs for the selected preset"
	@echo "make format       Format C++ source"
	@echo "make format-check Check C++ formatting"
	@echo "make lint         Build and run clang-tidy"
	@echo "Use PRESET=release for an optimized build (default: dev)"

configure:
	cmake --preset $(PRESET)

build: configure
	cmake --build --preset $(PRESET)

run: build
	cmake --build --preset $(PRESET) --target run

clean:
	cmake --build --preset $(PRESET) --target clean

format:
	find src -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +

format-check:
	find src -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format --dry-run --Werror {} +

lint: build
	find src -type f -name '*.cpp' -exec clang-tidy -p build/$(PRESET) {} +
