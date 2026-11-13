CORE = sched_core.c data_structures.c resource.c input.c

build:
	gcc process_generator.c input.c -o process_generator.out
	gcc clk.c -o clk.out
	gcc scheduler.c $(CORE) -o scheduler.out -lm
	gcc process.c -o process.out
	gcc test_generator.c -o test_generator.out
	gcc sim.c $(CORE) -o sim.out -lm

clean:
	rm -f *.out scheduler.log scheduler.perf

all: clean build

run:
	./process_generator.out

test: sim.out
	bash tests/run_tests.sh