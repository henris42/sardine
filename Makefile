CXX       = /opt/gcc-16.1/bin/g++
CXXFLAGS  = -std=c++26 -freflection -Wall -Wextra -Iinclude
LDFLAGS   = -Wl,-rpath,/opt/gcc-16.1/lib64

BUILD    := build

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_sardine: tests/test_sardine.cpp include/sardine/sardine.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/test_sardine.cpp -o $@ $(LDFLAGS)

test: $(BUILD)/test_sardine
	./$(BUILD)/test_sardine

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
