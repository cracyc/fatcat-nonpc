#include <unistd.h>
#include <time.h>
#include <string>
#include <iostream>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <set>

#include <FatUtils.h>
#include "FatFilename.h"
#include "FatEntry.h"
#include "FatDate.h"
#include "FatSystem.h"

using namespace std;

/**
 * Opens the FAT resource
 */
FatSystem::FatSystem(string filename_, unsigned long long globalOffset_)
    : strange(0),
    filename(filename_),
    globalOffset(globalOffset_),
    totalSize(-1),
    totalSectors(-1),
    listDeleted(false),
    statsComputed(false),
    freeClusters(0),
    cacheEnabled(false),
    type(FAT32),
    rootEntries(0)
{
    dsk_err_t err = dsk_open(&fd, filename.c_str(), NULL, NULL);
    writeMode = false;

    if (err != DSK_ERR_OK) {
        ostringstream oss;
        oss << "! Unable to open the input file: " << filename << " for reading err:" << err;

        throw oss.str();
    }
    err = dsk_getgeom(fd, &geom);
    if (err != DSK_ERR_OK) {
        ostringstream oss;
        oss << "! Unable to find the input file: " << filename << " geometry err:" << err;

        throw oss.str();
    }
}

void FatSystem::enableCache()
{
    if (!cacheEnabled) {
        cout << "Computing FAT cache..." << endl;
        for (int cluster=0; cluster<totalClusters; cluster++) {
            cache[cluster] = nextCluster(cluster);
        }

        cacheEnabled = true;
    }
}

void FatSystem::enableWrite()
{
    writeMode = true;
}

FatSystem::~FatSystem()
{
    dsk_close(&fd);
}

/**
 * Reading some data
 */
vector<char> FatSystem::readData(unsigned long long address, int size)
{
    if (totalSectors != -1 && address+size > totalSectors) {
        cerr << "! Trying to read outside the disk" << endl;
    }

    vector<char> buf(size * geom.dg_secsize);
    for (int i = 0; i < size; i++)
    {
        dsk_err_t err = dsk_lread(fd, &geom, &buf[i * geom.dg_secsize], address + i);
        if (err != DSK_ERR_OK)
                cerr << "! Error reading sector " << address << endl;
    }

    return buf;
}

int FatSystem::writeData(unsigned long long address, const char *buffer, int size)
{
    if (!writeMode) {
        throw string("Trying to write data while write mode is disabled");
    }

    for (int i = 0; i < size; i++)
    {
        dsk_err_t err = dsk_lwrite(fd, &geom, &buffer[i * geom.dg_secsize], address + i);
        if (err != DSK_ERR_OK)
                cerr << "! Error writing sector " << address << endl;
    }

    return size;
}

/**
 * Parses FAT header
 */
void FatSystem::parseHeader()
{
    vector<char> sector = readData(0, 1);
    char* buffer = &sector[0];
    bytesPerSector = FAT_READ_SHORT(buffer, FAT_BYTES_PER_SECTOR)&0xffff;
    sectorsPerCluster = buffer[FAT_SECTORS_PER_CLUSTER]&0xff;
    reservedSectors = FAT_READ_SHORT(buffer, FAT_RESERVED_SECTORS)&0xffff;
    oemName = string(buffer+FAT_DISK_OEM, FAT_DISK_OEM_SIZE);
    fats = buffer[FAT_FATS];

    sectorsPerFat = FAT_READ_SHORT(buffer, FAT16_SECTORS_PER_FAT)&0xffff;

    if (sectorsPerFat != 0) {
        type = FAT16;
        bits = 16;
        diskLabel = string(buffer+FAT16_DISK_LABEL, FAT16_DISK_LABEL_SIZE);
        fsType = string(buffer+FAT16_DISK_FS, FAT16_DISK_FS_SIZE);
        rootEntries = FAT_READ_SHORT(buffer, FAT16_ROOT_ENTRIES)&0xffff;
        rootDirectory = 0;

        if (trim(fsType) == "FAT12") {
            bits = 12;
        }

        totalSectors = FAT_READ_SHORT(buffer, FAT16_TOTAL_SECTORS)&0xffff;
        if (!totalSectors) {
            totalSectors = FAT_READ_LONG(buffer, FAT_TOTAL_SECTORS)&0xffffffff;
        }
        if((totalSectors / sectorsPerCluster) < 0xff4) {
            bits = 12;
        }
    } else {
        type = FAT32;
        bits = 32;
        sectorsPerFat = FAT_READ_LONG(buffer, FAT_SECTORS_PER_FAT)&0xffffffff;
        totalSectors = FAT_READ_LONG(buffer, FAT_TOTAL_SECTORS)&0xffffffff;
        diskLabel = string(buffer+FAT_DISK_LABEL, FAT_DISK_LABEL_SIZE);
        rootDirectory = FAT_READ_LONG(buffer, FAT_ROOT_DIRECTORY)&0xffffffff;
        fsType = string(buffer+FAT_DISK_FS, FAT_DISK_FS_SIZE);
    }

    if (!((bytesPerSector == 256) || (bytesPerSector == 512) || (bytesPerSector == 1024))) {
        printf("WARNING: Bytes per sector is not 256, 512 or 1024 (%llu)\n", bytesPerSector);
        strange++;
    }

    if (bytesPerSector != geom.dg_secsize) {
        printf("WARNING: Bytes per sector mismatch (%llu) (%llu)\n", bytesPerSector, geom.dg_secsize);
        strange++;
    }

    if (sectorsPerCluster > 128) {
        printf("WARNING: Sectors per cluster high (%llu)\n", sectorsPerCluster);
        strange++;
    }

    if (fats != 2) {
        printf("WARNING: Fats number different of 2 (%llu)\n", fats);
        strange++;
    }

    if (rootDirectory != 2 && type == FAT32) {
        printf("WARNING: Root directory is not 2 (%llu)\n", rootDirectory);
        strange++;
    }
}

