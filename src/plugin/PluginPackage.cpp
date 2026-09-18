#include "libera/plugin/PluginPackage.hpp"

#include "libera/plugin/libera_plugin.h"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace libera::plugin {

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr std::uint64_t maximumPackageBytes = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t maximumExtractedBytes = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t maximumFileBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t maximumManifestBytes = 1024ull * 1024ull;
constexpr std::uint64_t maximumCompressionRatio = 1000;
constexpr mz_uint maximumArchiveEntries = 512;
constexpr std::string_view preservedArchiveName = "archive.liberaplugin";

struct ArchiveEntry {
    mz_uint index = 0;
    std::string path;
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    bool directory = false;
};

struct OpenArchive {
    mz_zip_archive archive{};
    MZ_FILE* sourceFile = nullptr;
    bool open = false;

    ~OpenArchive() {
        if (open) {
            mz_zip_reader_end(&archive);
        }
        if (sourceFile) {
            std::fclose(sourceFile);
        }
    }
};

std::uint32_t rotateRight(std::uint32_t value, std::uint32_t amount) {
    return (value >> amount) | (value << (32u - amount));
}

// Small streaming SHA-256 implementation used only to give immutable package
// revisions a collision-resistant content identity. Package authenticity is a
// separate concern and is not implied by this digest.
class Sha256 {
public:
    Sha256() { reset(); }

