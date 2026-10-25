/*
 * Nexora OS - user/lib/hzrt/hzarchive.cpp
 * The archive reader behind user/include/hzarchive.h. The DEFLATE decoder follows RFC 1951
 * directly (fixed and dynamic Huffman blocks plus stored blocks), the CRC-32 is the ZIP/gzip
 * polynomial 0xEDB88320, and each container is parsed from its own specification: ZIP from the
 * end-of-central-directory record, gzip from its header and trailer, tar from the 512-byte
 * headers. Every stream is bounded, so a damaged archive cannot exhaust memory.
 */
#include "hzarchive.h"

namespace hz::archive {
namespace {

constexpr size_t kMaxArchiveBytes = 256u * 1024 * 1024; /* the file we open */
constexpr size_t kMaxEntryBytes = 256u * 1024 * 1024;   /* one member, uncompressed */

/* ---- CRC-32 ----------------------------------------------------------------------------------- */

uint32_t g_CrcTable[256];
bool g_CrcReady;

void MakeCrcTable()
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t C = n;
        for (int k = 0; k < 8; k++)
            C = (C & 1) ? (0xEDB88320u ^ (C >> 1)) : (C >> 1);
        g_CrcTable[n] = C;
    }
    g_CrcReady = true;
}

uint32_t Crc32(uint32_t Seed, const uint8_t *Data, size_t Length)
{
    if (!g_CrcReady)
        MakeCrcTable();
    uint32_t C = Seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < Length; i++)
        C = g_CrcTable[(C ^ Data[i]) & 0xFFu] ^ (C >> 8);
    return C ^ 0xFFFFFFFFu;
}

uint16_t Le16(const uint8_t *P)
{
    return (uint16_t)((uint16_t)P[0] | ((uint16_t)P[1] << 8));
}

uint32_t Le32(const uint8_t *P)
{
    return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
}

/* ---- DEFLATE (RFC 1951) ------------------------------------------------------------------------ */

struct BitReader {
    const uint8_t *In;
    size_t Length;
    size_t Position;
    uint32_t Bits;
    int32_t Count;
    int32_t Overread; /* bits that had to come from the padding at the very end */
};

/* The low Need bits of the stream. Past the end the stream is zero padded, and Overread counts it,
   because a valid stream is padded to at most seven bits. */
uint32_t BitsPeek(BitReader &R, int32_t Need)
{
    while (R.Count < Need) {
        if (R.Position >= R.Length) {
            R.Overread++;
            R.Count++;
            continue;
        }
        R.Bits |= (uint32_t)R.In[R.Position++] << R.Count;
        R.Count += 8;
    }
    return R.Bits & ((1u << Need) - 1u);
}

void BitsDrop(BitReader &R, int32_t Need)
{
    R.Bits >>= Need;
    R.Count -= Need;
}

struct Huffman {
    uint16_t Count[16]; /* the number of codes of each length */
    uint16_t Symbol[288];
};

void BuildHuffman(Huffman &H, const uint8_t *Lengths, int32_t Total)
{
    for (int i = 0; i < 16; i++)
        H.Count[i] = 0;
    for (int32_t i = 0; i < Total; i++)
        H.Count[Lengths[i]]++;
    H.Count[0] = 0;
    uint16_t Offsets[16] = {};
    uint16_t Sum = 0;
    for (int Len = 1; Len < 16; Len++) {
        Offsets[Len] = Sum;
        Sum = (uint16_t)(Sum + H.Count[Len]);
    }
    for (int32_t i = 0; i < Total; i++) {
        if (Lengths[i])
            H.Symbol[Offsets[Lengths[i]]++] = (uint16_t)i;
    }
}

/* Walks one canonical code, most significant bit first. Returns the symbol, or -1. */
int32_t DecodeSymbol(BitReader &R, const Huffman &H)
{
    int32_t Code = 0, First = 0, Index = 0;
    for (int Len = 1; Len <= 15; Len++) {
        Code |= (int32_t)BitsPeek(R, 1);
        BitsDrop(R, 1);
        int32_t Count = H.Count[Len];
        if (Code - Count < First)
            return H.Symbol[Index + (Code - First)];
        Index += Count;
        First = (First + Count) << 1;
        Code <<= 1;
    }
    return -1;
}

const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kCodeOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

bool PushByte(Vector<uint8_t> &Out, uint8_t Value)
{
    if (Out.Size() >= kMaxEntryBytes)
        return false;
    Out.Push(Value);
    return true;
}

