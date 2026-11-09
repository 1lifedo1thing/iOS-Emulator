#include "ipa.h"

#include <plist/plist.h>

#include <fstream>
#include <sstream>

namespace {

// The ZIP records this reads, by their signatures.
constexpr uint32_t kEndRecord = 0x06054b50, kCentralEntry = 0x02014b50, kLocalEntry = 0x04034b50;
constexpr size_t kEndRecordSize = 22, kCentralEntrySize = 46, kLocalEntrySize = 30;
constexpr size_t kMaxComment = 65535;              // how far the end record can be from the file's end
constexpr uint32_t kMaxPlist = 1 << 20;            // an Info.plist bigger than this isn't a real one
constexpr uint32_t kMaxDirectory = 64 << 20;
constexpr uint32_t kZip64 = 0xFFFFFFFF;            // this value means the real one is in a ZIP64 record
constexpr uint16_t kStored = 0, kDeflated = 8;     // compression methods

uint32_t Read16(const std::string &data, size_t at) {
    return uint8_t(data[at]) | uint8_t(data[at + 1]) << 8;
}

uint32_t Read32(const std::string &data, size_t at) {
    return Read16(data, at) | uint32_t(Read16(data, at + 2)) << 16;
}

std::string ReadAt(std::ifstream &file, uint64_t offset, size_t size) {
    std::string data(size, '\0');
    file.seekg(std::streamoff(offset));
    file.read(data.data(), std::streamsize(size));
    data.resize(size_t(file.gcount()));
    return data;
}

// "Payload/<name>.app/Info.plist", the app's own (plug-ins and frameworks have theirs deeper down).
bool IsAppInfoPlist(const std::string &name) {
    const std::string start = "Payload/", end = ".app/Info.plist";
    return name.size() > start.size() + end.size() && name.compare(0, start.size(), start) == 0 &&
           name.compare(name.size() - end.size(), end.size(), end) == 0 &&
           name.find('/', start.size()) == name.size() - end.size() + 4;
}

// zlib's z_stream as zlib.h declares it for 64-bit Windows, where its uLong has 32 bits.
struct ZStream {
    const uint8_t *nextIn;
    uint32_t availableIn, totalIn;
    uint8_t *nextOut;
    uint32_t availableOut, totalOut;
    const char *message;
    void *state, *allocate, *release, *opaque;
    int dataType;
    uint32_t adler, reserved;
};

// Inflates a ZIP member into `size` bytes; empty on failure. resources\libz-1.dll is resolved by
// name, as the engine is (engine.h), since there is no import library for it.
std::string Inflate(const std::string &compressed, size_t size) {
    constexpr int kRawDeflate = -15, kFinish = 4, kStreamEnd = 1;
    static HMODULE zlib = LoadLibraryW(L"libz-1.dll");
    if (!zlib)
        return {};
    auto init = reinterpret_cast<int (*)(ZStream *, int, const char *, int)>(GetProcAddress(zlib, "inflateInit2_"));
    auto inflate = reinterpret_cast<int (*)(ZStream *, int)>(GetProcAddress(zlib, "inflate"));
    auto end = reinterpret_cast<int (*)(ZStream *)>(GetProcAddress(zlib, "inflateEnd"));
    if (!init || !inflate || !end)
        return {};
    std::string out(size, '\0');
    ZStream stream{};
    stream.nextIn = reinterpret_cast<const uint8_t *>(compressed.data());
    stream.availableIn = uint32_t(compressed.size());
    stream.nextOut = reinterpret_cast<uint8_t *>(out.data());
    stream.availableOut = uint32_t(size);
    if (init(&stream, kRawDeflate, "1", int(sizeof stream)) != 0)
        return {};
    const bool finished = inflate(&stream, kFinish) == kStreamEnd;
    end(&stream);
    return finished ? out : std::string();
}

// One member's bytes, given its entry in the central directory.
std::string ReadMember(std::ifstream &file, const std::string &directory, size_t entry) {
    const uint16_t method = uint16_t(Read16(directory, entry + 10));
    const uint32_t compressed = Read32(directory, entry + 20), size = Read32(directory, entry + 24);
    const uint64_t local = Read32(directory, entry + 42);
    if (compressed > kMaxPlist || size > kMaxPlist)
        return {};
    const std::string header = ReadAt(file, local, kLocalEntrySize);
    if (header.size() < kLocalEntrySize || Read32(header, 0) != kLocalEntry)
        return {};
    // The local header's name and extra lengths can differ from the central directory's.
    const uint64_t start = local + kLocalEntrySize + Read16(header, 26) + Read16(header, 28);
    const std::string bytes = ReadAt(file, start, compressed);
    if (bytes.size() != compressed)
        return {};
    return method == kStored ? bytes : method == kDeflated ? Inflate(bytes, size) : std::string();
}

// A property list's string value (XML or binary); empty if it has none.
std::string PlistString(const std::string &data, const char *key) {
    plist_t root = nullptr;
    std::string value;
    plist_from_memory(data.data(), uint32_t(data.size()), &root, nullptr);
    plist_t item = root ? plist_dict_get_item(root, key) : nullptr;
    if (item && plist_get_node_type(item) == PLIST_STRING) {
        char *text = nullptr;
        plist_get_string_val(item, &text);
        value = text ? text : "";
        plist_mem_free(text);
    }
    if (root)
        plist_free(root);
    return value;
}

std::vector<int> VersionParts(const std::string &version) {
    std::vector<int> parts;
    std::istringstream stream(version);
    for (std::string part; std::getline(stream, part, '.');)
        parts.push_back(std::atoi(part.c_str()));
    return parts;
}

}  // namespace

