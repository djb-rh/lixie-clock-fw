CXXFLAGS = -std=c++14 -Wall -Wextra -O1 -g

TESTS = tz_test config_test

all: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; ./$$t || exit 1; echo; done

tz_test: tz_test.cpp ../src/tz.cpp ../src/tz.h
	$(CXX) $(CXXFLAGS) -o $@ tz_test.cpp ../src/tz.cpp

config_test: config_test.cpp ../src/config.h
	$(CXX) $(CXXFLAGS) -o $@ config_test.cpp

clean:
	rm -rf $(TESTS) *.dSYM

.PHONY: all clean