/* Decodes one DEFLATE stream into Out; Expected is the size the container promises (0 = unknown).
   Out is not cleared, so a caller can append. */
bool InflateRaw(BitReader &R, Vector<uint8_t> &Out, size_t Expected)
{
    static uint8_t CodeLengths[320];
    static uint8_t Fixed[288];
    static uint8_t FixedDistance[30];
    Huffman Lengths, Distances;
    bool FixedReady = false;

    for (;;) {
        int32_t Final = (int32_t)BitsPeek(R, 1);
        BitsDrop(R, 1);
        int32_t Type = (int32_t)BitsPeek(R, 2);
        BitsDrop(R, 2);
        if (Type == 0) {
            R.Bits = 0; /* a stored block starts on the next byte boundary */
            R.Count = 0;
            if (R.Position + 4 > R.Length)
                return false;
            uint16_t Length = Le16(R.In + R.Position);
            uint16_t Complement = Le16(R.In + R.Position + 2);
            R.Position += 4;
            if ((uint16_t)(~Length) != Complement || R.Position + Length > R.Length)
                return false;
            for (uint16_t i = 0; i < Length; i++)
                if (!PushByte(Out, R.In[R.Position + i]))
                    return false;
            R.Position += Length;
        } else if (Type == 1 || Type == 2) {
            if (Type == 1) {
                if (!FixedReady) {
                    for (int i = 0; i < 144; i++)
                        Fixed[i] = 8;
                    for (int i = 144; i < 256; i++)
                        Fixed[i] = 9;
                    for (int i = 256; i < 280; i++)
                        Fixed[i] = 7;
                    for (int i = 280; i < 288; i++)
                        Fixed[i] = 8;
                    for (int i = 0; i < 30; i++)
                        FixedDistance[i] = 5;
                    FixedReady = true;
                }
                BuildHuffman(Lengths, Fixed, 288);
                BuildHuffman(Distances, FixedDistance, 30);
            } else {
                int32_t Literals = (int32_t)BitsPeek(R, 5) + 257;
                int32_t DistancesCount = (int32_t)BitsPeek(R, 5) + 1;
                int32_t MetaCount = (int32_t)BitsPeek(R, 4) + 4;
                BitsDrop(R, 14);
                uint8_t Meta[19] = {};
                for (int32_t i = 0; i < MetaCount; i++) {
                    Meta[kCodeOrder[i]] = (uint8_t)BitsPeek(R, 3);
                    BitsDrop(R, 3);
                }
                Huffman MetaTree;
                BuildHuffman(MetaTree, Meta, 19);
                int32_t Total = Literals + DistancesCount;
                if (Total > 320 || Literals > 288 || DistancesCount > 32)
                    return false;
                int32_t Index = 0;
                while (Index < Total) {
                    int32_t Symbol = DecodeSymbol(R, MetaTree);
                    if (Symbol < 0)
                        return false;
                    if (Symbol < 16) {
                        CodeLengths[Index++] = (uint8_t)Symbol;
                        continue;
                    }
                    int32_t Repeat = 0, Value = 0;
                    if (Symbol == 16) {
                        if (Index == 0)
                            return false;
                        Value = CodeLengths[Index - 1];
                        Repeat = 3 + (int32_t)BitsPeek(R, 2);
                        BitsDrop(R, 2);
                    } else if (Symbol == 17) {
                        Repeat = 3 + (int32_t)BitsPeek(R, 3);
                        BitsDrop(R, 3);
                    } else {
                        Repeat = 11 + (int32_t)BitsPeek(R, 7);
                        BitsDrop(R, 7);
                    }
                    if (Index + Repeat > Total)
                        return false;
                    while (Repeat-- > 0)
                        CodeLengths[Index++] = (uint8_t)Value;
                }
                BuildHuffman(Lengths, CodeLengths, Literals);
                BuildHuffman(Distances, CodeLengths + Literals, DistancesCount);
            }
            for (;;) {
                int32_t Symbol = DecodeSymbol(R, Lengths);
                if (Symbol < 0)
                    return false;
                if (Symbol < 256) {
                    if (!PushByte(Out, (uint8_t)Symbol))
                        return false;
                    continue;
                }
                if (Symbol == 256)
                    break;
                Symbol -= 257;
                if (Symbol >= 29)
                    return false;
                size_t Length = (size_t)kLengthBase[Symbol] + (size_t)BitsPeek(R, kLengthExtra[Symbol]);
                BitsDrop(R, kLengthExtra[Symbol]);
                int32_t DistanceSymbol = DecodeSymbol(R, Distances);
                if (DistanceSymbol < 0 || DistanceSymbol >= 30)
                    return false;
                size_t Distance =
                    (size_t)kDistBase[DistanceSymbol] + (size_t)BitsPeek(R, kDistExtra[DistanceSymbol]);
                BitsDrop(R, kDistExtra[DistanceSymbol]);
                if (Distance == 0 || Distance > Out.Size())
                    return false;
                for (size_t i = 0; i < Length; i++)
                    if (!PushByte(Out, Out[Out.Size() - Distance]))
                        return false;
            }
        } else {
            return false; /* the reserved block type */
        }
        if (Final)
            break;
    }
    if (R.Overread > 7)
        return false;
    return Expected == 0 || Out.Size() == Expected;
}

