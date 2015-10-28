# include parent_common.mk for buildsystem's defines
REPO_PARENT=../..
-include $(REPO_PARENT)/parent_common.mk

CFLAGS = -I$(ZIO)/include/ -Wall $(EXTRACFLAGS)

progs := obsbox-dump

all: $(progs)

clean:
	rm -f $(progs) *~ *.o

%: %.c
	$(CC) $(CFLAGS) $^ -o $@
