BUILD_DIR ?= build
JOBS ?= 4
CMAKE_ARGS ?=

.PHONY: build test clean

build:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)
	cmake --build $(BUILD_DIR) --parallel $(JOBS)
	install -m 755 tools/nr-binder $(BUILD_DIR)/nr-binder

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	cmake --build $(BUILD_DIR) --target clean