/* ---- Names ------------------------------------------------------------------------------------ */

bool EndsWithNoCase(const char *Text, const char *Suffix)
{
    size_t A = strlen(Text), B = strlen(Suffix);
    if (B == 0 || B > A)
        return false;
    return hz::fs::EqualNames(Text + (A - B), Suffix);
}

/* Turns an archive member name into a relative path with '\', dropping "." components; false when
   the name is absolute or climbs out with "..", which would write outside the destination. */
bool SafeRelative(const String &Name, String &Out)
{
    if (Name.Empty() || Name[0] == '/' || Name[0] == '\\')
        return false;
    if (Name.Length() >= 2 && Name[1] == ':')
        return false;
    String Result;
    size_t Start = 0;
    for (size_t i = 0; i <= Name.Length(); i++) {
        if (i < Name.Length() && Name[i] != '/' && Name[i] != '\\')
            continue;
        size_t Part = i - Start;
        if (Part > 0) {
            const char *P = Name.CStr() + Start;
            if (Part == 1 && P[0] == '.') {
                /* a "." component is dropped */
            } else if (Part == 2 && P[0] == '.' && P[1] == '.') {
                return false;
            } else {
                if (!Result.Empty())
                    Result.Append("\\", 1);
                Result.Append(P, Part);
            }
        }
        Start = i + 1;
    }
    Out = Move(Result);
    return !Out.Empty();
}

/* ---- Folders ---------------------------------------------------------------------------------- */

STATUS MakeFolders(const String &Folder)
{
    if (Folder.Length() < 3 || Folder[1] != ':')
        return STATUS_INVALID_PARAMETER;
    String Current(Folder.CStr(), 2); /* "C:" */
    size_t Start = 3;
    for (size_t i = 2; i <= Folder.Length(); i++) {
        if (i < Folder.Length() && !(i > 2 && Folder[i] == '\\'))
            continue;
        if (i > Start) {
            String Part(Folder.CStr() + Start, i - Start);
            Current.Append("\\", 1);
            Current.Append(Part.CStr(), Part.Length());
            bool Directory = false;
            if (hz::fs::Exists(Current, &Directory)) {
                if (!Directory)
                    return STATUS_OBJECT_NAME_COLLISION;
            } else {
                STATUS Status = hz::fs::CreateDirectory(Current);
                if (!HZ_SUCCESS(Status))
                    return Status;
            }
        }
        Start = i + 1;
    }
    return STATUS_SUCCESS;
}

/* The folder a file lives in, given its full path. */
String FolderOf(const String &Path)
{
    const char *Last = nullptr;
    for (size_t i = 0; i < Path.Length(); i++)
        if (Path[i] == '\\')
            Last = Path.CStr() + i;
    if (!Last)
        return Path;
    return String(Path.CStr(), (size_t)(Last - Path.CStr()) + 1);
}

/* Writes one member. CreateFolders is false for a member whose parent folders are known to exist. */
STATUS WriteMember(const String &Dest, const String &Relative, const uint8_t *Data, size_t Length,
                   Result *Out, int *Counted)
{
    String Full = hz::fs::Join(Dest, Relative.CStr());
    STATUS Status = MakeFolders(FolderOf(Full));
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = hz::fs::WriteAll(Full, (const char *)Data, Length);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Out && Counted) {
        Out->Files++;
        Out->Bytes += Length;
    }
    return STATUS_SUCCESS;
}

