CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

all: minibox

minibox: minibox.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

rootfs: make_rootfs.sh
	sudo ./make_rootfs.sh ./rootfs

clean:
	rm -f minibox

clean-all: clean
	sudo rm -rf rootfs

.PHONY: all rootfs clean clean-all
