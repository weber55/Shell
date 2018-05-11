all: 1730sh

1730sh.o: 1730sh.cpp
	g++ -c -Wall -std=c++14 -g -O0 -pedantic-errors 1730sh.cpp

1730sh: 1730sh.o
	g++ -o 1730sh 1730sh.o

clean:
	rm -f 1730sh.o
	rm -f 1730sh