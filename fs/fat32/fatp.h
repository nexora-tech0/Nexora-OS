/*
 * Nexora OS - fs/fat32/fatp.h
 * FAT32 driver internals shared by the files in fs/fat32/.
 *
 * Locking: every operation on a volume holds its mutex (FAT_VOLUME.Lock) for its whole duration;
 * the I/O manager's per-file-object lock is taken before it (lock order: file -> volume).
 * Metadata is written through immediately (FAT entries to every FAT copy, directory entries);
 * the FSInfo free-cluster hint is written at the end of each modifying operation.
 */
#ifndef FATP_H
#define FATP_H

#include "ex.h"
#include "io.h"
#include "ke.h"
#include "rtl.h"

#define FAT_TAG HZ_POOL_TAG('F', 'a', 't', '3')

#define FAT_ENTRY_MASK 0x0FFFFFFFu
#define FAT_EOC_MIN 0x0FFFFFF8u
#define FAT_EOC 0x0FFFFFFFu
#define FAT_BAD 0x0FFFFFF7u

#define FAT_ATTR_READONLY 0x01
#define FAT_ATTR_HIDDEN 0x02
#define FAT_ATTR_SYSTEM 0x04
#define FAT_ATTR_VOLUME 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE 0x20
#define FAT_ATTR_LFN 0x0F

#define FAT_CASE_LOWER_BASE 0x08
#define FAT_CASE_LOWER_EXT 0x10

#define FAT_DIRENT_SIZE 32
#define FAT_MAX_DIRECTORY_BYTES (65536u * FAT_DIRENT_SIZE)
#define FAT_MAX_NAME 255

typedef struct HZ_PACKED _FAT_DIRENT {
    uint8_t Name[11];
    uint8_t Attributes;
    uint8_t NtCase;
    uint8_t CreateTenths;
    uint16_t CreateTime;
    uint16_t CreateDate;
    uint16_t AccessDate;
    uint16_t ClusterHigh;
    uint16_t WriteTime;
    uint16_t WriteDate;
    uint16_t ClusterLow;
    uint32_t Size;
} FAT_DIRENT;

typedef struct HZ_PACKED _FAT_LFN_ENTRY {
    uint8_t Order;
    uint16_t Name1[5];
    uint8_t Attributes;
    uint8_t Type;
    uint8_t Checksum;
    uint16_t Name2[6];
    uint16_t Cluster;
    uint16_t Name3[2];
} FAT_LFN_ENTRY;

_Static_assert(sizeof(FAT_DIRENT) == 32 && sizeof(FAT_LFN_ENTRY) == 32, "FAT directory entries are 32 bytes");

/* Position in a cluster chain: Index-th cluster of the chain starting at First is Cluster. */
typedef struct _FAT_CURSOR {
    uint32_t First;
    uint32_t Index;
    uint32_t Cluster; /* 0 = not positioned */
} FAT_CURSOR;

typedef struct _FAT_FCB {
    LIST_ENTRY Link;
    uint32_t References;
    uint32_t ParentCluster;  /* first cluster of the parent directory (0 for the root itself) */
    uint32_t EntryOffset;    /* byte offset of the short entry in the parent */
    uint32_t FirstOffset;    /* first long-name entry (== EntryOffset without one) */
    uint32_t FirstCluster;   /* 0 = no data yet */
    uint32_t Size;
    uint32_t Allocated;      /* clusters in the chain, UINT32_MAX = not counted yet */
    bool IsRoot;
    bool IsDirectory;
    bool DeletePending;
    FAT_DIRENT Entry;        /* copy of the short entry (times, attributes) */
    FAT_CURSOR Cursor;       /* data chain position cache */
    char Name[FAT_MAX_NAME + 1];
} FAT_FCB;

typedef struct _FAT_CCB {
    uint32_t DirectoryOffset; /* enumeration cursor */
    bool DeleteOnClose;
} FAT_CCB;

typedef struct _FAT_VOLUME {
    DEVICE_OBJECT *Storage;
    KMUTEX Lock;
    bool ReadOnly;
    uint32_t BytesPerSector;
    uint32_t SectorsPerCluster;
    uint32_t BytesPerCluster;
    uint32_t ReservedSectors;
    uint32_t FatCount;
    uint32_t SectorsPerFat;
    uint32_t ActiveFat;      /* UINT32_MAX = all FATs mirrored */
    uint32_t RootCluster;
    uint32_t FsInfoSector;
    uint64_t FirstDataSector;
    uint32_t ClusterCount;   /* data clusters: valid numbers are 2 .. ClusterCount + 1 */
    uint32_t FreeClusters;
    uint32_t NextFree;
    bool FsInfoDirty;
    uint32_t Serial;
    char Label[12];
    uint8_t *FatCache;       /* one sector of the (first or active) FAT */
    uint64_t FatCacheLba;
    uint32_t FatCacheIndex;  /* its sector number within the FAT */
    bool FatCacheDirty;      /* changed since written to every FAT copy (M11: write-back) */
    uint8_t *MetaCache;      /* one directory sector */
    uint64_t MetaCacheLba;
    uint8_t *SectorBuffer;   /* scratch for partial-sector data transfers */
    uint8_t *ClusterBuffer;  /* zeroed cluster for directory growth */
    LIST_ENTRY Fcbs;
    FAT_FCB *Root;
} FAT_VOLUME;

