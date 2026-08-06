CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -Isrc -I. -I..
BUILD_DIR := build
BIN_DIR := bin
CORE := src/modbus_rtu.cpp
PACKAGE_DIR := dist/p2-stm-modbus-tester-pi

.PHONY: all test package clean

all: $(BIN_DIR)/p2-stm-modbus-test

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

$(BIN_DIR)/p2-stm-modbus-test: $(CORE) src/main.cpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/p2-stm-modbus-unit: $(CORE) tests/test_modbus.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: $(BUILD_DIR)/p2-stm-modbus-unit $(BIN_DIR)/p2-stm-modbus-test
	./$(BUILD_DIR)/p2-stm-modbus-unit
	./$(BIN_DIR)/p2-stm-modbus-test --self-test

package: test
	mkdir -p $(PACKAGE_DIR)/bin $(PACKAGE_DIR)/src $(PACKAGE_DIR)/tests
	cp $(BIN_DIR)/p2-stm-modbus-test $(PACKAGE_DIR)/bin/
	cp src/main.cpp src/modbus_rtu.cpp src/modbus_rtu.hpp $(PACKAGE_DIR)/src/
	cp tests/test_modbus.cpp $(PACKAGE_DIR)/tests/
	cp Makefile CMakeLists.txt README-ko.md $(PACKAGE_DIR)/
	cp ../stm_protocol.h $(PACKAGE_DIR)/
	cp ../README-ko.md $(PACKAGE_DIR)/DATA-SPEC-ko.md
	tar -czf dist/p2-stm-modbus-tester-pi.tar.gz -C dist p2-stm-modbus-tester-pi
	@echo "Created dist/p2-stm-modbus-tester-pi.tar.gz"

clean:
	rm -f $(BUILD_DIR)/p2-stm-modbus-unit $(BIN_DIR)/p2-stm-modbus-test
