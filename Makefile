CXX := g++
CXXFLAGS := -std=c++20 -O3 -march=native -pthread
INCLUDES := -I cpp20/include -I cpp20/include/util

# TARGET C++20 
SRC := cpp20/src/main.cpp \
       $(wildcard cpp20/src/compressor/*.cpp) \
       $(wildcard cpp20/src/decompressor/*.cpp) \
       cpp20/src/util/utility.cpp \
       cpp20/src/util/miniz.c

TARGET := minizpp

# TARGET OPENMP
OMP_CXXFLAGS := -std=c++20 -O3 -march=native -fopenmp
OMP_INCLUDES := -I openmp/include -I openmp/include/util

OMP_SRC := openmp/src/main.cpp \
           $(wildcard openmp/src/compressor/*.cpp) \
           $(wildcard openmp/src/decompressor/*.cpp) \
           $(wildcard openmp/src/util/*.cpp) \
           openmp/src/util/miniz.c

OMP_TARGET := minizpp_omp

# RULES

.PHONY: cpp20 openmp clean

cpp20: $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRC) -o $(TARGET)

openmp: $(OMP_SRC)
	$(CXX) $(OMP_CXXFLAGS) $(OMP_INCLUDES) $(OMP_SRC) -o $(OMP_TARGET)

clean:
	rm -f $(TARGET) $(OMP_TARGET)