/**
 * Returns the 32-bit fat value for the given cluster number
 */
unsigned int FatSystem::nextCluster(unsigned int cluster, int fat)
{
    int bytes = (bits == 32 ? 4 : 2);

    if (!validCluster(cluster)) {
        return 0;
    }

    if (cacheEnabled) {
        return cache[cluster];
    }

    int offset = (float)(cluster * bytes) * (bits == 12 ? 12.0/16.0 : 1.0);

    vector<char> sector = readData(fatStart+((fatSize*fat+offset)/bytesPerSector), bits == 12 ? 2 : 1);
    char* buffer = &sector[offset%bytesPerSector];

    unsigned int next;

    if (type == FAT32) {
        next = FAT_READ_LONG(buffer, 0)&0x0fffffff;

        if (next >= 0x0ffffff0) {
            return FAT_LAST;
        } else {
            return next;
        }
    } else {
        next = FAT_READ_SHORT(buffer,0)&0xffff;

        if (bits == 12) {
            int bit = cluster*bits;
            if (bit%8 != 0) {
                next = next >> 4;
            }
            next &= 0xfff;
            if (next >= 0xff0) {
                return FAT_LAST;
            } else {
                return next;
            }
        } else {
            if (next >= 0xfff0) {
                return FAT_LAST;
            } else {
                return next;
            }
        }
    }
}

/**
 * Changes the next cluster in a file
 */
bool FatSystem::writeNextCluster(unsigned int cluster, unsigned int next, int fat)
{
    int bytes = (bits == 32 ? 4 : 2);

    if (!validCluster(cluster)) {
        throw string("Trying to access a cluster outside bounds");
    }

    int offset = (float)(cluster * bytes) * (bits == 12 ? 12.0/16.0 : 1.0);
    int address = fatStart+((fatSize*fat+offset)/bytesPerSector);

    vector<char> sector = readData(address, bits == 12 ? 2 : 1);
    char* buffer = &sector[offset%bytesPerSector];

    if (bits == 12) {
        int bit = cluster*bits;

        if (bit%8 != 0) {
            buffer[0] = ((next&0x0f)<<4)|(buffer[0]&0x0f);
            buffer[1] = (next>>4)&0xff;
        } else {
            buffer[0] = next&0xff;
            buffer[1] = (buffer[1]&0xf0)|((next>>8)&0x0f);
        }
    } else {
        for (int i=0; i<(bits/8); i++) {
            buffer[i] = (next>>(8*i))&0xff;
        }
    }

    return writeData(address, &sector[0], bits == 12 ? 2 : 1) != 0;
}

bool FatSystem::validCluster(unsigned int cluster)
{
    return cluster < totalClusters;
}

