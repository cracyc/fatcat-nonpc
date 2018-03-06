CC = g++

SOURCES = src/core/FatEntry.cpp src/core/FatFilename.cpp src/core/FatModule.cpp src/core/FatPath.cpp src/core/FatSystem.cpp src/core/FatDate.cpp src/table/FatBackup.cpp src/table/FatDiff.cpp src/analysis/FatExtract.cpp src/analysis/FatFix.cpp src/analysis/FatChain.cpp src/analysis/FatChains.cpp src/analysis/FatSearch.cpp src/analysis/FatWalk.cpp src/fatcat.cpp

OBJS = $(SOURCES:.cpp=.o)

INCLUDES = -Isrc -Ilibdsk/include

TARGET = fatcat

ifeq ($(OS),Windows_NT)
	CFLAGS = -DNOTWINDLL
	LDFLAGS = -static
else
	CFLAGS = 
	LDFLAGS = 
endif
	
LDFLAGS += -Llibdsk/lib/.libs 

LIBS = -ldsk -lz

RM = rm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

.cpp.o:
	$(CC) $(CFLAGS) -c $(INCLUDES) $< -o $@

clean:
	$(RM) $(TARGET) $(OBJS)

depend: $(SOURCES)
	makedepend $^

# DO NOT DELETE THIS LINE -- make depend needs it

