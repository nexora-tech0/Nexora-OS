/*
 * Nexora OS - user/include/hzarchive.h
 * hz::archive reads the archives Files can unpack (user/lib/hzrt/hzarchive.cpp): ZIP (stored and
 * deflated), tar (ustar, with GNU long names), gzip, and .tar.gz / .tgz built from the last two.
 *
 * The module carries its own DEFLATE decoder (RFC 1951) and CRC-32, so nothing here depends on a
 * compression library. Listing reads only the index (for ZIP, the central directory); Extract
 * recreates the tree under a destination folder, refuses entry names that would escape it, and
 * checks every stream against the CRC-32 the format records, so a damaged archive is reported
 * rather than quietly unpacked.
 *
 * Not supported, and reported as such: Zip64, encrypted entries, bzip2 and other methods, and
 * multi-disk archives.
 */
#ifndef HZARCHIVE_H
#define HZARCHIVE_H

#include "hzfile.h"

namespace hz::archive {

enum class Format : uint8_t { Unknown, Zip, Gzip, Tar };

struct Entry {
    String Name;             /* the path inside the archive, '\' separated */
    bool Directory = false;
    uint64_t Size = 0;       /* uncompressed; 0 for a gzip stream, whose size is only known when it is read */
    uint64_t Compressed = 0;
    uint32_t Crc = 0;
    bool HasCrc = false;
};

/* The format of a file, from its leading bytes (Unknown when it is not an archive we read). Reads
   the file, so Extract and List prefer the DetectBytes form internally. */
Format Detect(const String &Path);
const char *FormatName(Format Value);

/* True when the file name ends in an extension we can open (.zip, .tar, .gz, .tgz, .tar.gz). */
bool IsArchive(const String &FileName);
/* The file name without its archive extension, for naming the folder "Extract to X\" creates. */
String StripSuffix(const String &FileName);

/* The index: one Entry per member, in archive order. Nothing is written. */
STATUS List(const String &Path, Vector<Entry> &Out);

struct Result {
    int Files = 0;
    int Folders = 0;
    uint64_t Bytes = 0; /* the uncompressed size of the files written */
};

/* Unpacks everything into DestFolder (created, with its parents, when it does not exist). An
   existing file of the same name is replaced; an existing folder is kept. A member that fails to
   decode, or whose CRC-32 does not match, stops the whole extraction with the error. */
STATUS Extract(const String &Path, const String &DestFolder, Result *Out = nullptr);

}  // namespace hz::archive

#endif /* HZARCHIVE_H */
