TARGET_EXEC:=smw

SRCS:=$(wildcard smb1/*.c smbll/*.c src/*.c src/snes/*.c src/hd/*.c) third_party/gl_core/gl_core_3_1.c
OBJS:=$(SRCS:%.c=%.o)

PYTHON:=/usr/bin/env python3
CFLAGS:=$(if $(CFLAGS),$(CFLAGS),-O2 -fno-strict-aliasing -Werror )
CFLAGS:=${CFLAGS} $(shell sdl2-config --cflags) -DSYSTEM_VOLUME_MIXER_AVAILABLE=0 -I. -Isrc

ifeq (${OS},Windows_NT)
    WINDRES:=windres
#    RES:=sm.res
    SDLFLAGS:=-Wl,-Bstatic $(shell sdl2-config --static-libs)
else
    SDLFLAGS:=$(shell sdl2-config --libs) -lm
endif

.PHONY: all clean clean_obj

all: $(TARGET_EXEC) smw_assets.dat
$(TARGET_EXEC): $(OBJS) $(RES)
	$(CC) $^ -o $@ $(LDFLAGS) $(SDLFLAGS)

%.o : %.c
	$(CC) -c $(CFLAGS) $< -o $@

#$(RES): src/platform/win32/smw.rc
#	@echo "Generating Windows resources"
#	@$(WINDRES) $< -O coff -o $@

smw_assets.dat:
	@echo "Extracting game resources"
	$(PYTHON) assets/restool.py

# ---------------------------------------------------------------------------
# Unit tests — each tests/test_*.c compiles to its own binary.
# Tests link only the specific src/hd/*.c files they exercise, not the full
# game.  No SDL dependency.
# ---------------------------------------------------------------------------
TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=tests/%.elf)
TEST_CFLAGS := -O0 -g -Werror -I. -Isrc -Ithird_party/unity

tests/test_hd_vram_map.elf: tests/test_hd_vram_map.c \
                              src/hd/hd_vram_map.c \
                              third_party/unity/unity.c
	$(CC) $(TEST_CFLAGS) -o $@ $^

.PHONY: test
test: $(TEST_BINS)
	@failed=0; \
	for t in $(TEST_BINS); do \
	    echo "--- $$t ---"; \
	    ./$$t || failed=1; \
	done; \
	exit $$failed

clean: clean_obj clean_gen
clean_obj:
	@$(RM) $(OBJS) $(TARGET_EXEC)
clean_gen:
	@$(RM) $(RES) smw_assets.dat
	@rm -rf assets/__pycache__
