CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -I. -Isrc/
DEPS = sharkybuf.h

src/%.o : src/%.c $(DEPS)
	$(CC) -o $@ $< $(CFLAGS)

##bin/% : src/%.c
##	$(CC) -o $@ $< $(CFLAGS)

bin/sharky : src/sharky.o src/sharkybuf.o
	$(CC) -o $@ src/sharky.o src/sharkybuf.o $(CFLAGS)
