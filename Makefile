CC = g++

SOURCES = src/core/FatEntry.cpp src/core/FatFilename.cpp src/core/FatModule.cpp src/core/FatPath.cpp src/core/FatSystem.cpp src/core/FatDate.cpp src/table/FatBackup.cpp src/table/FatDiff.cpp src/analysis/FatExtract.cpp src/analysis/FatFix.cpp src/analysis/FatChain.cpp src/analysis/FatChains.cpp src/analysis/FatSearch.cpp src/analysis/FatWalk.cpp src/fatcat.cpp

OBJS = $(SOURCES:.cpp=.o)

INCLUDES = -Isrc

TARGET = fatcat

LDFLAGS = -static

RM = rm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJS)

.cpp.o:
	$(CC) -c $(INCLUDES) $< -o $@

clean:
	$(RM) $(TARGET) $(OBJS)

depend: $(SOURCES)
	makedepend $^

# DO NOT DELETE THIS LINE -- make depend needs it

