# Sea Otter - DDCS V4.1 controller app.
#   make host        native build (WSL/Linux) - simulator backend + browser panel at :8080
#   make arm         static ARMv5 build for the controller (real backends, --sim available)
#   make test        unit/layer tests, native
#   make test-arm    the same tests cross-built and run under qemu-arm (if installed)
#   make deploy      copy build/arm/seaotter to the unit over SSH (see README)
# Build id shown on the panel: PIPELINE_ID if set (CI), else a local counter.

CC_HOST  ?= gcc
CC_ARM   ?= arm-linux-gnueabi-gcc
CFLAGS   ?= -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
BUILD    := build

COUNTER  := $(BUILD)/buildno
BUILDNO  := $(shell mkdir -p $(BUILD); n=$$(cat $(COUNTER) 2>/dev/null || echo 0); echo $$((n+1)) > $(COUNTER); echo $$((n+1)))
PIPELINE_ID ?= local-$(BUILDNO)
BUILD_DATE := $(shell date +%Y-%m-%d)
DEFS := -DBUILD_ID='"$(PIPELINE_ID)"' -DBUILD_DATE='"$(BUILD_DATE)"'

CORE := src/common.c src/core/settings.c src/core/settings_hints.c src/core/gcode.c src/core/motion.c src/core/program.c src/core/estimate.c src/core/macro.c \
        src/core/machine.c src/core/ctl_tick.c \
        src/hal/fpga.c src/hal/fpga_vendor_init.c src/hal/bus_sim.c src/sim/fpga_sim.c
UI   := src/ui/fonts.c src/ui/gfx.c src/ui/ui.c src/ui/ui_frame.c src/ui/ui_run.c src/ui/ui_jog.c \
        src/ui/ui_files.c src/ui/ui_work.c src/ui/ui_macros.c src/ui/ui_settings.c src/ui/ui_io.c src/ui/ui_view.c src/ui/ui_editor.c \
        src/ui/ui_pendant.c
NET  := src/net/http.c src/net/api.c src/net/web_panel.c
HAL  := src/hal/bus_ioctl.c src/hal/panel_tm1638.c src/hal/panel_sim.c src/hal/display.c
APP  := $(CORE) $(UI) $(NET) $(HAL) src/main.c
TESTS := tests/test_main.c tests/test_fpga.c tests/test_gcode.c tests/test_motion.c \
         tests/test_machine.c tests/test_ui.c tests/test_planner.c tests/test_estimate.c

.PHONY: host arm test test-arm clean deploy
host: $(BUILD)/host/seaotter
arm:  $(BUILD)/arm/seaotter

src/net/web_panel.c: web/panel.html tools/embed.py
	python3 tools/embed.py web/panel.html $@ WEB_PANEL

$(BUILD)/host/seaotter: $(APP) $(wildcard src/*/*.h src/*.h)
	@mkdir -p $(dir $@)
	$(CC_HOST) $(CFLAGS) $(DEFS) -o $@ $(APP) -lm

$(BUILD)/arm/seaotter: $(APP) $(wildcard src/*/*.h src/*.h)
	@mkdir -p $(dir $@)
	$(CC_ARM) $(CFLAGS) -Os -march=armv5te -mtune=arm926ej-s -static $(DEFS) -o $@ $(APP) -lm
	@ls -l $@

$(BUILD)/host/tests: $(CORE) $(UI) src/hal/display.c $(TESTS) $(wildcard src/*/*.h tests/*.h)
	@mkdir -p $(dir $@)
	$(CC_HOST) $(CFLAGS) $(DEFS) -Isrc -o $@ $(CORE) $(UI) src/hal/display.c $(TESTS) -lm

$(BUILD)/arm/tests: $(CORE) $(UI) $(TESTS)
	@mkdir -p $(dir $@)
	$(CC_ARM) $(CFLAGS) -march=armv5te -static $(DEFS) -Isrc -o $@ $(CORE) $(UI) src/hal/display.c $(TESTS) -lm

test: $(BUILD)/host/tests
	cd $(BUILD) && ./host/tests

test-arm: $(BUILD)/arm/tests
	cd $(BUILD) && qemu-arm ./arm/tests

deploy: arm
	scp -i ../mockup/dropbear/ddcsv4_id_ed25519 $(BUILD)/arm/seaotter root@192.168.1.200:/ddcsv4/seaotter

clean:
	rm -rf $(BUILD)/host $(BUILD)/arm
