CC ?= cc
CXX ?= c++
AR ?= ar
BUILD_DIR ?= build
CPPFLAGS += -Iinclude -Isrc
CFLAGS ?= -O3 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer
CXXFLAGS ?= -O3 -g -std=c++20 -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer
LDLIBS += -pthread -lm

COMMON_OBJ := $(BUILD_DIR)/common.o
POLICY_OBJ := $(BUILD_DIR)/policy.o
PROGRAMS := $(BUILD_DIR)/hfior-bridge \
	$(BUILD_DIR)/hfior-latency-client \
	$(BUILD_DIR)/evdev-rate-probe \
	$(BUILD_DIR)/example-minimal \
	$(BUILD_DIR)/example-full-history \
	$(BUILD_DIR)/example-late-latch
TESTS := $(BUILD_DIR)/test-ring $(BUILD_DIR)/test-policy
GAME_PROTOCOL_DIR := $(BUILD_DIR)/game-protocol
GAME_PROTOCOL_XML := integrations/wayland/experimental-protocol/hyprland-hf-input-v1.xml
GAME_PKGS := sdl3 epoxy wayland-client
GAME_CPPFLAGS = $(shell pkg-config --cflags $(GAME_PKGS)) -I$(GAME_PROTOCOL_DIR) -Ibenchmarks/game
GAME_LDLIBS = $(shell pkg-config --libs $(GAME_PKGS)) -lm

.PHONY: all clean game-benchmark test sanitize
all: $(PROGRAMS) $(BUILD_DIR)/libhfior.a $(TESTS)

game-benchmark: $(BUILD_DIR)/hfior-game

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/common.o: src/common.c src/common.h include/hfior/abi.h include/hfior/ring.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/policy.o: src/policy.c include/hfior/policy.h include/hfior/ring.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/hfior-bridge: reference/bridge/main.c $(COMMON_OBJ) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/hfior-latency-client: reference/latency-client/main.c $(COMMON_OBJ) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/evdev-rate-probe: tools/evdev/evdev-rate-probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(BUILD_DIR)/example-minimal: examples/minimal-consumer/main.c $(COMMON_OBJ) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/example-full-history: examples/full-history-consumer/main.c $(COMMON_OBJ) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/example-late-latch: examples/late-latch-consumer/main.c $(COMMON_OBJ) $(POLICY_OBJ) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/libhfior.a: $(COMMON_OBJ) $(POLICY_OBJ) | $(BUILD_DIR)
	$(AR) rcs $@ $^

$(BUILD_DIR)/test-ring: tests/unit/test_ring.c $(COMMON_OBJ) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test-policy: tests/unit/test_policy.c $(COMMON_OBJ) $(POLICY_OBJ) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(GAME_PROTOCOL_DIR): | $(BUILD_DIR)
	mkdir -p $@

$(GAME_PROTOCOL_DIR)/hyprland-hf-input-v1-client-protocol.h: $(GAME_PROTOCOL_XML) | $(GAME_PROTOCOL_DIR)
	wayland-scanner client-header $< $@

$(GAME_PROTOCOL_DIR)/hyprland-hf-input-v1-protocol.c: $(GAME_PROTOCOL_XML) | $(GAME_PROTOCOL_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/hfior-game-client.o: benchmarks/game/hfior_wayland_client.c benchmarks/game/hfior_wayland_client.h include/hfior/abi.h $(GAME_PROTOCOL_DIR)/hyprland-hf-input-v1-client-protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(GAME_CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/hfior-game-protocol.o: $(GAME_PROTOCOL_DIR)/hyprland-hf-input-v1-protocol.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(GAME_CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/hfior-game-main.o: benchmarks/game/main.cc benchmarks/game/hfior_wayland_client.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(GAME_CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/hfior-game: $(BUILD_DIR)/hfior-game-main.o $(BUILD_DIR)/hfior-game-client.o $(BUILD_DIR)/hfior-game-protocol.o | $(BUILD_DIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(GAME_LDLIBS)

test: all
	./scripts/test.sh

sanitize:
	./scripts/sanitize.sh

clean:
	rm -rf -- $(BUILD_DIR)
