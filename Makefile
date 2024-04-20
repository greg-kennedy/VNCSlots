all:	vncslots

vncslots:	main.c image.c
#	cc -Wall -Wextra -Ofast -march=native -flto  -o vncslots main.c image.c

#debug:	main.c
	cc -Wall -Wextra -g -fsanitize=address,undefined,leak,integer  -o vncslots main.c image.c

clean:
	rm -f *.o vncslots
