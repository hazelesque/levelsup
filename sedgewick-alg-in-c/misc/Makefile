CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE

bin/% : src/%.c
	$(CC) -o $@ $< $(CFLAGS)
