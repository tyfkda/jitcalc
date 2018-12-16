CFLAGS=-O2 -std=gnu11 -Wall -Werror

calc: main.c
	gcc $(CFLAGS) -o jitcalc main.c
jitcalc: main.c
	gcc $(CFLAGS) -o jitcalc -D JIT main.c

normal-test: calc
	sh test.sh
jit-test: jitcalc
	sh test.sh
test: normal-test clean jit-test ;

clean:
	-rm -f jitcalc jitcalc.exe