/* A directory entry as found by FatReadDirectory. */
typedef struct _FAT_DIR_ITEM {
    FAT_DIRENT Entry;
    uint32_t EntryOffset;
    uint32_t FirstOffset;
    char Name[FAT_MAX_NAME + 1];
    char ShortName[13];
} FAT_DIR_ITEM;

/* fs/fat32/fattable.c: sectors, the FAT, chains */
STATUS FatReadMeta(FAT_VOLUME *Vol, uint64_t Lba, uint8_t **Sector);
STATUS FatWriteMeta(FAT_VOLUME *Vol, uint64_t Lba);
static inline uint64_t FatClusterToSector(FAT_VOLUME *Vol, uint32_t Cluster)
{
    return Vol->FirstDataSector + (uint64_t)(Cluster - 2) * Vol->SectorsPerCluster;
}
static inline bool FatIsValidCluster(FAT_VOLUME *Vol, uint32_t Cluster)
{
    return Cluster >= 2 && Cluster <= Vol->ClusterCount + 1;
}
STATUS FatGetEntry(FAT_VOLUME *Vol, uint32_t Cluster, uint32_t *Value);
STATUS FatSetEntry(FAT_VOLUME *Vol, uint32_t Cluster, uint32_t Value);
STATUS FatSeek(FAT_VOLUME *Vol, FAT_CURSOR *Cursor, uint32_t Index, uint32_t *Cluster);
STATUS FatAllocateCluster(FAT_VOLUME *Vol, uint32_t Previous, bool Zero, uint32_t *Cluster);
STATUS FatFreeChain(FAT_VOLUME *Vol, uint32_t First);
STATUS FatCountChain(FAT_VOLUME *Vol, uint32_t First, uint32_t *Count);
STATUS FatCountFreeClusters(FAT_VOLUME *Vol, uint32_t *Free);
/* Writes the cached FAT sector (if changed) to every FAT copy, then the FSInfo hint; called at
   the end of every modifying operation, so the volume is consistent between operations. */
STATUS FatFlushFsInfo(FAT_VOLUME *Vol);

/* fs/fat32/fatdir.c: directories and names */
STATUS FatReadDirEntry(FAT_VOLUME *Vol, FAT_CURSOR *Dir, uint32_t Offset, void *Entry);
STATUS FatWriteDirEntry(FAT_VOLUME *Vol, FAT_CURSOR *Dir, uint32_t Offset, const void *Entry);
STATUS FatReadDirectory(FAT_VOLUME *Vol, FAT_CURSOR *Dir, uint32_t *Offset, FAT_DIR_ITEM *Item);
STATUS FatLookup(FAT_VOLUME *Vol, uint32_t DirCluster, const char *Name, FAT_DIR_ITEM *Item);
bool FatIsValidLongName(const char *Name);
STATUS FatInsertEntries(FAT_VOLUME *Vol, uint32_t DirCluster, const char *Name, FAT_DIRENT *Short,
                        uint32_t *FirstOffset, uint32_t *EntryOffset);
STATUS FatMarkDeleted(FAT_VOLUME *Vol, uint32_t DirCluster, uint32_t FirstOffset, uint32_t EntryOffset);
STATUS FatIsDirectoryEmpty(FAT_VOLUME *Vol, uint32_t DirCluster, bool *Empty);
void FatEncodeTime(uint64_t Time, uint16_t *Date, uint16_t *Clock, uint8_t *Tenths);
uint64_t FatDecodeTime(uint16_t Date, uint16_t Clock, uint8_t Tenths);
static inline uint32_t FatEntryCluster(const FAT_DIRENT *E)
{
    return ((uint32_t)E->ClusterHigh << 16) | E->ClusterLow;
}
static inline void FatSetEntryCluster(FAT_DIRENT *E, uint32_t Cluster)
{
    E->ClusterHigh = (uint16_t)(Cluster >> 16);
    E->ClusterLow = (uint16_t)Cluster;
}

#endif /* FATP_H */