    void update(const std::uint8_t* data, std::size_t size) {
        totalBytes += size;
        while (size > 0) {
            const std::size_t count = std::min(size, block.size() - blockSize);
            std::copy_n(data, count, block.data() + blockSize);
            blockSize += count;
            data += count;
            size -= count;
            if (blockSize == block.size()) {
                transform(block.data());
                blockSize = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finish() {
        const std::uint64_t totalBits = totalBytes * 8u;
        block[blockSize++] = 0x80u;
        if (blockSize > 56) {
            std::fill(block.begin() + static_cast<std::ptrdiff_t>(blockSize),
                      block.end(), 0);
            transform(block.data());
            blockSize = 0;
        }
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(blockSize),
                  block.begin() + 56, 0);
        for (std::size_t i = 0; i < 8; ++i) {
            block[63 - i] = static_cast<std::uint8_t>(totalBits >> (i * 8));
        }
        transform(block.data());

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t i = 0; i < state.size(); ++i) {
            digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
            digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
            digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
        }
        return digest;
    }

private:
    void reset() {
        state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        block.fill(0);
        blockSize = 0;
        totalBytes = 0;
    }

    void transform(const std::uint8_t* input) {
        static constexpr std::array<std::uint32_t, 64> constants = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<std::uint32_t>(input[i * 4]) << 24) |
                       (static_cast<std::uint32_t>(input[i * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(input[i * 4 + 2]) << 8) |
                       static_cast<std::uint32_t>(input[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const std::uint32_t s0 = rotateRight(words[i - 15], 7) ^
                                     rotateRight(words[i - 15], 18) ^
                                     (words[i - 15] >> 3);
            const std::uint32_t s1 = rotateRight(words[i - 2], 17) ^
                                     rotateRight(words[i - 2], 19) ^
                                     (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::uint32_t sum1 = rotateRight(e, 6) ^
                                       rotateRight(e, 11) ^
                                       rotateRight(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sum1 + choose + constants[i] + words[i];
            const std::uint32_t sum0 = rotateRight(a, 2) ^
                                       rotateRight(a, 13) ^
                                       rotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::uint32_t, 8> state{};
    std::array<std::uint8_t, 64> block{};
    std::size_t blockSize = 0;
    std::uint64_t totalBytes = 0;
};

std::string fileSha256(const fs::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "Failed to open package for hashing";
        return {};
    }

    Sha256 sha;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            sha.update(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) {
        if (error) *error = "Failed while hashing package";
        return {};
    }

    const auto digest = sha.finish();
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool isWindowsReservedName(const std::string& component) {
    std::string base = lowerAscii(component);
    const auto dot = base.find('.');
    if (dot != std::string::npos) base.resize(dot);
    static const std::set<std::string> reserved = {
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4",
        "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2",
        "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    return reserved.find(base) != reserved.end();
}

bool validatePortablePath(const std::string& path,
                          bool directory,
                          std::string* error) {
    if (path.empty() || path.size() > 1024 || path.front() == '/' ||
        path.front() == '\\' || path.find('\\') != std::string::npos) {
        if (error) *error = "Package contains an unsafe path: " + path;
        return false;
    }

    std::string normalized = path;
    if (directory && !normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    if (normalized.empty()) {
        if (error) *error = "Package contains an empty path";
        return false;
    }

    std::size_t start = 0;
    while (start <= normalized.size()) {
        const auto slash = normalized.find('/', start);
        const auto length = slash == std::string::npos
            ? normalized.size() - start
            : slash - start;
        const std::string component = normalized.substr(start, length);
        if (component.empty() || component == "." || component == ".." ||
            component.back() == ' ' || component.back() == '.' ||
            isWindowsReservedName(component)) {
            if (error) *error = "Package contains an unsafe path: " + path;
            return false;
        }
        for (const unsigned char c : component) {
            if (c < 0x20 || c > 0x7e || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') {
                if (error) *error = "Package path is not portable: " + path;
                return false;
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

std::string archiveFilename(mz_zip_archive* archive, mz_uint index) {
    const mz_uint needed = mz_zip_reader_get_filename(archive, index, nullptr, 0);
    if (needed == 0 || needed > 4096) return {};
    std::vector<char> storage(needed);
    if (mz_zip_reader_get_filename(archive, index,
                                   storage.data(), needed) == 0) {
        return {};
    }
    if (std::char_traits<char>::length(storage.data()) + 1 != needed) {
        return {};
    }
    return std::string(storage.data());
}

bool openAndValidateArchive(const fs::path& packagePath,
                            OpenArchive& opened,
                            std::vector<ArchiveEntry>& entries,
                            std::unordered_map<std::string, mz_uint>& filesByPath,
                            std::string* error) {
#ifdef _WIN32
    opened.sourceFile = _wfopen(packagePath.c_str(), L"rb");
#else
    opened.sourceFile = std::fopen(packagePath.c_str(), "rb");
#endif
    if (!opened.sourceFile ||
        !mz_zip_reader_init_cfile(&opened.archive,
                                  opened.sourceFile,
                                  0,
                                  0)) {
        if (error) {
            *error = "Invalid ZIP archive: " + std::string(mz_zip_get_error_string(
                mz_zip_get_last_error(&opened.archive)));
        }
        return false;
    }
    opened.open = true;

    const mz_uint fileCount = mz_zip_reader_get_num_files(&opened.archive);
    if (fileCount == 0 || fileCount > maximumArchiveEntries) {
        if (error) *error = "Package has an invalid number of entries";
        return false;
    }

    std::set<std::string> portablePaths;
    std::set<std::string> portableFilePaths;
    std::uint64_t totalSize = 0;
    for (mz_uint index = 0; index < fileCount; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&opened.archive, index, &stat)) {
            if (error) *error = "Failed to read ZIP entry metadata";
            return false;
        }
        if (stat.m_is_encrypted || !stat.m_is_supported) {
            if (error) *error = "Package contains an encrypted or unsupported entry";
            return false;
        }

        const std::string path = archiveFilename(&opened.archive, index);
        if (!validatePortablePath(path, stat.m_is_directory != 0, error)) {
            return false;
        }

        // ZIP stores Unix file type bits in the upper half of external_attr.
        // Reject symlinks and other special files; packages contain only
        // ordinary files and directory markers.
        const std::uint32_t unixMode = stat.m_external_attr >> 16u;
        const std::uint32_t unixType = unixMode & 0170000u;
        if (unixType != 0 && unixType != 0100000u && unixType != 0040000u) {
            if (error) *error = "Package contains a link or special file: " + path;
            return false;
        }

        const std::string collisionKey = lowerAscii(
            stat.m_is_directory && path.back() == '/'
                ? path.substr(0, path.size() - 1)
                : path);
        if (!portablePaths.insert(collisionKey).second) {
            if (error) *error = "Package contains duplicate paths: " + path;
            return false;
        }

        // A regular file may not also act as a parent directory. ZIP permits
        // that ambiguous topology, but filesystem extraction does not.
        std::size_t slash = collisionKey.find('/');
        while (slash != std::string::npos) {
            if (portableFilePaths.find(collisionKey.substr(0, slash)) !=
                portableFilePaths.end()) {
                if (error) *error = "Package file collides with a directory: " + path;
                return false;
            }
            slash = collisionKey.find('/', slash + 1);
        }
        if (!stat.m_is_directory) {
            const std::string childPrefix = collisionKey + "/";
            const auto possibleChild = portablePaths.lower_bound(childPrefix);
            if (possibleChild != portablePaths.end() &&
                possibleChild->compare(0, childPrefix.size(), childPrefix) == 0) {
                if (error) *error = "Package file collides with a directory: " + path;
                return false;
            }
            portableFilePaths.insert(collisionKey);
        }

        if (!stat.m_is_directory) {
            if (stat.m_uncomp_size > maximumFileBytes ||
                totalSize > maximumExtractedBytes - stat.m_uncomp_size) {
                if (error) *error = "Package exceeds the allowed expanded size";
                return false;
            }
            if (stat.m_uncomp_size > 1024 * 1024 &&
                (stat.m_comp_size == 0 ||
                 stat.m_uncomp_size / stat.m_comp_size > maximumCompressionRatio)) {
                if (error) *error = "Package contains a suspicious compression ratio";
                return false;
            }
            totalSize += stat.m_uncomp_size;
            filesByPath.emplace(path, index);
        }
        entries.push_back({index,
                           path,
                           stat.m_comp_size,
                           stat.m_uncomp_size,
                           stat.m_is_directory != 0});
    }

    if (!mz_zip_validate_archive(&opened.archive, 0)) {
        if (error) *error = "ZIP data or CRC validation failed";
        return false;
    }
    return true;
}

bool readRequiredString(const Json& object,
                        const char* key,
                        std::string& output,
                        std::string* error) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
        if (error) *error = std::string("Manifest is missing '") + key + "'";
        return false;
    }
    output = it->get<std::string>();
    return true;
}

bool validIdentifier(const std::string& value) {
    static const std::regex pattern("^[a-z0-9]+([.-][a-z0-9]+)*$");
    return value.size() <= 128 && std::regex_match(value, pattern);
}

bool validControllerType(const std::string& value) {
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_.-]*$");
    return value.size() <= 128 && std::regex_match(value, pattern);
}

bool validVersion(const std::string& value) {
    static const std::regex pattern(
        "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
        "(?:-(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
        "(?:\\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*)?"
        "(?:\\+[0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*)?$");
    return value.size() <= 64 && std::regex_match(value, pattern);
}

bool validSingleLineText(const std::string& value, std::size_t maximumBytes) {
    if (value.empty() || value.size() > maximumBytes) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

bool validDescription(const std::string& value) {
    if (value.size() > 4096) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return (c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f;
    });
}

bool hasExpectedLibraryExtension(const PluginEntrypoint& entrypoint) {
    const std::string extension = lowerAscii(
        fs::path(entrypoint.path).extension().string());
    if (entrypoint.os == "windows") return extension == ".dll";
    if (entrypoint.os == "macos") return extension == ".dylib";
    if (entrypoint.os == "linux") return extension == ".so";
    return false;
}

std::string currentOs() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string currentArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

bool parseManifest(const std::string& text,
                   PluginManifest& manifest,
                   std::string* error) {
    Json root;
    try {
        root = Json::parse(text);
    } catch (const std::exception& exception) {
        if (error) *error = "Invalid manifest JSON: " + std::string(exception.what());
        return false;
    }
    if (!root.is_object()) {
        if (error) *error = "Manifest root must be an object";
        return false;
    }

    if (!root.contains("schemaVersion") || !root["schemaVersion"].is_number_unsigned()) {
        if (error) *error = "Manifest is missing unsigned schemaVersion";
        return false;
    }
    manifest.schemaVersion = root["schemaVersion"].get<std::uint32_t>();
    if (manifest.schemaVersion != 1) {
        if (error) *error = "Unsupported plugin package schema version";
        return false;
    }

    if (!readRequiredString(root, "id", manifest.id, error) ||
        !readRequiredString(root, "version", manifest.version, error) ||
        !readRequiredString(root, "name", manifest.name, error) ||
        !readRequiredString(root, "vendor", manifest.vendor, error) ||
        !readRequiredString(root, "controllerType", manifest.controllerType, error)) {
        return false;
    }
    if (!validIdentifier(manifest.id)) {
        if (error) *error =
            "Plugin id must be a lowercase dotted or hyphenated identifier";
        return false;
    }
    if (manifest.id == "libera.builtin" ||
        manifest.id.rfind("libera.builtin.", 0) == 0) {
        if (error) *error = "Plugin id uses the reserved libera.builtin namespace";
        return false;
    }
    if (!validVersion(manifest.version)) {
        if (error) *error = "Plugin version must be valid semantic versioning";
        return false;
    }
    if (!validControllerType(manifest.controllerType)) {
        if (error) *error = "controllerType contains invalid characters";
        return false;
    }
    if (!validSingleLineText(manifest.name, 256) ||
        !validSingleLineText(manifest.vendor, 256)) {
        if (error) *error =
            "Plugin name and vendor must be single-line text of at most 256 bytes";
        return false;
    }
    if (root.contains("description")) {
        if (!root["description"].is_string()) {
            if (error) *error = "Manifest description must be a string";
            return false;
        }
        manifest.description = root["description"].get<std::string>();
        if (!validDescription(manifest.description)) {
            if (error) *error =
                "Plugin description contains invalid control characters or is too long";
            return false;
        }
    }

    if (!root.contains("libera") || !root["libera"].is_object() ||
        !root["libera"].contains("abiVersion") ||
        !root["libera"]["abiVersion"].is_number_unsigned()) {
        if (error) *error = "Manifest is missing libera.abiVersion";
        return false;
    }
    manifest.abiVersion = root["libera"]["abiVersion"].get<std::uint32_t>();
    if (manifest.abiVersion != LIBERA_PLUGIN_API_VERSION) {
        if (error) *error = "Plugin ABI is not supported by this Libera build";
        return false;
    }

    if (!root.contains("entrypoints") || !root["entrypoints"].is_array() ||
        root["entrypoints"].empty()) {
        if (error) *error = "Manifest must contain at least one entrypoint";
        return false;
    }
    std::set<std::pair<std::string, std::string>> targets;
    for (const auto& value : root["entrypoints"]) {
        if (!value.is_object()) {
            if (error) *error = "Each entrypoint must be an object";
            return false;
        }
        PluginEntrypoint entrypoint;
        if (!readRequiredString(value, "os", entrypoint.os, error) ||
            !readRequiredString(value, "arch", entrypoint.arch, error) ||
            !readRequiredString(value, "path", entrypoint.path, error)) {
            return false;
        }
        static const std::set<std::string> supportedOs = {
            "windows", "macos", "linux",
        };
        static const std::set<std::string> supportedArch = {
            "arm64", "x86_64", "x86", "universal",
        };
        if (supportedOs.find(entrypoint.os) == supportedOs.end() ||
            supportedArch.find(entrypoint.arch) == supportedArch.end()) {
            if (error) *error = "Entrypoint has an unsupported OS or architecture";
            return false;
        }
        if (!validatePortablePath(entrypoint.path, false, error)) return false;
        if (!hasExpectedLibraryExtension(entrypoint)) {
            if (error) *error = "Entrypoint extension does not match its OS";
            return false;
        }
        if (!targets.insert({entrypoint.os, entrypoint.arch}).second) {
            if (error) *error = "Manifest contains duplicate platform entrypoints";
            return false;
        }
        manifest.entrypoints.push_back(std::move(entrypoint));
    }

    if (root.contains("documents")) {
        if (!root["documents"].is_object()) {
            if (error) *error = "Manifest documents must be an object";
            return false;
        }
        for (const auto& item : {
                 std::pair<const char*, std::optional<std::string>*>(
                     "readme", &manifest.documents.readme),
                 std::pair<const char*, std::optional<std::string>*>(
                     "license", &manifest.documents.license)}) {
            if (root["documents"].contains(item.first)) {
                if (!root["documents"][item.first].is_string()) {
                    if (error) *error = std::string("Document path '") + item.first +
                                        "' must be a string";
                    return false;
                }
                *item.second = root["documents"][item.first].get<std::string>();
                if (!validatePortablePath(**item.second, false, error)) return false;
            }
        }
    }
    return true;
}

std::optional<std::string> selectEntrypoint(const PluginManifest& manifest) {
    const std::string os = currentOs();
    const std::string arch = currentArch();
    for (const auto& entrypoint : manifest.entrypoints) {
        if (entrypoint.os == os && entrypoint.arch == arch) {
            return entrypoint.path;
        }
    }
    for (const auto& entrypoint : manifest.entrypoints) {
        if (entrypoint.os == os && entrypoint.arch == "universal") {
            return entrypoint.path;
        }
    }
    return std::nullopt;
}

} // namespace

PluginPackageInspection inspectPluginPackage(const std::string& packagePath) {
    PluginPackageInspection result;
    const fs::path path = fs::u8path(packagePath);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        result.message = "Plugin package is not a regular file";
        return result;
    }
    if (lowerAscii(path.extension().string()) != ".liberaplugin") {
        result.message = "Plugin package must use the .liberaplugin extension";
        return result;
    }
    const auto archiveSize = fs::file_size(path, ec);
    if (ec || archiveSize == 0 || archiveSize > maximumPackageBytes) {
        result.message = "Plugin package has an invalid size";
        return result;
    }

    OpenArchive opened;
    std::vector<ArchiveEntry> entries;
    std::unordered_map<std::string, mz_uint> filesByPath;
    if (!openAndValidateArchive(path, opened, entries, filesByPath,
                                &result.message)) {
        return result;
    }

    const auto manifestEntry = filesByPath.find("manifest.json");
    if (manifestEntry == filesByPath.end()) {
        result.message = "Package does not contain manifest.json at its root";
        return result;
    }
    if (filesByPath.find(std::string(preservedArchiveName)) != filesByPath.end()) {
        result.message = "Package contains the reserved archive.liberaplugin path";
        return result;
    }
    const auto manifestMeta = std::find_if(
        entries.begin(), entries.end(), [&](const ArchiveEntry& entry) {
            return entry.index == manifestEntry->second;
        });
    if (manifestMeta == entries.end() ||
        manifestMeta->uncompressedSize > maximumManifestBytes) {
        result.message = "Plugin manifest is too large";
        return result;
    }

    std::string manifestText(
        static_cast<std::size_t>(manifestMeta->uncompressedSize), '\0');
    if (!mz_zip_reader_extract_to_mem(&opened.archive,
                                      manifestEntry->second,
                                      manifestText.data(),
                                      manifestText.size(),
                                      0)) {
        result.message = "Failed to read plugin manifest";
        return result;
    }
    if (!parseManifest(manifestText, result.manifest, &result.message)) {
        return result;
    }

    for (const auto& entrypoint : result.manifest.entrypoints) {
        if (filesByPath.find(entrypoint.path) == filesByPath.end()) {
            result.message = "An entrypoint referenced by the manifest is missing";
            return result;
        }
    }

    const auto selected = selectEntrypoint(result.manifest);
    if (!selected) {
        result.message = "Package has no entrypoint for " + currentOs() +
                         "-" + currentArch();
        return result;
    }
    for (const auto* document : {&result.manifest.documents.readme,
                                 &result.manifest.documents.license}) {
        if (*document && filesByPath.find(**document) == filesByPath.end()) {
            result.message = "A document referenced by the manifest is missing";
            return result;
        }
    }

    result.packageSha256 = fileSha256(path, &result.message);
    if (result.packageSha256.empty()) return result;
    result.selectedEntrypoint = *selected;
    result.success = true;
    result.message = "OK: " + result.manifest.name + " " + result.manifest.version;
    return result;
}

bool extractPluginPackage(const std::string& packagePath,
                          const std::string& destinationDirectory,
                          PluginPackageInspection* inspection,
                          std::string* error) {
    PluginPackageInspection checked = inspectPluginPackage(packagePath);
    if (!checked.success) {
        if (error) *error = checked.message;
        return false;
    }

    const fs::path destination = fs::u8path(destinationDirectory);
    std::error_code ec;
    if (fs::exists(destination, ec)) {
        if (error) *error = "Extraction destination already exists";
        return false;
    }
    fs::create_directories(destination, ec);
    if (ec) {
        if (error) *error = "Failed to create extraction directory: " + ec.message();
        return false;
    }

    OpenArchive opened;
    std::vector<ArchiveEntry> entries;
    std::unordered_map<std::string, mz_uint> filesByPath;
    std::string archiveError;
    if (!openAndValidateArchive(packagePath, opened, entries, filesByPath,
                                &archiveError)) {
        fs::remove_all(destination, ec);
        if (error) *error = archiveError;
        return false;
    }

    for (const auto& entry : entries) {
        if (entry.directory) continue;
        const fs::path output = destination / fs::path(entry.path);
        fs::create_directories(output.parent_path(), ec);
        std::ofstream outputStream(output, std::ios::binary | std::ios::trunc);
        const auto writeOutput = [](void* opaque,
                                    mz_uint64 offset,
                                    const void* data,
                                    std::size_t size) -> std::size_t {
            auto* stream = static_cast<std::ofstream*>(opaque);
            if (static_cast<mz_uint64>(stream->tellp()) != offset) {
                return 0;
            }
            stream->write(static_cast<const char*>(data),
                          static_cast<std::streamsize>(size));
            return *stream ? size : 0;
        };
        const bool extracted = !ec && outputStream &&
            mz_zip_reader_extract_to_callback(&opened.archive,
                                              entry.index,
                                              writeOutput,
                                              &outputStream,
                                              0);
        outputStream.flush();
        if (!extracted || !outputStream) {
            const std::string reason = ec
                ? ec.message()
                : mz_zip_get_error_string(mz_zip_get_last_error(&opened.archive));
            fs::remove_all(destination, ec);
            if (error) *error = "Failed to extract " + entry.path + ": " + reason;
            return false;
        }
    }

    if (inspection) *inspection = std::move(checked);
    return true;
}

const char* pluginPackageExtension() {
    return "liberaplugin";
}

} // namespace libera::plugin
