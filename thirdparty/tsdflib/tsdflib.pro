TEMPLATE = lib

CONFIG += staticlib create_prl
CONFIG -= qt

include (../../options.pri)

INCLUDEPATH += ../graphicsio
INCLUDEPATH += ../gdbmp
INCLUDEPATH += ../cutil
INCLUDEPATH += ../fi

SOURCES += container.cc \
           fidset.cc \
           fslist.cc \
           gdbmkeys.cc \
           giofidset.cc \
           myfile.c 