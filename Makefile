CFLAGS  := -O3 -Wall -Wno-unused-result
LD      := gcc
LDFLAGS := ${LDFLAGS} -lrdmacm -libverbs -lrt -lpthread

main: common.o conn.o main.o
	${LD} -o $@ $^ ${LDFLAGS}

PHONY: clean
clean:
	rm  ./*.o main
