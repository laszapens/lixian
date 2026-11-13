CXX ?= g++
CC ?= gcc
CXXFLAGS ?= -std=c++20 -O3 -march=native -flto -DNDEBUG -Wall -Wextra -Wno-unused-parameter
CFLAGS ?= -std=c11 -O3 -march=native -flto -DNDEBUG -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
LDFLAGS ?= -flto -pthread
FATHOM_INC = -Isrc/third_party/fathom

CORE_SRC = src/bitboard.cpp src/position.cpp src/movegen.cpp
CORE_OBJ = $(CORE_SRC:src/%.cpp=build/%.o)

C_SRC = src/third_party/fathom/tbprobe.c
GLUE_SRC = src/third_party/fathom/tb_glue.cpp
C_OBJ = $(C_SRC:src/%.c=build/%.o)

ENGINE_SRC = $(CORE_SRC) src/eval.cpp src/nnue.cpp src/search.cpp src/book.cpp src/tbprobe_glue.cpp src/uci.cpp
ENGINE_OBJ = $(ENGINE_SRC:src/%.cpp=build/%.o) \
             $(GLUE_SRC:src/%.cpp=build/%.o) $(C_OBJ)

all: lixian

lixian: $(ENGINE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

perft: $(CORE_OBJ) build/perft.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(FATHOM_INC) -c -o $@ $<

# Fathom is C99 and tbchess.c is textually included by tbprobe.c, so only
# tbprobe.c is compiled on its own.
build/%.o: src/%.c
	@mkdir -p build
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FATHOM_INC) -c -o $@ $<

build/%.o: tests/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(FATHOM_INC) -Isrc -c -o $@ $<

# Standalone NNUE trainer, OpenMP only.
trainer: tools/nnue/trainer.cpp
	@mkdir -p build
	$(CXX) -std=c++20 -O3 -march=native -fopenmp -DNDEBUG -o build/trainer tools/nnue/trainer.cpp

debug: CXXFLAGS = -std=c++20 -O1 -g -march=native -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined
debug: LDFLAGS = -pthread -fsanitize=address,undefined
debug: lixian

clean:
	rm -rf build lixian perft

.PHONY: all perft clean debug trainer
