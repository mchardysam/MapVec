# Host tests. `make check` builds and runs everything, including the round-trip:
# the Python producer writes a .mapvec from a committed fixture and the C parser
# verifies it — two independent implementations of the format agreeing, or failing.
# `make SAN=1 clean check` runs the lot under ASan/UBSan (the fuzz sweeps are
# written for it). Needs python3 for the round-trip; nothing else.
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra
ifeq ($(SAN),1)
CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif
INC = -I ../src

HDRS = ../src/mapvec.h ../src/mapvec_render.h

all: check

test_mapvec_parse: test_mapvec_parse.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_mapvec_parse.cpp

test_mapvec_fill: test_mapvec_fill.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_mapvec_fill.cpp

test_mapvec_fuzz: test_mapvec_fuzz.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_mapvec_fuzz.cpp

test_mapvec_roundtrip: test_mapvec_roundtrip.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_mapvec_roundtrip.cpp

test_mapvec_rotate: test_mapvec_rotate.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_mapvec_rotate.cpp

fixture.mapvec: fixture_overpass.json ../tools/osm_to_vectors.py
	python3 ../tools/osm_to_vectors.py fixture_overpass.json \
	  --bbox=-35.010,148.990,-34.990,149.010 --tol 2 --out fixture.mapvec > /dev/null

check: test_mapvec_parse test_mapvec_fill test_mapvec_fuzz test_mapvec_rotate test_mapvec_roundtrip fixture.mapvec
	./test_mapvec_parse
	./test_mapvec_fill
	./test_mapvec_fuzz
	./test_mapvec_rotate
	./test_mapvec_roundtrip fixture.mapvec

clean:
	rm -f test_mapvec_parse test_mapvec_fill test_mapvec_fuzz test_mapvec_rotate test_mapvec_roundtrip fixture.mapvec

.PHONY: all check clean