unsigned long long FatSystem::clusterAddress(unsigned int cluster, bool isRoot)
{
    if (type == FAT32 || !isRoot) {
        cluster -= 2;
    }

    unsigned long long addr = (dataStart + sectorsPerCluster*cluster);

    if (type == FAT16 && !isRoot) {
        addr += (rootEntries * FAT_ENTRY_SIZE) / bytesPerSector;
    }

    return addr;
}

vector<FatEntry> FatSystem::getEntries(unsigned int cluster, int *clusters, bool *hasFree)
{
    bool isRoot = false;
    bool contiguous = false;
    int foundEntries = 0;
    int badEntries = 0;
    bool isValid = false;
    set<unsigned int> visited;
    vector<FatEntry> entries;
    FatFilename filename;

    if (clusters != NULL) {
        *clusters = 0;
    }

    if (hasFree != NULL) {
        *hasFree = false;
    }

    if (cluster == 0 && type == FAT32) {
        cluster = rootDirectory;
    }

    isRoot = (type==FAT16 && cluster==rootDirectory);

    if (cluster == rootDirectory) {
        isValid = true;
    }

    if (!validCluster(cluster)) {
        return vector<FatEntry>();
    }

    do {
        bool localZero = false;
        int localFound = 0;
        int localBadEntries = 0;
        int sectors = isRoot ? rootSectors : sectorsPerCluster;
        unsigned long long address = clusterAddress(cluster, isRoot);
        if (visited.find(cluster) != visited.end()) {
            cerr << "! Looping directory" << endl;
            break;
        }
        visited.insert(cluster);

        unsigned int i, j;
        
        for (j=0; j<sectors; j++) {
            vector<char> sector = readData(address+j, 1);
            for (i=0; i<bytesPerSector; i+=FAT_ENTRY_SIZE) {
                char* buffer = &sector[i];

                // Creating entry
                FatEntry entry;

                entry.attributes = buffer[FAT_ATTRIBUTES];
                entry.sector = address+j;
                entry.offset = i;

                if (entry.attributes == FAT_ATTRIBUTES_LONGFILE) {
                    // Long file part
                    filename.append(buffer);
                } else {
                    entry.shortName = string(buffer, 11);
                    entry.longName = filename.getFilename();
                    entry.size = FAT_READ_LONG(buffer, FAT_FILESIZE)&0xffffffff;
                    entry.cluster = (FAT_READ_SHORT(buffer, FAT_CLUSTER_LOW)&0xffff) | (FAT_READ_SHORT(buffer, FAT_CLUSTER_HIGH)<<16);
                    entry.setData(string(buffer, sizeof(buffer)));

                    if (!entry.isZero()) {
                        if (entry.isCorrect() && validCluster(entry.cluster)) {
                            entry.creationDate = FatDate(&buffer[FAT_CREATION_DATE]);
                            entry.changeDate = FatDate(&buffer[FAT_CHANGE_DATE]);
                            entries.push_back(entry);
                            localFound++;
                            foundEntries++;

                            if (!isValid && entry.getFilename() == "." && entry.cluster == cluster) {
                                isValid = true;
                            }
                        } else {
                            localBadEntries++;
                            badEntries++;
                        }

                        localZero = false;
                    } else {
                        localZero = true;
                    }
                }
            }
        }

        int previousCluster = cluster;

        if (isRoot) {
            cluster = FAT_LAST;
        } else {
            cluster = nextCluster(cluster);
        }

        if (clusters) {
            (*clusters)++;
        }

        if (cluster == 0 || contiguous) {
            contiguous = true;

            if (hasFree != NULL) {
                *hasFree = true;
            }
            if (!localZero && localFound && localBadEntries<localFound) {
                cluster = previousCluster+1;
            } else {
                if (!localFound && clusters) {
                    (*clusters)--;
                }
                break;
            }
        }

        if (!isValid) {
            if (badEntries>foundEntries) {
                cerr << "! Entries don't look good, this is maybe not a directory" << endl;
                return vector<FatEntry>();
          }
        }
    } while (cluster != FAT_LAST);

    return entries;
}

void FatSystem::list(FatPath &path)
{
    FatEntry entry;

    if (findDirectory(path, entry)) {
        list(entry.cluster);
    }
}

void FatSystem::list(unsigned int cluster)
{
    bool hasFree = false;
    vector<FatEntry> entries = getEntries(cluster, NULL, &hasFree);
    printf("Directory cluster: %u\n", cluster);
    if (hasFree) {
        printf("Warning: this directory has free clusters that was read contiguously\n");
    }
    list(entries);
}

