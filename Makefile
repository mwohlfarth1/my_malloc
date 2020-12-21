SRC=my_malloc.c printing.c
GCC=gcc -std=gnu11 -Wall -I"/homes/cs252/public/include"

my_malloc:
	$(GCC) -c $(SRC)

clean:
	rm -f *.o
