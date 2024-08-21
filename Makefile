FILES = fat_utf16_utf8.c fat32.c lfn.c
OBJS = ${FILES:.c=.o}

all: $(OBJS)
	$(CC) $(OBJS) -o fat32

$(OBJS): %.o: %.c
	$(CC) -c $< -g -O0 -o $@

clean:
	-rm $(OBJS)