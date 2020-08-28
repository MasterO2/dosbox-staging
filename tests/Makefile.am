if BUILD_TESTS

AM_CPPFLAGS = -I$(top_srcdir)/include

SUBDIRS = ../src/misc

bin_PROGRAMS = tests

tests_CXXFLAGS = \
	$(AM_CXXFLAGS) \
	$(CXXFLAGS) \
	-lgtest \
	-lgtest_main

tests_SOURCES = \
	example.cpp

tests_LDADD = ../src/misc/libmisc.a

endif
