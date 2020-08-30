if BUILD_TESTS

AM_CPPFLAGS = -I$(top_srcdir)/include

SUBDIRS = ../src/misc

bin_PROGRAMS = tests

# FIXME
# some Linux distributions provide only dynamic libs (Fedora) while other
# provide only static libs (Ubuntu).
tests_CXXFLAGS = \
	$(AM_CXXFLAGS) \
	$(CXXFLAGS) \
	-Wl,-Bstatic \
	-lgtest_main \
	-lgtest \
	-Wl,-Bdynamic

tests_SOURCES = \
	example.cpp

tests_LDADD = ../src/misc/libmisc.a

endif
