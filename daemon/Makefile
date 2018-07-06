TARGET = dbsyncd
VERSION = 0.1.0

LIBS = -lhiredis -lcrypto

INCLUDEDIRS = -I/usr/local/include -I../common
LIBDIRS = -L/usr/local/lib

CXXSOURCES = $(wildcard *.c) $(wildcard ../common/*.c)
CXXOBJECTS = $(patsubst %.c,%.o,$(CXXSOURCES))
CXXDEPENDS = $(subst .c,.d,$(CXXSOURCES))
CXXFLAGS = $(INCLUDEDIRS) -Wall -DDSVERSION=\"$(VERSION)\"
CXX = gcc -fPIE

ifdef DEBUG
        CXXFLAGS += -g3 -DDSDEBUG
else
        CXXFLAGS += -O3
endif

LDFLAGS = $(LIBDIRS) $(LIBS)

all: $(TARGET)

$(TARGET): $(CXXOBJECTS)
	$(CXX) -o $@ $(CXXOBJECTS) $(LDFLAGS)

%.o: %.c
	$(CXX) $(CXXFLAGS) -o $@ -c $<

clean:
	$(RM) $(CXXOBJECTS) $(TARGET) $(CXXDEPENDS)


include $(CXXDEPENDS)

%.d: %.c
	$(CXX) -M $(CXXFLAGS) $< > $@.$$$$;                      \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@;     \
	rm -f $@.$$$$  
