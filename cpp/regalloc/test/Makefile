TOP=../..
include $(TOP)/common.mk


DEFS+=-DDEBUG
OPTS=-O0 -g -Wall

LDFLAGS=-L/usr/local/lib -L$(TOP)
LIBS=-lstdc++ -lm -lc -static -lcore

all: do_all_test


do_all_test: LivenessTest ColorTest
	./LivenessTest
	./ColorTest

LivenessTest: LivenessTest.o
	$(CXX) $@.o $(LDFLAGS) $(LIBS) -o$@

ColorTest: ColorTest.o
	$(CXX) $@.o $(LDFLAGS) $(LIBS) -o$@

clean:
	@rm LivenessTest ColorTest