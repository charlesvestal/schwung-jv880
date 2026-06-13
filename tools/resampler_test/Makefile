# Build + run the fixed-ratio resampler verification/benchmark.
#
#   cd tools/resampler_test && make run
#
# Compiles our header-only resampler together with the vendored libresample
# sources and runs the comparison.  Host build (clang or gcc).

ROOT    := ../..
DSP     := $(ROOT)/src/dsp
RESAMP  := $(DSP)/resample

CXX     ?= clang++
CC      ?= clang
CXXFLAGS := -O3 -std=c++11 -I$(DSP) -Wall
CFLAGS   := -O3 -I$(RESAMP)

LR_SRCS := $(RESAMP)/resample.c $(RESAMP)/resamplesubs.c $(RESAMP)/filterkit.c
LR_OBJS := $(LR_SRCS:.c=.o)

resampler_test: resampler_test.cpp $(LR_OBJS) $(DSP)/resampler_fixed.h
	$(CXX) $(CXXFLAGS) resampler_test.cpp $(LR_OBJS) -o $@ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: run clean
run: resampler_test
	./resampler_test

clean:
	rm -f resampler_test $(LR_OBJS)
