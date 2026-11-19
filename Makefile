CORE = sched_core.c data_structures.c resource.c input.c
CFLAGS = -std=gnu17

build:
	gcc $(CFLAGS) process_generator.c input.c -o process_generator.out
	gcc $(CFLAGS) clk.c -o clk.out
	gcc $(CFLAGS) scheduler.c $(CORE) -o scheduler.out -lm
	gcc $(CFLAGS) process.c -o process.out
	gcc $(CFLAGS) test_generator.c -o test_generator.out
	gcc $(CFLAGS) sim.c $(CORE) -o sim.out -lm

clean:
	rm -f *.out scheduler.log scheduler.perf

all: clean build

run:
	./process_generator.out

test: sim.out
	bash tests/run_tests.sh

test-ipc: build
	bash tests/run_tests.sh --ipc --quick