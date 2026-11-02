# opzsim — OP-Z engine host simulator for macOS.
#
#   make -C sim          build opzsim and every engine under engines/ as a dylib
#   make -C sim test     build, then run the host checks and golden-render tests
#   make -C sim golden   re-render and overwrite the golden hashes (review the diff!)
#   ENGINE=name          with test or golden: only that engine's golden renders (host checks still run on test)
#   make -C sim cycles   Blackfin cycle counts of every engine in bfin-elf-run (skipped if not installed)
#   make -C sim wasm     every engine and SDK template as WebAssembly with the host, for the web simulator (sim/web)
#   make -C sim wasm-test  build them, then check every scenario rendered in wasm against the golden hashes
#
# Paths stay relative: the repository path contains spaces.

CC      = clang
BUILD   = build
CFLAGS  = -std=c11 -O2 -Wall -Wextra -ffp-contract=off -I../sdk/include
# Engines: freestanding, integer only. -ffp-contract=off keeps any stray float maths identical across Macs.
ENGINE_CFLAGS = -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-common -ffp-contract=off -fno-stack-protector -I../sdk/include

SIM_SRC  = $(wildcard src/*.c)
ENGINES  = $(patsubst ../engines/%/plug.json,%,$(wildcard ../engines/*/plug.json))
DYLIBS   = $(ENGINES:%=$(BUILD)/engines/%.dylib)
FIXTURES = $(patsubst tests/fixtures/%.c,$(BUILD)/fixtures/%.dylib,$(wildcard tests/fixtures/*.c))
SDK_H    = $(wildcard ../sdk/include/*.h)
TESTKIT  = $(BUILD)/samples/opzk_testkit.aif

.PHONY: all test golden cycles clean wasm wasm-test wasm-toolchain

all: $(BUILD)/opzsim $(DYLIBS) $(FIXTURES) $(TESTKIT)

$(BUILD)/opzsim: $(SIM_SRC) src/host.h ../sdk/include/opz_engine.h ../sdk/include/opz_tables.h ../sdk/include/opz_tables_gen.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SIM_SRC)

define ENGINE_RULE
$(BUILD)/engines/$(1).dylib: $(wildcard ../engines/$(1)/*.c) $(wildcard ../engines/$(1)/*.h) $(SDK_H)
	@mkdir -p $(BUILD)/engines
	$(CC) $(ENGINE_CFLAGS) -dynamiclib -o $$@ $(wildcard ../engines/$(1)/*.c)
endef
$(foreach e,$(ENGINES),$(eval $(call ENGINE_RULE,$(e))))

$(BUILD)/fixtures/%.dylib: tests/fixtures/%.c $(SDK_H)
	@mkdir -p $(BUILD)/fixtures
	$(CC) $(ENGINE_CFLAGS) -dynamiclib -o $@ $<

# Sample packs for the scenarios that attach one (opzk_testkit*.aif, opzk_plain.aif): synthesised, never committed.
$(TESTKIT): tests/testkit.py ../tools/opz_samples.py
	python3 tests/testkit.py $(BUILD)/samples

ONLY = $(if $(ENGINE),--engine $(ENGINE))

test: all
	python3 tests/golden.py --build $(BUILD) $(ONLY)

golden: all
	python3 tests/golden.py --build $(BUILD) --update $(ONLY)

BFIN ?= $(HOME)/opt/bfin-elf
cycles:
	@if [ -x "$(BFIN)/bin/bfin-elf-run" ] && [ -x "$(BFIN)/bin/bfin-elf-gcc" ]; then \
		python3 ../tools/opz_cycles.py --all --toolchain "$(BFIN)" --markdown; \
	else \
		echo "cycles: skipped, no bfin-elf-run/bfin-elf-gcc in $(BFIN)/bin (research/toolchain-setup.md)"; \
	fi

# ---- WebAssembly (sim/web): one module per engine, linked with this host -------------------------------------
# Needs an LLVM with the wasm32 target, wasm-ld and a WASI sysroot; Apple's clang has no wasm32 target.
#   brew install llvm lld wasi-libc wasi-runtimes        (sdk/docs/QUICKSTART.md)
# Override WASM_CC, WASM_LD and WASI_SYSROOT for another install (e.g. wasi-sdk: $WASI_SDK/bin/clang, its sysroot).
BREW         := $(shell command -v brew 2>/dev/null)
WASM_CC      ?= $(if $(BREW),$(shell brew --prefix llvm 2>/dev/null)/bin/clang,clang)
WASM_LD      ?= $(if $(BREW),$(shell brew --prefix lld 2>/dev/null)/bin/wasm-ld,wasm-ld)
WASI_SYSROOT ?= $(if $(BREW),$(shell brew --prefix 2>/dev/null)/share/wasi-sysroot,/opt/wasi-sdk/share/wasi-sysroot)
WASM         = $(BUILD)/wasm
WASM_TARGET  = --target=wasm32-wasip1 --sysroot="$(WASI_SYSROOT)"
WASM_CFLAGS  = $(WASM_TARGET) -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -ffp-contract=off -I../sdk/include -Isrc
WASM_ENGINE_CFLAGS = $(WASM_TARGET) $(ENGINE_CFLAGS) -Wno-unknown-warning-option -Wno-unused-but-set-global
# A reactor (no _start): the page calls _initialize, then ws_* for live play or opzsim_main for a scenario.
WASM_LDFLAGS = $(WASM_TARGET) -fuse-ld="$(WASM_LD)" -mexec-model=reactor -Wl,-z,stack-size=1048576 \
               -Wl,--export=opzsim_main -Wl,--export=malloc -Wl,--export=free
WASM_HOST_OBJ = $(patsubst src/%.c,$(WASM)/obj/host/%.o,$(SIM_SRC))
TEMPLATES     = synth drum fx
WASM_MODULES  = $(ENGINES:%=$(WASM)/engines/%.wasm) $(TEMPLATES:%=$(WASM)/templates/%.wasm)

wasm: wasm-toolchain $(WASM_MODULES) $(TESTKIT)

wasm-toolchain:
	@"$(WASM_CC)" --print-targets 2>/dev/null | grep -q wasm32 || { echo "wasm: $(WASM_CC) has no wasm32 target;" \
		"install LLVM (brew install llvm lld wasi-libc wasi-runtimes) or set WASM_CC (sdk/docs/QUICKSTART.md)"; exit 1; }
	@test -x "$(WASM_LD)" || command -v "$(WASM_LD)" >/dev/null || { echo "wasm: no wasm-ld at $(WASM_LD);" \
		"brew install lld, or set WASM_LD"; exit 1; }
	@test -f "$(WASI_SYSROOT)/include/wasm32-wasip1/stdio.h" -o -f "$(WASI_SYSROOT)/include/stdio.h" || { \
		echo "wasm: no WASI sysroot at $(WASI_SYSROOT); brew install wasi-libc wasi-runtimes, or set WASI_SYSROOT"; \
		exit 1; }

$(WASM)/obj/host/%.o: src/%.c src/host.h $(SDK_H) | wasm-toolchain
	@mkdir -p $(WASM)/obj/host
	"$(WASM_CC)" $(WASM_CFLAGS) $(if $(filter opzsim,$*),-Dmain=opzsim_main) -c -o $@ $<

# $(1) module name, $(2) source directory, $(3) output
define WASM_RULE
$(3): $(wildcard $(2)/*.c) $(wildcard $(2)/*.h) $(SDK_H) $(WASM_HOST_OBJ) wasm/websim.c
	@mkdir -p $(WASM)/obj/$(1) $$(dir $$@)
	@rm -f $(WASM)/obj/$(1)/*.o
	@for f in $(wildcard $(2)/*.c); do b=$$$$(basename "$$$$f" .c); \
		"$(WASM_CC)" $(WASM_ENGINE_CFLAGS) -c -o "$(WASM)/obj/$(1)/e_$$$$b.o" "$$$$f" || exit 1; done
	"$(WASM_CC)" $(WASM_CFLAGS) -DWS_ENGINE_NAME='"$(1)"' -c -o $(WASM)/obj/$(1)/websim.o wasm/websim.c
	"$(WASM_CC)" $(WASM_LDFLAGS) -o $$@ $(WASM_HOST_OBJ) $(WASM)/obj/$(1)/*.o
endef
$(foreach e,$(ENGINES),$(eval $(call WASM_RULE,$(e),../engines/$(e),$(WASM)/engines/$(e).wasm)))
$(foreach t,$(TEMPLATES),$(eval $(call WASM_RULE,template_$(t),../sdk/templates/$(t),$(WASM)/templates/$(t).wasm)))

wasm-test: wasm
	npm --prefix web test

clean:
	rm -rf $(BUILD)
