# Point this at your Linux kernel sources.
# Note: this assumes you're running under UML, because running this on a
# real kernel would be stupid.
KERNEL := /home/tal00r/UML/linux-2.4.20

all: lazyfs.o zero

CC=gcc
CFLAGS   := -O2 -Wall -Wstrict-prototypes -Wmissing-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -g
MODFLAGS := -DMODULE -D__KERNEL__
UMFLAGS  := -I${KERNEL}/include -U__i386__ -Ui386 -DUM_FASTCALL -D__arch_um__ -DSUBARCH="i386" -D_LARGEFILE64_SOURCE -I${KERNEL}/arch/um/include -Derrno=kernel_errno -I${KERNEL}/arch/um/kernel/tt/include -I${KERNEL}/arch/um/kernel/skas/include -DARCH=um

clean:
	rm lazyfs.o zero

lazyfs.o: lazyfs.c
	${CC} -c ${CFLAGS} ${UMFLAGS} ${MODFLAGS} -o $@ $<
