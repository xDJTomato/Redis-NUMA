# NUMAflow - N8N-style memory-scheduling workflow engine (pure C11).
# Builds with GNU make (Linux) or mingw32-make (native Windows).

CC      = gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
INC      = -Iinclude
BUILD    = build
LDLIBS   = -lm

ifeq ($(OS),Windows_NT)
  MKDIR = if not exist $(BUILD) mkdir $(BUILD)
  MKDIR_RESULTS = if not exist ..\results mkdir ..\results
  RM    = del /q
  EXE   = .exe
  NULL  = NUL
else
  MKDIR = mkdir -p $(BUILD)
  MKDIR_RESULTS = mkdir -p ../results
  RM    = rm -rf
  EXE   =
  NULL  = /dev/null
endif

BIN = $(BUILD)/numaflow$(EXE)

SRCS = src/nf_common.c src/numa_shim.c src/nf_json.c src/nf_graph.c \
       src/nf_ops.c src/nf_track.c src/nf_exec.c src/nf_strategy.c src/nf_bench.c \
       src/nf_bridge.c src/nf_adapt.c
OBJS = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
CORE_OBJS = $(filter-out $(BUILD)/nf_bench.o,$(OBJS))

.PHONY: all test report clean ops strategies

all: $(BIN) $(BUILD)/nf_tui$(EXE)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(BUILD)/nf_cli.o: src/nf_cli.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(BIN): $(OBJS) $(BUILD)/nf_cli.o
	$(CC) $(CFLAGS) $(INC) $(OBJS) $(BUILD)/nf_cli.o -o $@ $(LDLIBS)

$(BUILD):
	$(MKDIR)

$(BUILD)/test_smoke$(EXE): $(CORE_OBJS) tests/test_smoke.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) tests/test_smoke.c $(CORE_OBJS) -o $@ $(LDLIBS)

$(BUILD)/test_all$(EXE): $(CORE_OBJS) tests/test_all.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) tests/test_all.c $(CORE_OBJS) -o $@ $(LDLIBS)

$(BUILD)/test_adapt$(EXE): $(CORE_OBJS) tests/test_adapt.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) tests/test_adapt.c $(CORE_OBJS) -o $@ $(LDLIBS)

$(BUILD)/nf_tui$(EXE): $(OBJS) tui/nf_tui.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) tui/nf_tui.c $(OBJS) -o $@ $(LDLIBS)

test: $(BUILD)/test_all$(EXE) $(BUILD)/test_smoke$(EXE) $(BUILD)/test_adapt$(EXE) $(BUILD)/nf_tui$(EXE)
	$(BUILD)/test_all$(EXE)
	$(BUILD)/test_smoke$(EXE)
	$(BUILD)/test_adapt$(EXE)

ops: $(BIN)
	$(BIN) ops

strategies: $(BIN)
	$(BIN) strategies

# generate the benchmark JSON + HTML report
report: $(BIN)
	python eval/report.py || true

clean:
	-$(RM) $(BUILD)\* 2>$(NULL)
	-rmdir $(BUILD) 2>$(NULL) || true

bench: $(BIN)
	$(MKDIR_RESULTS)
	$(BIN) eval --workload zipf --keys 3000 --accesses 120000 --epoch 3000 --budget 256 --nodes 2 --seed 20240517 --out ../results/bench_zipf.json
	$(BIN) eval --workload uniform --keys 3000 --accesses 120000 --epoch 3000 --budget 256 --nodes 2 --seed 20240517 --out ../results/bench_uniform.json
	$(BIN) eval --workload hotspot --keys 3000 --accesses 120000 --epoch 3000 --budget 256 --nodes 2 --seed 20240517 --out ../results/bench_hotspot.json
	$(BIN) eval --workload temporal --keys 3000 --accesses 120000 --epoch 3000 --budget 256 --nodes 2 --seed 20240517 --out ../results/bench_temporal.json
	python eval/report.py