IpaInfo ReadIpa(const fs::path &ipa) {
    IpaInfo info;
    std::ifstream file(ipa, std::ios::binary);
    std::error_code error;
    const uint64_t length = fs::file_size(ipa, error);
    if (!file || error || length < kEndRecordSize)
        return info;

    // The end record is the last thing in the file, before a comment of any length up to 64 KB.
    const uint64_t tailSize = std::min<uint64_t>(length, kEndRecordSize + kMaxComment);
    const std::string tail = ReadAt(file, length - tailSize, size_t(tailSize));
    size_t end = tail.size() - kEndRecordSize + 1;
    while (end > 0 && Read32(tail, end - 1) != kEndRecord)
        end--;
    if (end == 0)
        return info;
    const size_t record = end - 1;
    const uint32_t directorySize = Read32(tail, record + 12), directoryAt = Read32(tail, record + 16);
    if (directorySize > kMaxDirectory || directoryAt == kZip64)
        return info;
    const std::string directory = ReadAt(file, directoryAt, directorySize);

    uint64_t unpacked = 0;
    size_t appPlist = std::string::npos;
    for (size_t at = 0; at + kCentralEntrySize <= directory.size();) {
        if (Read32(directory, at) != kCentralEntry)
            return info;
        const uint32_t size = Read32(directory, at + 24);
        const size_t nameSize = Read16(directory, at + 28), extra = Read16(directory, at + 30),
                     comment = Read16(directory, at + 32), next = at + kCentralEntrySize + nameSize + extra + comment;
        if (size == kZip64 || next > directory.size())
            return info;
        unpacked += size;
        if (IsAppInfoPlist(directory.substr(at + kCentralEntrySize, nameSize)))
            appPlist = at;
        at = next;
    }
    info.unpackedBytes = unpacked;
    if (appPlist != std::string::npos)
        info.minimumOS = PlistString(ReadMember(file, directory, appPlist), "MinimumOSVersion");
    return info;
}

bool IosOlderThan(const std::string &device, const std::string &minimum) {
    // Device::Version() falls back to the build ("7E18"), which isn't a version.
    if (minimum.empty() || device.find('.') == std::string::npos)
        return false;
    return VersionParts(device) < VersionParts(minimum);
}
