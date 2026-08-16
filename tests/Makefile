CXXFLAGS = -std=c++14 -Wall -Wextra -O1 -g

TESTS = tz_test config_test json_test tzvec_test effects_test

all: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; ./$$t || exit 1; echo; done

# The JSON scanner parses untrusted network input, so "did not crash" is not a
# strong enough result -- run it under ASan/UBSan where an over-read aborts.
asan: CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean $(TESTS)
	@for t in $(TESTS); do echo "=== $$t (sanitized) ==="; ./$$t || exit 1; echo; done

tz_test: tz_test.cpp ../src/tz.cpp ../src/tz.h
	$(CXX) $(CXXFLAGS) -o $@ tz_test.cpp ../src/tz.cpp

config_test: config_test.cpp ../src/config.h
	$(CXX) $(CXXFLAGS) -o $@ config_test.cpp

json_test: json_test.cpp ../src/json.cpp ../src/json.h
	$(CXX) $(CXXFLAGS) -o $@ json_test.cpp ../src/json.cpp

tzvec_test: tzvec_test.cpp ../src/tz.cpp ../src/tz.h
	$(CXX) $(CXXFLAGS) -o $@ tzvec_test.cpp ../src/tz.cpp

# Regenerate the cross-validation vectors from the shipping page's own code.
# Needs node; the committed tz_vectors.txt means `make` alone does not.
vectors:
	node ../tools/gen_tz_vectors.mjs > tz_vectors.txt

clean:
	rm -rf $(TESTS) *.dSYM

.PHONY: all asan vectors clean

effects_test: effects_test.cpp ../src/effects.cpp ../src/effects.h
	$(CXX) $(CXXFLAGS) -o $@ effects_test.cpp ../src/effects.cpp