STATUS MakeMemberFolder(const String &Dest, const String &Relative, Result *Out, int *Counted)
{
    String Full = hz::fs::Join(Dest, Relative.CStr());
    STATUS Status = MakeFolders(Full);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Out && Counted)
        Out->Folders++;
    return STATUS_SUCCESS;
}

/* ---- ZIP -------------------------------------------------------------------------------------- */

struct ZipRecord {
    String Name;
    uint64_t LocalOffset;
    uint64_t Compressed;
    uint64_t Size;
    uint16_t Method;
    uint32_t Crc;
    bool Directory;
};

Format DetectBytes(const uint8_t *Data, size_t Length)
{
    if (Length >= 4 && Data[0] == 'P' && Data[1] == 'K' && (Data[2] == 3 || Data[2] == 5 || Data[2] == 7))
        return Format::Zip;
    if (Length >= 3 && Data[0] == 0x1F && Data[1] == 0x8B && Data[2] == 8)
        return Format::Gzip;
    if (Length >= 263 && memcmp(Data + 257, "ustar", 5) == 0)
        return Format::Tar;
    return Format::Unknown;
}

STATUS ReadZipIndex(const uint8_t *Data, size_t Length, Vector<ZipRecord> &Out)
{
    if (Length < 22)
        return STATUS_FILE_CORRUPT_ERROR;
    size_t Scan = Length > 65557 ? Length - 65557 : 0;
    size_t End = (size_t)-1;
    for (size_t i = Length - 22 + 1; i-- > Scan;) {
        if (Le32(Data + i) == 0x06054B50u) {
            End = i;
            break;
        }
    }
    if (End == (size_t)-1)
        return STATUS_FILE_CORRUPT_ERROR;
    uint16_t Count = Le16(Data + End + 10);
    uint32_t DirectorySize = Le32(Data + End + 12);
    uint32_t DirectoryOffset = Le32(Data + End + 16);
    if ((uint64_t)DirectoryOffset + DirectorySize > Length)
        return STATUS_FILE_CORRUPT_ERROR;
    size_t P = DirectoryOffset;
    for (uint16_t i = 0; i < Count; i++) {
        if (P + 46 > Length || Le32(Data + P) != 0x02014B50u)
            return STATUS_FILE_CORRUPT_ERROR;
        uint16_t Method = Le16(Data + P + 10);
        uint32_t Crc = Le32(Data + P + 16);
        uint32_t Compressed = Le32(Data + P + 20);
        uint32_t Size = Le32(Data + P + 24);
        uint16_t NameLength = Le16(Data + P + 28);
        uint16_t ExtraLength = Le16(Data + P + 30);
        uint16_t CommentLength = Le16(Data + P + 32);
        uint32_t Attributes = Le32(Data + P + 38);
        uint32_t LocalOffset = Le32(Data + P + 42);
        if (P + 46u + NameLength > Length)
            return STATUS_FILE_CORRUPT_ERROR;
        if (Size == 0xFFFFFFFFu || Compressed == 0xFFFFFFFFu || LocalOffset == 0xFFFFFFFFu)
            return STATUS_NOT_SUPPORTED; /* Zip64 */
        ZipRecord Record;
        Record.Name = String((const char *)Data + P + 46, NameLength);
        Record.Method = Method;
        Record.Size = Size;
        Record.Compressed = Compressed;
        Record.LocalOffset = LocalOffset;
        Record.Crc = Crc;
        Record.Directory = (!Record.Name.Empty() && Record.Name[Record.Name.Length() - 1] == '/') ||
                           (Attributes & 0x10u) != 0;
        Out.Push(Move(Record));
        P += 46u + NameLength + ExtraLength + CommentLength;
    }
    return STATUS_SUCCESS;
}

