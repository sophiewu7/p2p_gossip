# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++11 -Wall

# Targets
all: process stopall clean

# Compile process
process: process.o
	$(CXX) $(CXXFLAGS) -o process process.o

process.o: process.cpp
	$(CXX) $(CXXFLAGS) -c process.cpp

# Compile stopall
stopall: stopall.o
	$(CXX) $(CXXFLAGS) -o stopall stopall.o

stopall.o: stopall.cpp
	$(CXX) $(CXXFLAGS) -c stopall.cpp

# Clean up
clean:
	rm -f process.o stopall.o

tarball: 
	tar cf - `ls -a | grep -v '^\..*' | grep -v '^proj[0-9].*\.tar\.gz'` | gzip > proj2-$(USER).tar.gz