void FatSystem::list(vector<FatEntry> &entries)
{
    vector<FatEntry>::iterator it;

    for (it=entries.begin(); it!=entries.end(); it++) {
        FatEntry &entry = *it;

        if (entry.isErased() && !listDeleted) {
            continue;
        }

        if (entry.isDirectory()) {
            printf("d");
        } else {
            printf("f");
        }

        string name = entry.getFilename();
        if (entry.isDirectory()) {
            name += "/";
        }

        printf(" %s ", entry.changeDate.pretty().c_str());
        printf(" %-30s", name.c_str());

        printf(" c=%u", entry.cluster);

        if (!entry.isDirectory()) {
            string pretty = prettySize(entry.size);
            printf(" s=%llu (%s)", entry.size, pretty.c_str());
        }

        if (entry.isHidden()) {
            printf(" h");
        }
        if (entry.isErased()) {
            printf(" d");
        }

        printf("\n");
        fflush(stdout);
    }
}

void FatSystem::readFile(unsigned int cluster, unsigned int size, FILE *f, bool deleted)
{
    bool contiguous = deleted;

    if (f == NULL) {
        f = stdout;
    }
#ifdef WIN32
    _setmode(_fileno(f), _O_BINARY);
#endif
    while ((size!=0) && cluster!=FAT_LAST) {
        int currentCluster = cluster;
        int toRead = size;
        if (toRead > (bytesPerSector*sectorsPerCluster) || size < 0) {
            toRead = bytesPerSector*sectorsPerCluster;
        }
        vector<char> buffer = readData(clusterAddress(cluster), sectorsPerCluster);

        if (size != -1) {
            size -= toRead;
        }

        // Write file data to the given file
        fwrite(&buffer[0], toRead, 1, f);
        fflush(f);

        if (contiguous) {
            if (deleted) {
                do {
                    cluster++;
                } while (!freeCluster(cluster));
            } else {
                if (!freeCluster(cluster)) {
                    fprintf(stderr, "! Contiguous file contains cluster that seems allocated\n");
                    fprintf(stderr, "! Trying to disable contiguous mode\n");
                    contiguous = false;
                    cluster = nextCluster(cluster);
                } else {
                    cluster++;
                }
            }
        } else {
            cluster = nextCluster(currentCluster);

            if (cluster == 0) {
                fprintf(stderr, "! One of your file's cluster is 0 (maybe FAT is broken, have a look to -2 and -m)\n");
                fprintf(stderr, "! Trying to enable contigous mode\n");
                contiguous = true;
                cluster = currentCluster+1;
            }
        }
    }
}

bool FatSystem::init()
{
    // Parsing header
    parseHeader();

    // Computing values
    fatStart = reservedSectors;
    dataStart = fatStart + fats*sectorsPerFat;
    totalSize = totalSectors*bytesPerSector;
    fatSize = sectorsPerFat*bytesPerSector;
    totalClusters = (fatSize*8)/bits;
    dataSize = totalClusters*bytesPerSector*sectorsPerCluster;

    if (type == FAT16) {
        rootSectors = rootEntries*32/bytesPerSector;
    }

    return strange == 0;
}

void FatSystem::infos()
{
    int bytesPerCluster = sectorsPerCluster*bytesPerSector;
    cout << "FAT Filesystem information" << endl << endl;

    cout << "Filesystem type: " << fsType << endl;
    cout << "OEM name: " << oemName << endl;
    cout << "Total sectors: " << totalSectors << endl;
    cout << "Total data clusters: " << totalClusters << endl;
    cout << "Data size: " << dataSize << " (" << prettySize(dataSize) << ")" << endl;
    cout << "Disk size: " << totalSize << " (" << prettySize(totalSize) << ")" << endl;
    cout << "Bytes per sector: " << bytesPerSector << endl;
    cout << "Sectors per cluster: " << sectorsPerCluster << endl;
    cout << "Reserved sectors: " << reservedSectors << endl;
    if (type == FAT16) {
        cout << "Root entries: " << rootEntries << endl;
        cout << "Root sectors: " << rootSectors << endl;
    }
    cout << "Sectors per FAT: " << sectorsPerFat << endl;
    cout << "Fat size: " << fatSize << " (" << prettySize(fatSize) << ")" << endl;
    printf("FAT1 start sector: %llx\n", fatStart);
    printf("FAT2 start sector: %llx\n", fatStart+(fatSize/bytesPerSector));
    printf("Data start sector: %llx\n", dataStart);
    cout << "Root directory cluster: " << rootDirectory << endl;
    cout << "Disk label: " << diskLabel << endl;
    cout << endl;

    computeStats();
    cout << "Free clusters: " << freeClusters << "/" << totalClusters;
    cout << " (" << (100*freeClusters/(double)totalClusters) << "%)" << endl;
    cout << "Free space: " << (freeClusters*bytesPerCluster) <<
        " (" << prettySize(freeClusters*bytesPerCluster) << ")" << endl;
    cout << "Used space: " << ((totalClusters-freeClusters)*bytesPerCluster) <<
        " (" << prettySize((totalClusters-freeClusters)*bytesPerCluster) << ")" << endl;
    cout << endl;
}

