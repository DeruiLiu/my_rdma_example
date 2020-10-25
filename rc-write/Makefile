CC = gcc
CFLAGS = -Wall -O2 -g
INCLUDES =
LDFLAGS = -libverbs


SRCS1=setup_ib.c client.c
SRCS2=setup_ib.c server.c

OBJS1=$(SRCS1:.c=.o)
OBJS2=$(SRCS2:.c=.o)

PROG1=client
PROG2=server
PROG = $(PROG1) $(PROG2)
all:$(PROG)

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS1) $(LDFLAGS)

$(PROG2): $(OBJS2)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS2) $(LDFLAGS)

clean:
	$(RM) *.o *~ $(PROG1) $(PROG2)
