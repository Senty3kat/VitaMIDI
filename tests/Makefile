test: test_umidi
	./test_umidi

test_umidi: test_umidi.c ../common/umidi.c ../common/umidi.h ../include/vitamidi.h
	cc -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -I../include -I../common -o $@ test_umidi.c ../common/umidi.c

clean:
	rm -f test_umidi
