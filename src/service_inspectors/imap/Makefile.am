
file_list = \
imap_config.h \
imap.cc \
imap.h \
imap_paf.cc \
imap_paf.h \
imap_module.cc \
imap_module.h

# can't be linked dynamically yet
#if STATIC_INSPECTORS
noinst_LIBRARIES = libimap.a
libimap_a_SOURCES = $(file_list)
#else
#shlibdir = $(pkglibdir)/inspectors
#shlib_LTLIBRARIES = libimap.la
#libimap_la_CXXFLAGS = $(AM_CXXFLAGS) -DBUILDING_SO
#libimap_la_LDFLAGS = $(AM_LDFLAGS) -export-dynamic -shared
#libimap_la_SOURCES = $(file_list)
#endif

