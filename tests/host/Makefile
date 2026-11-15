# Host unit tests for the firmware's hardware-independent code.
#   make -C tests/host        build and run all tests
# Built with AddressSanitizer and UndefinedBehaviorSanitizer, so memory errors and
# undefined behaviour fail the tests too.
ROOT    := ../..
CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
INC     := -I. -I$(ROOT)/components/dsp/include -I$(ROOT)/components/battery/include \
           -I$(ROOT)/components/improv/include -I$(ROOT)/components/setup_web/include \
           -I$(ROOT)/components/gemini_live/include -Ivendor/cjson
BUILD   := build

TESTS := dsp battery improv web gemini
SRC_dsp     := $(ROOT)/components/dsp/dsp.c
SRC_battery := $(ROOT)/components/battery/battery_est.c
SRC_improv  := $(ROOT)/components/improv/improv_proto.c
SRC_web     := $(ROOT)/components/setup_web/web_util.c
SRC_gemini  := $(ROOT)/components/gemini_live/gemini_proto.c vendor/cjson/cJSON.c

.PHONY: test clean
test: $(TESTS:%=$(BUILD)/test_%)
	@set -e; for t in $^; do ./$$t; done; echo "all host tests passed"

# Rebuild when the code under test changes, not only the test file.
.SECONDEXPANSION:
$(BUILD)/test_%: test_%.c test.h $$(SRC_$$*) $$(wildcard $(ROOT)/components/*/include/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ $< $(SRC_$*) -lm

clean:
	rm -rf $(BUILD)