bool FatSystem::findDirectory(FatPath &path, FatEntry &outputEntry)
{
    int cluster;
    vector<string> parts = path.getParts();
    cluster = rootDirectory;
    outputEntry.cluster = cluster;

    for (int i=0; i<parts.size(); i++) {
        if (parts[i] != "") {
            parts[i] = strtolower(parts[i]);
            vector<FatEntry> entries = getEntries(cluster);
            vector<FatEntry>::iterator it;
            bool found = false;

            for (it=entries.begin(); it!=entries.end(); it++) {
                FatEntry &entry = *it;
                string name = entry.getFilename();
                if (entry.isDirectory() && strtolower(name) == parts[i]) {
                    outputEntry = entry;
                    cluster = entry.cluster;
                    found = true;
                }
            }

            if (!found) {
                cerr << "Error: directory " << path.getPath() << " not found" << endl;
                return false;
            }
        }
    }

    return true;
}

bool FatSystem::findFile(FatPath &path, FatEntry &outputEntry)
{
    bool found = false;
    string dirname = path.getDirname();
    string basename = path.getBasename();
    basename = strtolower(basename);

    FatPath parent(dirname);
    FatEntry parentEntry;
    if (findDirectory(parent, parentEntry)) {
        vector<FatEntry> entries = getEntries(parentEntry.cluster);
        vector<FatEntry>::iterator it;

        for (it=entries.begin(); it!=entries.end(); it++) {
            FatEntry &entry = (*it);
            if (strtolower(entry.getFilename()) == basename) {
                outputEntry = entry;

                if (entry.size == 0) {
                    found = true;
                } else {
                    return true;
                }
            }
        }
    }

    return found;
}

void FatSystem::readFile(FatPath &path, FILE *f)
{
    FatEntry entry;

    if (findFile(path, entry)) {
        bool contiguous = false;
        if (entry.isErased() && freeCluster(entry.cluster)) {
            fprintf(stderr, "! Trying to read a deleted file, enabling deleted mode\n");
            contiguous = true;
        }
        readFile(entry.cluster, entry.size, f, contiguous);
    }
    else
            fprintf(stderr, "! File not found\n");
}

void FatSystem::setListDeleted(bool listDeleted_)
{
    listDeleted = listDeleted_;
}

FatEntry FatSystem::rootEntry()
{
    FatEntry entry;
    entry.longName = "/";
    entry.attributes = FAT_ATTRIBUTES_DIR;
    entry.cluster = rootDirectory;

    return entry;
}

bool FatSystem::freeCluster(unsigned int cluster)
{
    return nextCluster(cluster) == 0;
}

void FatSystem::computeStats()
{
    if (statsComputed) {
        return;
    }

    statsComputed = true;

    freeClusters = 0;
    for (unsigned int cluster=0; cluster<totalClusters; cluster++) {
        if (freeCluster(cluster)) {
            freeClusters++;
        }
    }
}

void FatSystem::rewriteUnallocated(bool random)
{
    int total = 0;
    srand(time(NULL));
    for (int cluster=0; cluster<totalClusters; cluster++) {
        if (freeCluster(cluster)) {
            char buffer[bytesPerSector*sectorsPerCluster];
            for (int i=0; i<sizeof(buffer); i++) {
                if (random) {
                    buffer[i] = rand()&0xff;
                } else {
                    buffer[i] = 0x0;
                }
            }
            writeData(clusterAddress(cluster), buffer, sectorsPerCluster);
            total++;
        }
    }

    cout << "Scrambled " << total << " sectors" << endl;
}