STATUS ReadZipMember(const uint8_t *Data, size_t Length, const ZipRecord &Record, Vector<uint8_t> &Out)
{
    size_t P = (size_t)Record.LocalOffset;
    if (P + 30 > Length || Le32(Data + P) != 0x04034B50u)
        return STATUS_FILE_CORRUPT_ERROR;
    uint16_t NameLength = Le16(Data + P + 26);
    uint16_t ExtraLength = Le16(Data + P + 28);
    size_t Start = P + 30u + NameLength + ExtraLength;
    if ((uint64_t)Start + Record.Compressed > Length)
        return STATUS_FILE_CORRUPT_ERROR;
    if (Record.Method == 0) {
        if (Record.Compressed > kMaxEntryBytes)
            return STATUS_FILE_TOO_LARGE;
        for (uint64_t i = 0; i < Record.Compressed; i++)
            Out.Push(Data[Start + i]);
    } else if (Record.Method == 8) {
        BitReader Reader = {Data + Start, (size_t)Record.Compressed, 0, 0, 0, 0};
        if (!InflateRaw(Reader, Out, (size_t)Record.Size))
            return STATUS_FILE_CORRUPT_ERROR;
    } else {
        return STATUS_NOT_SUPPORTED; /* encrypted, deflate64, bzip2, ... */
    }
    if (Out.Size() != Record.Size)
        return STATUS_FILE_CORRUPT_ERROR;
    if (Crc32(0, Out.Data(), Out.Size()) != Record.Crc)
        return STATUS_FILE_CORRUPT_ERROR;
    return STATUS_SUCCESS;
}

STATUS ExtractZip(const uint8_t *Data, size_t Length, const String &Dest, Result *Out)
{
    Vector<ZipRecord> Records;
    STATUS Status = ReadZipIndex(Data, Length, Records);
    if (!HZ_SUCCESS(Status))
        return Status;
    Vector<uint8_t> Member;
    int Counted = 0;
    for (ZipRecord &Record : Records) {
        String Relative;
        if (!SafeRelative(Record.Name, Relative))
            return STATUS_FILE_CORRUPT_ERROR;
        if (Record.Directory) {
            Status = MakeMemberFolder(Dest, Relative, Out, &Counted);
        } else {
            Member.Clear();
            Status = ReadZipMember(Data, Length, Record, Member);
            if (HZ_SUCCESS(Status))
                Status = WriteMember(Dest, Relative, Member.Data(), Member.Size(), Out, &Counted);
        }
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    return STATUS_SUCCESS;
}

/* ---- tar (ustar) ------------------------------------------------------------------------------ */

bool TarOctal(const uint8_t *Field, size_t Length, uint64_t &Out)
{
    size_t i = 0;
    while (i < Length && (Field[i] == ' ' || Field[i] == '\0'))
        i++;
    uint64_t Value = 0;
    bool Any = false;
    for (; i < Length; i++) {
        if (Field[i] < '0' || Field[i] > '7')
            break;
        Value = (Value << 3) | (uint64_t)(Field[i] - '0');
        Any = true;
    }
    Out = Value;
    return Any;
}

bool TarHeaderOk(const uint8_t *Header)
{
    uint64_t Stored = 0;
    if (!TarOctal(Header + 148, 8, Stored))
        return false;
    uint32_t Sum = 0;
    for (size_t i = 0; i < 512; i++)
        Sum += (i >= 148 && i < 156) ? (uint32_t)' ' : (uint32_t)Header[i];
    return Sum == (uint32_t)Stored;
}

String TarField(const uint8_t *Field, size_t Maximum)
{
    size_t Length = 0;
    while (Length < Maximum && Field[Length] != '\0')
        Length++;
    return String((const char *)Field, Length);
}

String TarName(const uint8_t *Header)
{
    String Name = TarField(Header, 100);
    String Prefix = TarField(Header + 345, 155);
    if (Prefix.Empty())
        return Name;
    String Full = Prefix;
    Full.Append("/", 1);
    Full.Append(Name.CStr(), Name.Length());
    return Full;
}

STATUS ExtractTar(const uint8_t *Data, size_t Length, const String &Dest, Result *Out)
{
    size_t P = 0;
    String LongName;
    int Counted = 0;
    while (P + 512 <= Length) {
        const uint8_t *Header = Data + P;
        bool Empty = true;
        for (size_t i = 0; i < 512 && Empty; i++)
            Empty = Header[i] == 0;
        if (Empty)
            break; /* the two zero blocks that end an archive */
        if (!TarHeaderOk(Header))
            return STATUS_FILE_CORRUPT_ERROR;
        uint64_t Size = 0;
        TarOctal(Header + 124, 12, Size);
        uint8_t Type = Header[156];
        String Name = LongName.Empty() ? TarName(Header) : LongName;
        LongName.Clear();
        size_t DataStart = P + 512;
        size_t DataEnd = DataStart + (size_t)Size;
        if (DataEnd > Length)
            return STATUS_FILE_CORRUPT_ERROR;
        STATUS Status = STATUS_SUCCESS;
        if (Type == 'L') {
            /* a GNU long name: the data is the name of the next member */
            size_t Count = (size_t)Size;
            while (Count > 0 && Data[DataStart + Count - 1] == '\0')
                Count--;
            LongName = String((const char *)Data + DataStart, Count);
        } else if (Type == '0' || Type == '\0') {
            String Relative;
            if (!SafeRelative(Name, Relative))
                return STATUS_FILE_CORRUPT_ERROR;
            Status = WriteMember(Dest, Relative, Data + DataStart, (size_t)Size, Out, &Counted);
        } else if (Type == '5') {
            String Relative;
            if (!SafeRelative(Name, Relative))
                return STATUS_FILE_CORRUPT_ERROR;
            Status = MakeMemberFolder(Dest, Relative, Out, &Counted);
        } else if (Type == 'x' || Type == 'g' || Type == '1' || Type == '2') {
            /* a pax header or a hard/symbolic link: skipped, the file it points at is not unpacked */
        } else {
            return STATUS_NOT_SUPPORTED;
        }
        if (!HZ_SUCCESS(Status))
            return Status;
        P = DataStart + ((size_t)Size + 511u) / 512u * 512u;
    }
    return STATUS_SUCCESS;
}

/* ---- gzip ------------------------------------------------------------------------------------- */

STATUS Gunzip(const uint8_t *Data, size_t Length, Vector<uint8_t> &Out)
{
    if (Length < 18)
        return STATUS_FILE_CORRUPT_ERROR;
    uint8_t Flags = Data[3];
    size_t P = 10;
    if (Flags & 0x04) { /* FEXTRA */
        if (P + 2 > Length)
            return STATUS_FILE_CORRUPT_ERROR;
        P += 2 + (size_t)Le16(Data + P);
    }
    if (Flags & 0x08) { /* FNAME */
        while (P < Length && Data[P] != 0)
            P++;
        P++;
    }
    if (Flags & 0x10) { /* FCOMMENT */
        while (P < Length && Data[P] != 0)
            P++;
        P++;
    }
    if (Flags & 0x02) /* FHCRC */
        P += 2;
    if (P + 8 > Length)
        return STATUS_FILE_CORRUPT_ERROR;
    BitReader Reader = {Data + P, Length - 8 - P, 0, 0, 0, 0};
    if (!InflateRaw(Reader, Out, 0))
        return STATUS_FILE_CORRUPT_ERROR;
    if (Crc32(0, Out.Data(), Out.Size()) != Le32(Data + Length - 8))
        return STATUS_FILE_CORRUPT_ERROR;
    if ((uint32_t)(Out.Size() & 0xFFFFFFFFu) != Le32(Data + Length - 4))
        return STATUS_FILE_CORRUPT_ERROR;
    return STATUS_SUCCESS;
}

}  // namespace

