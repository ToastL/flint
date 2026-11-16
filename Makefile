# Flint is written in Flint (compiler/). Building it needs a Flint compiler to start from, stage 0:
#   - FLINT_BOOTSTRAP=path/to/flint, if given
#   - otherwise the release pinned in bootstrap/release (bootstrap/fetch.sh downloads it into
#     build/bootstrap and checks its sha256)
# Stage 0 builds stage 1 (build/stage1), which builds the compiler that's installed: build/flint.
# The compiler is an ordinary Flint package (compiler/flint.toml): each stage runs `flint build` in
# compiler/.
# `make selfhost-check` builds it once more and checks the result is the same (a fixpoint).

STD = $(wildcard std/*.fl)
FLSRC = $(shell find compiler -name '*.fl') $(STD)
MODE ?= ReleaseSafe
PREFIX ?= /usr/local
BINDIR = $(DESTDIR)$(PREFIX)/bin
VERSION = $(shell cat VERSION)
STAGE0 = $(abspath $(if $(FLINT_BOOTSTRAP),$(FLINT_BOOTSTRAP),$(shell bootstrap/fetch.sh)))
BUILD = cd compiler && $(1) build -O $(MODE) -o $(abspath $(2))

all: build/flint

# every std file must be in the list compiler/modules/load.fl embeds
check-std-list: $(STD)
	@for f in $(notdir $(STD)); do grep -q "\"$$f\"" compiler/modules/load.fl || \
	  { echo "std/$$f is missing from STD_FILES in compiler/modules/load.fl"; exit 1; }; done

build/stage1: $(FLSRC) bootstrap/release | check-std-list
	@mkdir -p build
	$(call BUILD,$(STAGE0),$@)

build/flint: build/stage1 $(FLSRC)
	$(call BUILD,$(abspath build/stage1),$@)

# the compiler builds itself to the same assembly
selfhost-check: build/flint
	$(call BUILD,$(abspath build/flint),build/stage3)
	cd compiler && ../build/flint asm -O $(MODE) > ../build/stage2.s
	cd compiler && ../build/stage3 asm -O $(MODE) > ../build/stage3.s
	cmp build/stage2.s build/stage3.s && echo "fixpoint: build/flint and build/stage3 produce the same compiler"

test: build/flint
	FLINT=build/flint tests/run.sh

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

.PHONY: all test selfhost-check check-std-list release install uninstall clean
