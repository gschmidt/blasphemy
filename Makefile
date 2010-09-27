LIBS=-lzephyr -lreadline -lkrb4 -lkrb5 -lk5crypto -ldes425 -lresolv -lcom_err -ldl -lcom_err
CFLAGS=-g
CXXFLAGS=-g

CC=gcc
CXX=g++

all: blasphemy

blasphemy.o: blasphemy.cc
	${CXX} -o $@ -c ${CXXFLAGS} -Icajun $<

blasphemy: blasphemy.o ${LIBS}
	${CXX} -o $@ blasphemy.o ${LIBS}

clean:
	rm -f *.o blasphemy

.PHONY: all clean

