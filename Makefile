# Flint is written in Flint (compiler/). Building it needs a Flint compiler to start from, stage 0:
#   - FLINT_BOOTSTRAP=path/to/flint, if given (for example a downloaded release:
#     scripts/bootstrap.sh fetches the one pinned in bootstrap/VERSION)
#   - otherwise build/flint-c, the original compiler written in C (src/)
# Stage 0 builds stage 1 (build/stage1), which builds the compiler that's installed: build/flint.
# `make selfhost-check` builds it once more and checks the result is the same (a fixpoint).

CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
CSRC = src/util.c src/lex.c src/parse.c src/module.c src/manifest.c src/cimport.c src/lower.c src/interp.c src/ir.c src/opt.c src/arm64.c src/main.c
STD = $(wildcard std/*.fl)
FLSRC = $(shell find compiler -name '*.fl') $(STD)
MODE ?= ReleaseSafe
PREFIX ?= /usr/local
BINDIR = $(DESTDIR)$(PREFIX)/bin
VERSION = $(shell cat VERSION)

ifeq ($(FLINT_BOOTSTRAP),)
STAGE0 = build/flint-c
else
STAGE0 = $(FLINT_BOOTSTRAP)
endif

all: build/flint

# the compiler written in C
build/flint-c: $(CSRC) src/flint.h src/fmt_float.h build/std_fl.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibuild $(CSRC) -o $@

# the standard library embedded in the C compiler (the Flint compiler embeds it with @embedFile)
build/std_fl.h: $(STD) Makefile
	@mkdir -p build
	@(cd std && for f in *.fl; do xxd -i $$f; done; \
	  echo 'static const struct { const char *name; const unsigned char *data; unsigned len; } STD_FILES[] = {'; \
	  for f in *.fl; do v=$$(echo $$f | tr . _); echo "  {\"$$f\", $$v, sizeof $$v},"; done; \
	  echo '  {0, 0, 0}};') > $@

# every std file must be in the list compiler/modules/load.fl embeds
check-std-list: $(STD)
	@for f in $(notdir $(STD)); do grep -q "\"$$f\"" compiler/modules/load.fl || \
	  { echo "std/$$f is missing from STD_FILES in compiler/modules/load.fl"; exit 1; }; done

build/stage1: $(FLSRC) $(if $(FLINT_BOOTSTRAP),,build/flint-c) | check-std-list
	@mkdir -p build
	$(STAGE0) build -O $(MODE) compiler/main.fl -o $@

build/flint: build/stage1 $(FLSRC)
	build/stage1 build -O $(MODE) compiler/main.fl -o $@

# the compiler builds itself to the same assembly (and C and Flint agree on every test program)
selfhost-check: build/flint
	build/flint build -O $(MODE) compiler/main.fl -o build/stage3
	build/flint asm -O $(MODE) compiler/main.fl > build/stage2.s
	build/stage3 asm -O $(MODE) compiler/main.fl > build/stage3.s
	cmp build/stage2.s build/stage3.s && echo "fixpoint: build/flint and build/stage3 produce the same compiler"

test: build/flint
	FLINT=build/flint tests/run.sh

# the C compiler against the Flint one: identical IR, assembly and errors for every test program
diff-test: build/flint build/flint-c
	for w in ir ir-fast asm asm-safe asm-fast; do OLD=build/flint-c NEW=build/flint tests/diff.sh $$w || exit 1; done

# a release: the single flint binary (std and the runtime are embedded in it), and its checksum
release: build/flint
	@mkdir -p build/release/flint-$(VERSION)-aarch64-macos
	cp build/flint README.md build/release/flint-$(VERSION)-aarch64-macos/
	tar -C build/release -czf build/release/flint-$(VERSION)-aarch64-macos.tar.gz flint-$(VERSION)-aarch64-macos
	cd build/release && shasum -a 256 flint-$(VERSION)-aarch64-macos.tar.gz > flint-$(VERSION)-aarch64-macos.tar.gz.sha256
	@cat build/release/flint-$(VERSION)-aarch64-macos.tar.gz.sha256

install: build/flint
	install -d $(BINDIR)
	install -m 755 build/flint $(BINDIR)/flint

uninstall:
	rm -f $(BINDIR)/flint

clean:
	rm -rf build

.PHONY: all test diff-test selfhost-check check-std-list release install uninstall clean
