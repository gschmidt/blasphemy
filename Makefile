# may need -lkrb4, -ldes425 on athena..
LIBS=-lzephyr -lreadline -lkrb5 -lk5crypto -lresolv -lcom_err -ldl
CFLAGS=-g
CXXFLAGS=-g

CC=gcc
CXX=g++

all: blasphemy

blasphemy.o: blasphemy.cc
	${CXX} -o $@ -c ${CXXFLAGS} -Icajun $<

blasphemy: blasphemy.o
	${CXX} -o $@ blasphemy.o ${LIBS}

clean:
	rm -f *.o blasphemy

.PHONY: all clean

