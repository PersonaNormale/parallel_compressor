CXX := g++
CXXFLAGS := -std=c++20 -O3 -march=native -pthread
INCLUDES := -I cpp20/include -I cpp20/include/util

SRC := cpp20/src/main.cpp \
       $(wildcard cpp20/src/compressor/*.cpp) \
       $(wildcard cpp20/src/decompressor/*.cpp) \
       cpp20/src/util/utility.cpp \
       cpp20/src/util/miniz.c

TARGET := minizpp

.PHONY: cpp20 clean

cpp20: $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)
