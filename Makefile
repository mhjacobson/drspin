drspin: drspin.cpp lldb-symbolicator.cpp lldb-symbolicator.h freebsd-symbolicator.cpp freebsd-symbolicator.h util.h
	c++ --std=c++17 -o drspin drspin.cpp lldb-symbolicator.cpp freebsd-symbolicator.cpp -lc++ -lutil -g
