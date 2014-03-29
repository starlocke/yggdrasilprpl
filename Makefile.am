EXTRA_DIST = \
	Makefile.mingw \
	README

pkgdir = $(libdir)/purple-$(PURPLE_MAJOR_VERSION)

YGGDRASILSOURCES = yggdrasilprpl.c

AM_CFLAGS = $(st)

libyggdrasil_la_LDFLAGS = -module -avoid-version

# yggdrasilprpl isn't built by default. when it is built, it's dynamically linked.
st =
pkg_LTLIBRARIES    = libyggdrasil.la
libyggdrasil_la_SOURCES = $(YGGDRASILSOURCES)
libyggdrasil_la_LIBADD  = $(GLIB_LIBS)

AM_CPPFLAGS = \
	-I$(top_srcdir)/libpurple \
	-I$(top_builddir)/libpurple \
	$(GLIB_CFLAGS) \
	$(DEBUG_CFLAGS)
