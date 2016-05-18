XEN_ROOT = $(CURDIR)/../..
include $(XEN_ROOT)/tools/Rules.mk

SUBDIRS :=
SUBDIRS += libxenstat

# This compiles if compiler environment has curses
ifeq ($(wildcard /usr/include/curses.h),/usr/include/curses.h)
SUBDIRS += xentop
endif

.PHONY: all install clean distclean

all install clean distclean: %: subdirs-%