Format Detect(const String &Path)
{
    String Bytes;
    if (!HZ_SUCCESS(hz::fs::ReadAll(Path, Bytes, kMaxArchiveBytes)))
        return Format::Unknown;
    return DetectBytes((const uint8_t *)Bytes.CStr(), Bytes.Length());
}

const char *FormatName(Format Value)
{
    switch (Value) {
    case Format::Zip: return "ZIP";
    case Format::Gzip: return "gzip";
    case Format::Tar: return "tar";
    case Format::Unknown: return "archive";
    }
    return "archive";
}

bool IsArchive(const String &FileName)
{
    static const char *const Extensions[] = {".zip", ".tar", ".gz", ".tgz", ".tar.gz"};
    for (const char *Extension : Extensions)
        if (EndsWithNoCase(FileName.CStr(), Extension))
            return true;
    return false;
}

String StripSuffix(const String &FileName)
{
    static const char *const Extensions[] = {".tar.gz", ".tgz", ".zip", ".tar", ".gz"};
    for (const char *Extension : Extensions) {
        if (EndsWithNoCase(FileName.CStr(), Extension) && FileName.Length() > strlen(Extension))
            return String(FileName.CStr(), FileName.Length() - strlen(Extension));
    }
    return String(FileName.CStr());
}

STATUS List(const String &Path, Vector<Entry> &Out)
{
    Out.Clear();
    String Bytes;
    STATUS Status = hz::fs::ReadAll(Path, Bytes, kMaxArchiveBytes);
    if (!HZ_SUCCESS(Status))
        return Status;
    const uint8_t *Data = (const uint8_t *)Bytes.CStr();
    size_t Length = Bytes.Length();
    switch (DetectBytes(Data, Length)) {
    case Format::Zip: {
        Vector<ZipRecord> Records;
        Status = ReadZipIndex(Data, Length, Records);
        if (!HZ_SUCCESS(Status))
            return Status;
        for (const ZipRecord &Record : Records) {
            Entry Item;
            String Relative;
            if (!SafeRelative(Record.Name, Relative))
                return STATUS_FILE_CORRUPT_ERROR;
            Item.Name = Move(Relative);
            Item.Directory = Record.Directory;
            Item.Size = Record.Size;
            Item.Compressed = Record.Compressed;
            Item.Crc = Record.Crc;
            Item.HasCrc = true;
            Out.Push(Move(Item));
        }
        return STATUS_SUCCESS;
    }
    case Format::Tar: {
        size_t P = 0;
        String LongName;
        while (P + 512 <= Length) {
            const uint8_t *Header = Data + P;
            bool Empty = true;
            for (size_t i = 0; i < 512 && Empty; i++)
                Empty = Header[i] == 0;
            if (Empty)
                break;
            if (!TarHeaderOk(Header))
                return STATUS_FILE_CORRUPT_ERROR;
            uint64_t Size = 0;
            TarOctal(Header + 124, 12, Size);
            uint8_t Type = Header[156];
            String Name = LongName.Empty() ? TarName(Header) : LongName;
            LongName.Clear();
            if (Type == 'L') {
                size_t Count = (size_t)Size;
                while (Count > 0 && Data[P + 512 + Count - 1] == '\0')
                    Count--;
                LongName = String((const char *)Data + P + 512, Count);
            } else if (Type == '0' || Type == '\0' || Type == '5') {
                Entry Item;
                String Relative;
                if (!SafeRelative(Name, Relative))
                    return STATUS_FILE_CORRUPT_ERROR;
                Item.Name = Move(Relative);
                Item.Directory = Type == '5';
                Item.Size = Item.Directory ? 0 : Size;
                Out.Push(Move(Item));
            } else if (Type != 'x' && Type != 'g') {
                return STATUS_NOT_SUPPORTED;
            }
            P += 512 + ((size_t)Size + 511u) / 512u * 512u;
        }
        return STATUS_SUCCESS;
    }
    case Format::Gzip: {
        /* A gzip stream is one file; its size is only known once it is decompressed, so the listing
           reports the name and leaves the size at zero. */
        Entry Item;
        Item.Name = StripSuffix(hz::fs::FileName(Path));
        Out.Push(Move(Item));
        return STATUS_SUCCESS;
    }
    case Format::Unknown:
        break;
    }
    return STATUS_NOT_SUPPORTED;
}

