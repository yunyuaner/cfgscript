# Makefile - build a chosen C source file or all sources in this folder
# Usage:
#   make build            # builds default FILE (cfgscript.c)
#   make FILE=expr_eval.c build   # build a specific source file
#   make FILE=all build   # build all .c files into .exe
#   make run FILE=cfgscript.c    # build then run the chosen exe

CC := gcc
CFLAGS := -g -O2 -Wall -I.
LDFLAGS := -mconsole

# Default file to build; override with `make FILE=some.c`
FILE ?= cfgscript_cli.c

SRC_DIR := .
SRCS := $(wildcard $(SRC_DIR)/*.c)

# Detect OS for proper clean command. Use POSIX `rm` in MSYS/MinGW.
ifeq ($(OS),Windows_NT)
# Prefer `rm -f` under MSYS/MinGW shells where `del` (cmd internal) isn't available.
RM := rm -f
else
RM := rm -f
endif

ifeq ($(FILE),all)
TARGETS := $(patsubst %.c,%.exe,$(notdir $(SRCS)))
else
TARGETS := $(patsubst %.c,%.exe,$(notdir $(FILE)))
endif

.PHONY: all build clean run list unit_test

all: build

build: $(TARGETS)

# Special rule for cfgscript_cli - needs to link with cfgscript.c
cfgscript_cli.exe: cfgscript_cli.c cfgscript.c cfgscript.h
	$(CC) $(CFLAGS) cfgscript_cli.c cfgscript.c -o $@ $(LDFLAGS)

# Unit test target
unit_test: unit_tests/test_cfgscript.exe

unit_tests/test_cfgscript.exe: unit_tests/test_cfgscript.c cfgscript.c cfgscript.h
	$(CC) $(CFLAGS) unit_tests/test_cfgscript.c cfgscript.c -o $@ $(LDFLAGS)

%.exe: %.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

run: build
	@if [ "$(FILE)" = "all" ]; then \
		echo "Built all targets; pick a single FILE to run."; \
	else \
		./$(basename $(notdir $(FILE))).exe; \
	fi

clean:
	-$(RM) *.o *.exe
	-$(RM) unit_tests/*.exe

list:
	@echo "Source files in $(SRC_DIR):"
	@for f in $(SRCS); do echo "  $$f"; done

