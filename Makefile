# PolyForce: wavetable synth as a VST2 instrument for MPC OS (Force / MPC standalone).
# Builds on Linux or WSL. Native: g++ (tests, x86 bench). Device: arm-linux-gnueabihf-g++ 13 (the
# Force ships GCC 13's libstdc++, so the .so links it dynamically).
# Your own settings (FORCE, SSH_KEY, PY, WAVETABLES, ...) go in local.mk, which git ignores.
-include local.mk

CXX      ?= g++
ARM_CXX  ?= arm-linux-gnueabihf-g++
BUILD    := build
# FORCE: the device, root@<ip>, for bench-device and plugin-install. SSH_KEY: the private key for
# it (empty: ssh's own defaults). PY: a Python 3 with Pillow, for skin, preview and plugin-package.
FORCE    ?=
SSH_KEY  ?=
PY       ?= python3

ifneq ($(filter bench-device plugin-install,$(MAKECMDGOALS)),)
ifeq ($(strip $(FORCE)),)
$(error set FORCE=root@<device-ip> (on the command line or in local.mk))
endif
endif

# -n: ssh otherwise inherits make's stdin and hangs. No host-key checks: the Force's DHCP
# address changes on every restart, so its key is never in known_hosts.
SSH_OPTS := $(if $(strip $(SSH_KEY)),-i $(SSH_KEY) -o IdentitiesOnly=yes) -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
SSH      := ssh -n $(SSH_OPTS)

MV       := third_party/mpc-vst-plugins
SURF     := surface
SURF_OUT := $(SURF)/build
SKIN_DIR := $(SURF_OUT)/skin/Devko - VST - PolyForce
GEN      := $(SURF_OUT)/param_ids.h $(SURF_OUT)/factory_presets.h
SKIN     := $(SURF_OUT)/skin.stamp

