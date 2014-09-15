CFLAGS = -I$(ZIO)/include/ -Wall $(EXTRACFLAGS)

progs := obsbox-dump

all: $(progs)

clean:
	rm -f $(progs) *~ *.o

%: %.c
	$(CC) $(CFLAGS) $^ -o $@