STATUS Extract(const String &Path, const String &DestFolder, Result *Out)
{
    Result Local;
    if (!Out)
        Out = &Local;
    String Bytes;
    STATUS Status = hz::fs::ReadAll(Path, Bytes, kMaxArchiveBytes);
    if (!HZ_SUCCESS(Status))
        return Status;
    const uint8_t *Data = (const uint8_t *)Bytes.CStr();
    size_t Length = Bytes.Length();
    Status = MakeFolders(DestFolder);
    if (!HZ_SUCCESS(Status))
        return Status;
    switch (DetectBytes(Data, Length)) {
    case Format::Zip:
        return ExtractZip(Data, Length, DestFolder, Out);
    case Format::Tar:
        return ExtractTar(Data, Length, DestFolder, Out);
    case Format::Gzip: {
        /* .tar.gz holds a tar stream; a plain .gz holds one file named after the archive. */
        Vector<uint8_t> Plain;
        Status = Gunzip(Data, Length, Plain);
        if (!HZ_SUCCESS(Status))
            return Status;
        if (DetectBytes(Plain.Data(), Plain.Size()) == Format::Tar)
            return ExtractTar(Plain.Data(), Plain.Size(), DestFolder, Out);
        int Counted = 0;
        String Name = StripSuffix(hz::fs::FileName(Path));
        return WriteMember(DestFolder, Name, Plain.Data(), Plain.Size(), Out, &Counted);
    }
    case Format::Unknown:
        break;
    }
    return STATUS_NOT_SUPPORTED;
}

}  // namespace hz::archive