SRC      := $(wildcard dsp/*.cpp) $(wildcard plugin/*.cpp)
HDR      := $(wildcard dsp/*.h plugin/*.h)
INC      := -I$(SURF_OUT) -Iplugin

# -funsafe-math-optimizations: without it GCC won't use NEON for float vectors on ARMv7
# (NEON flushes denormals, so it isn't IEEE-exact). Not -ffast-math: keep isfinite() honest.
# -fno-tree-loop-distribute-patterns: the engine clears its small per-chunk buffers with vector
# stores on purpose; GCC would turn those loops back into (much slower) memset calls.
ARM_OPT  := -O3 -march=armv7-a -mtune=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard \
            -funsafe-math-optimizations -fno-math-errno -fno-tree-loop-distribute-patterns
ARM_SO   := $(BUILD)/arm/polyforce.so
ARM_SO_STAGES := $(BUILD)/arm/polyforce_stages.so
ARM_BENCH := $(BUILD)/arm/pfbench

.PHONY: all surface skin test test-arm test-arm-pgo test-tables bench arm-plugin arm-bench arm-bench-stages bench-device preview plugin-package plugin-install clean FORCE
# A recipe that fails leaves no half-written target behind for the next make to trust.
.DELETE_ON_ERROR:
# The stage-timing build too: a -DPF_STAGE_TIMING break shows here, not at bench time.
all: test arm-plugin $(BUILD)/polyforce_stages.so

# --- generated --------------------------------------------------------------------------------
# surface.py: params.json, layout.conf, vst.json and build/param_ids.h (the C++ side). Needs only
# python3, so tests and the .so build anywhere. It also checks the layout (keys, options, when=,
# Q-Link sets, geometry) before writing anything.
surface: $(GEN)
# presets/Factory itself too: its time changes when a preset is deleted.
$(GEN) &: $(SURF)/surface.py presets/Factory $(wildcard presets/Factory/*.pfp)   # one run writes both
	python3 $(SURF)/surface.py

# The skin (TUI.json + PNGs) and the plugin-list entry: sd88me's generator, Pillow and a host gcc.
# Then surface/skin_polish.py redraws the knob strips, trigger buttons and stepper arrows (same names
# and sizes) from build/skin_style.json; it checks the skin first and fails the build on a mismatch.
# SHADOW_TITLE_FONT = TITLE_FONT in surface.py.
skin: $(SKIN)
$(SKIN): $(GEN) $(MV)/tools/gen_vst.py $(MV)/tools/shadow_skin.py $(MV)/tools/skin_assets.py $(MV)/tools/shadow_art.c \
         $(SURF)/skin_polish.py $(wildcard $(SURF)/fonts/*.ttf)
	mkdir -p $(SURF_OUT)
	gcc -O2 -I$(MV)/tools/vendor/force-shadow/tools -o $(SURF_OUT)/shadow_art $(MV)/tools/shadow_art.c -lm
	cd $(SURF) && SHADOW_TITLE_FONT=fonts/TitilliumWeb-Bold.ttf $(PY) ../$(MV)/tools/gen_vst.py vst.json
	$(PY) $(SURF)/skin_polish.py "$(SKIN_DIR)/Plugin Skins" --layout $(SURF)/layout.conf --style $(SURF_OUT)/skin_style.json
	touch $@

# Skin previews (PNG per page) for checking the layout without a device.
preview: $(SKIN)
	$(PY) $(MV)/tools/studio.py preview "$(SKIN_DIR)/Plugin Skins" -o $(SURF_OUT)/page_%d.png

# --- native -----------------------------------------------------------------------------------
# Sample wavetables for the tests (Serum-layout WAVs). Kept OUTSIDE this repo: third-party
# content, never committed or packaged.
WAVETABLES ?= $(firstword $(wildcard ../wavetables ../../wavetables) ../wavetables)

# The whole plugin through its VST2 entry points, under ASan/UBSan.
test: $(BUILD)/plugin_test
	PF_WAVETABLES="$(WAVETABLES)" $(BUILD)/plugin_test

TESTS    := $(wildcard test/*_test.cpp)
$(BUILD)/plugin_test: $(TESTS) $(wildcard test/*.h) $(SRC) $(HDR) $(GEN) | $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -pthread \
		$(INC) $(SRC) $(TESTS) -o $@

# The same suite cross-compiled for the Force's CPU and run under qemu-user (no sanitizers):
# catches 32-bit and ARM-only code paths (the FPSCR flush, NEON float vectorisation).
test-arm: $(BUILD)/arm/plugin_test
	PF_WAVETABLES="$(WAVETABLES)" qemu-arm -L /usr/arm-linux-gnueabihf $<

$(BUILD)/arm/plugin_test: $(TESTS) $(wildcard test/*.h) $(SRC) $(HDR) $(GEN)
	mkdir -p $(BUILD)/arm
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wall -Wextra -Wno-psabi -pthread $(INC) $(SRC) $(TESTS) -o $@

# Every WAV under $(WAVETABLES): load, check, play through the engine; load cost + memory.
test-tables: $(BUILD)/tables_sweep
	$(BUILD)/tables_sweep "$(WAVETABLES)"

$(BUILD)/tables_sweep: test/tables_sweep.cpp $(wildcard dsp/*.cpp) $(HDR) | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(wildcard dsp/*.cpp) $< -o $@

# x86 numbers say nothing about the Force; this only checks the bench and the .so paths work
# (the plain plugin, then the stage-timing build).
bench: $(BUILD)/polyforce.so $(BUILD)/polyforce_stages.so $(BUILD)/pfbench
	$(BUILD)/pfbench $(BUILD)/polyforce.so -s 1 -c -1
	$(BUILD)/pfbench $(BUILD)/polyforce_stages.so -v 8 -u 1,8 -s 1 -c -1

X86_SO_CMD = $(CXX) -std=c++17 -O3 -fno-tree-loop-distribute-patterns -fPIC -fvisibility=hidden -Wall -Wextra -pthread $(INC) -shared -Wl,--no-undefined
$(BUILD)/polyforce.so: $(SRC) $(HDR) $(GEN) | $(BUILD)
	$(X86_SO_CMD) $(SRC) -o $@
$(BUILD)/polyforce_stages.so: $(SRC) $(HDR) $(GEN) | $(BUILD)
	$(X86_SO_CMD) -DPF_STAGE_TIMING $(SRC) -o $@

$(BUILD)/pfbench: tools/bench.cpp dsp/wavetable.cpp $(HDR) $(GEN) | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(INC) $< dsp/wavetable.cpp -ldl -o $@

# --- device -----------------------------------------------------------------------------------
# The .so MPC loads: only VSTPluginMain exported; --no-undefined because an unresolved symbol
# otherwise only shows up as MPC crashing on load.
ARM_SO_FLAGS = -std=c++17 $(ARM_OPT) -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra -Wno-psabi -pthread $(INC)
ARM_SO_LINK  = -shared -Wl,--no-undefined -Wl,-soname,polyforce.so
ARM_SO_CMD   = $(ARM_CXX) $(ARM_SO_FLAGS) $(ARM_SO_LINK)

# Profile-guided: on by default when qemu-arm is installed (as test-arm needs); PGO=0 builds
# without. A copy of the plugin compiled with counters is linked into tools/pgo_train.cpp,
# which plays a spread of patches under qemu-arm (about 10 s); then the .so is compiled from
# the same sources with the same flags plus that profile, which tells the compiler which paths
# are hot. -fprofile-partial-training keeps functions the trainer never ran optimised as usual;
# inside a trained function, branches it never took (other LFO shapes, glide, mono, ...) are
# treated as cold. ARM instruction counts: 8 voices -7.7%, 1 voice -8.5%, 8 voices x 8 unison
# with the busy matrix -2.6%. Objects keep one path (dir_name.o) in both rounds: GCC names the
# profile files after it. A missing profile fails the build instead of quietly building without.
PGO      ?= auto
QEMU_ARM := $(shell command -v qemu-arm 2>/dev/null)
PGO_ON   := $(if $(filter auto,$(PGO)),$(if $(QEMU_ARM),1,0),$(PGO))
PGO_DIR  := $(BUILD)/arm/pgo
PGO_PROF := $(abspath $(PGO_DIR)/profile)
PGO_OBJ  := $(PGO_DIR)/obj
PGO_O    = $(PGO_OBJ)/$$(echo $$f | tr / _ | sed 's/\.cpp$$/.o/')

# The .so is rebuilt when the way it is built changes (PGO on/off, flags), not only its sources.
ARM_SO_STAMP := $(BUILD)/arm/so_flags
$(ARM_SO_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo '$(PGO_ON) $(ARM_SO_FLAGS)' | cmp -s - $@ || echo '$(PGO_ON) $(ARM_SO_FLAGS)' > $@
FORCE:

arm-plugin: $(ARM_SO)
$(ARM_SO): $(SRC) $(HDR) $(GEN) tools/pgo_train.cpp $(ARM_SO_STAMP)
	mkdir -p $(BUILD)/arm
ifeq ($(PGO_ON),1)
	rm -rf $(PGO_DIR) && mkdir -p $(PGO_OBJ) $(PGO_PROF)
	for f in $(SRC); do $(ARM_CXX) $(ARM_SO_FLAGS) -fprofile-generate=$(PGO_PROF) -fprofile-update=prefer-atomic \
		-c $$f -o $(PGO_O) || exit 1; done
	$(ARM_CXX) $(ARM_SO_FLAGS) -fprofile-generate -static tools/pgo_train.cpp $(PGO_OBJ)/*.o -o $(PGO_DIR)/train
	PF_DATA_DIR=$(PGO_DIR) PF_TABLE_ROOTS=$(PGO_DIR) PF_PRESET_ROOTS=$(PGO_DIR) PF_TUNING_ROOTS=$(PGO_DIR) PF_CPU_GUARD=0 \
		qemu-arm $(PGO_DIR)/train
	@n=$$(ls $(PGO_PROF)/*.gcda 2>/dev/null | wc -l); [ $$n -eq $(words $(SRC)) ] || \
		{ echo "PGO: $$n of $(words $(SRC)) profiles written (PGO=0 builds without)"; exit 1; }
	for f in $(SRC); do $(ARM_CXX) $(ARM_SO_FLAGS) -fprofile-use=$(PGO_PROF) -fprofile-partial-training -Werror=missing-profile \
		-c $$f -o $(PGO_O) || exit 1; done
	$(ARM_CXX) $(ARM_SO_FLAGS) $(ARM_SO_LINK) $(PGO_OBJ)/*.o -o $@
	@echo "profile-guided build"
else
	$(ARM_SO_CMD) $(SRC) -o $@
	@echo "plain build (PGO=$(PGO): qemu-arm $(if $(QEMU_ARM),found,not found))"
endif
	arm-linux-gnueabihf-strip --strip-unneeded $@
	@arm-linux-gnueabihf-readelf -V $@ | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | sed 's/^/needs /'
	@arm-linux-gnueabihf-nm -D --defined-only $@ | grep -c ' T ' | sed 's/^/exported functions: /'

# The suite against the objects the shipped .so is linked from (profile-guided), under qemu.
test-arm-pgo: $(ARM_SO)
ifeq ($(PGO_ON),1)
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wno-psabi -pthread $(INC) $(TESTS) $(PGO_OBJ)/*.o -o $(BUILD)/arm/plugin_test_pgo
	PF_WAVETABLES="$(WAVETABLES)" qemu-arm -L /usr/arm-linux-gnueabihf $(BUILD)/arm/plugin_test_pgo
else
	@echo "test-arm-pgo: the .so is a plain build here (PGO=$(PGO)); test-arm covers it"
endif

# The profiling build: the same plugin with timers between the render passes (dsp/stages.h)
# and one more export, PolyForceStageTimes, which pfbench reads. Never shipped.
arm-bench-stages: $(ARM_SO_STAGES) $(ARM_BENCH)
$(ARM_SO_STAGES): $(SRC) $(HDR) $(GEN)
	mkdir -p $(BUILD)/arm
	$(ARM_SO_CMD) -DPF_STAGE_TIMING $(SRC) -o $@
	arm-linux-gnueabihf-strip --strip-unneeded $@

arm-bench: $(ARM_BENCH)
$(ARM_BENCH): tools/bench.cpp dsp/wavetable.cpp $(HDR) $(GEN)
	mkdir -p $(BUILD)/arm
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wall -Wextra -Wno-psabi $(INC) $< dsp/wavetable.cpp -ldl -o $@

# Bench on the Force: copies the .so, its stage-timing build, the bench and one full-size
# (256-frame) sample table to /tmp, runs pinned to core 1 (MPC's audio workers own cores 2-3)
# while MPC keeps running, then deletes them. Touches nothing else on the device. Verdicts come
# from the plain .so; the stage build then shows where the time goes, and with a table how
# much a multi-MB table costs over one that fits the cache.
#   make bench-device FORCE=root@<ip> WAVETABLES=<folder>
BENCH_ARGS ?= -v 1,2,4,8 -u 1,2,4,8 -s 3
BENCH_MATRIX_ARGS ?= -v 8 -u 1,8 -s 3 -m 1   # the same with a busy modulation matrix
BENCH_STAGE_ARGS ?= -v 8 -u 1,8 -s 3
TABLE      ?= $(shell find "$(WAVETABLES)" -name '*.wav' -size +2047k 2>/dev/null | sort | head -1)
# The first table at least 2 MB (a 256-frame float table is 2 MB) under $(WAVETABLES); none:
# the large-table run is skipped, and says so. The make fails if any pfbench run fails.
bench-device: $(ARM_SO) $(ARM_SO_STAGES) $(ARM_BENCH)
	$(SSH) $(FORCE) 'rm -f /tmp/pf_table.wav'
	scp -q $(SSH_OPTS) $(ARM_SO) $(ARM_SO_STAGES) $(ARM_BENCH) $(FORCE):/tmp/
	@if [ -n "$(TABLE)" ]; then scp -q $(SSH_OPTS) "$(TABLE)" $(FORCE):/tmp/pf_table.wav; \
	 else echo "no table of 2 MB or more under WAVETABLES=$(WAVETABLES): the large-table run is skipped"; fi
	$(SSH) $(FORCE) 'r=0; T=; [ -f /tmp/pf_table.wav ] && T="-t /tmp/pf_table.wav"; \
		/tmp/pfbench /tmp/polyforce.so $(BENCH_ARGS) -c 1 || r=1; \
		/tmp/pfbench /tmp/polyforce.so $(BENCH_MATRIX_ARGS) -c 1 || r=1; \
		/tmp/pfbench /tmp/polyforce_stages.so $(BENCH_STAGE_ARGS) -c 1 $$T || r=1; \
		rm -f /tmp/pfbench /tmp/polyforce.so /tmp/polyforce_stages.so /tmp/pf_table.wav; exit $$r'

# Release zip: plugin + skin + sd88me's installer (stops MPC, backs up and edits
# MPC.settings, restarts MPC).
PLUGIN_VERSION ?= 0.0.3
plugin-package: $(ARM_SO) $(SKIN)
	@# Everything shipped runs under BusyBox on the device: a CR in a script breaks it there.
	@! grep -l "$$(printf '\r')" $(MV)/tools/release/* || { echo "error: CRLF in a shipped script"; exit 1; }
	$(PY) $(MV)/tools/release.py --so $(ARM_SO) --skin "$(SKIN_DIR)" --entry $(SURF_OUT)/pluginlist-entry.xml \
		--version $(PLUGIN_VERSION) --repo Devko/PolyForce --license MIT \
		--about "PolyForce wavetable synth (preview): 8 voices, 2 wavetable oscillators with 8x unison and subs, 2 filters with comb and vowel, 2 LFOs, a 12-slot mod matrix, arpeggiator and sequencers, presets, microtuning." \
		--requires "root SSH (MockbaMod)" \
		--user-data Wavetables --user-data Presets --user-data Tunings \
		--user-data favorites.txt --user-data recent.txt --user-data preset_favorites.txt --user-data preset_recent.txt \
		-o dist

# Install on a device: stops MPC, backs up + edits MPC.settings, restarts MPC. Save the MPC
# project first. Usage: make plugin-install FORCE=root@<ip>
PKG_TMP := /tmp/pfpkg
plugin-install: plugin-package
	rm -rf $(PKG_TMP) && mkdir -p $(PKG_TMP)
	python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' dist/PolyForce-$(PLUGIN_VERSION)-mpc-armv7.zip $(PKG_TMP)
	tar -C $(PKG_TMP) -cf - PolyForce-$(PLUGIN_VERSION) | ssh $(SSH_OPTS) $(FORCE) 'rm -rf /tmp/PolyForce-$(PLUGIN_VERSION) && tar -C /tmp -xf -'
	$(SSH) $(FORCE) 'sh /tmp/PolyForce-$(PLUGIN_VERSION)/install.sh -y'

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(SURF_OUT)
