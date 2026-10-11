#include "driver_archive_packer.h"
#include <sstream>
#include <fstream>
#include <vector>
#include <cstring>

namespace malicompat {

namespace {

uint32_t ComputeCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

void WriteLE16(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

void WriteLE32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

struct ZipEntry {
    std::string filename;
    std::vector<uint8_t> data;
    uint32_t crc{0};
    uint32_t localHeaderOffset{0};
};

} // namespace

std::string DriverArchivePacker::GenerateMetaJson(const DriverArchiveMeta& meta) {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"schemaVersion\": " << meta.schemaVersion << ",\n"
       << "  \"name\": \"" << meta.name << "\",\n"
       << "  \"description\": \"" << meta.description << "\",\n"
       << "  \"author\": \"" << meta.author << "\",\n"
       << "  \"packageVersion\": \"" << meta.packageVersion << "\",\n"
       << "  \"vendor\": \"" << meta.vendor << "\",\n"
       << "  \"driverVersion\": \"" << meta.driverVersion << "\",\n"
       << "  \"minApi\": " << meta.minApi << ",\n"
       << "  \"libraryName\": \"" << meta.libraryName << "\"\n"
       << "}\n";
    return ss.str();
}

std::string DriverArchivePacker::GenerateDriverJson(const DriverJsonConfig& config) {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"name\": \"" << config.name << "\",\n"
       << "  \"libraryName\": \"" << config.libraryName << "\",\n"
       << "  \"folder\": \"" << config.folder << "\"\n"
       << "}\n";
    return ss.str();
}

std::string DriverArchivePacker::GenerateLibreriaFile(const std::string& libraryName) {
    return libraryName + "\n";
}

DriverArchiveMeta DriverArchivePacker::CreateDefaultMeta() {
    DriverArchiveMeta meta;
    meta.schemaVersion = 1;
    meta.name = "MaliCompat (Mali-G71)";
    meta.description = "Vulkan translation shim and descriptor virtualizer for ARM Mali-G71 Bifrost";
    meta.author = "CodeAbhi826";
    meta.packageVersion = "1.0";
    meta.vendor = "MaliCompat";
    meta.driverVersion = "1.0.0-compat";
    meta.libraryName = "libmalicompat.so";
    meta.minApi = 28;
    return meta;
}

DriverJsonConfig DriverArchivePacker::CreateDefaultDriverJson() {
    DriverJsonConfig config;
    config.name = "MaliCompat-G71";
    config.libraryName = "libmalicompat.so";
    config.folder = "payload";
    return config;
}

bool DriverArchivePacker::CreateDriverBundle(const std::string& outputZipPath, const std::string& soFilePath, const DriverArchiveMeta& meta) {
    std::vector<ZipEntry> entries;

    // 1. meta.json
    {
        std::string metaStr = GenerateMetaJson(meta);
        ZipEntry e;
        e.filename = "meta.json";
        e.data.assign(metaStr.begin(), metaStr.end());
        e.crc = ComputeCrc32(e.data.data(), e.data.size());
        entries.push_back(std::move(e));
    }

    // 2. driver.json
    {
        DriverJsonConfig config;
        config.name = meta.name;
        config.libraryName = meta.libraryName;
        config.folder = "payload";
        std::string driverStr = GenerateDriverJson(config);
        ZipEntry e;
        e.filename = "driver.json";
        e.data.assign(driverStr.begin(), driverStr.end());
        e.crc = ComputeCrc32(e.data.data(), e.data.size());
        entries.push_back(std::move(e));
    }

    // 3. payload/libmalicompat.so
    {
        ZipEntry e;
        e.filename = "payload/" + meta.libraryName;
        std::ifstream soFile(soFilePath, std::ios::binary);
        if (soFile.is_open()) {
            e.data.assign(std::istreambuf_iterator<char>(soFile), std::istreambuf_iterator<char>());
        }
        if (e.data.empty()) {
            // Write placeholder ELF header if real so is not yet present on host
            e.data = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        }
        e.crc = ComputeCrc32(e.data.data(), e.data.size());
        entries.push_back(std::move(e));
    }

    // Construct ZIP file stream
    std::vector<uint8_t> zipData;

    // Write Local File Headers + Data
    for (auto& entry : entries) {
        entry.localHeaderOffset = static_cast<uint32_t>(zipData.size());

        WriteLE32(zipData, 0x04034b50); // Local header signature
        WriteLE16(zipData, 20);         // Version needed: 2.0
        WriteLE16(zipData, 0);          // General purpose bit flag
        WriteLE16(zipData, 0);          // Compression method: Store (0)
        WriteLE16(zipData, 0);          // File last modification time
        WriteLE16(zipData, 0);          // File last modification date
        WriteLE32(zipData, entry.crc);
        WriteLE32(zipData, static_cast<uint32_t>(entry.data.size())); // Compressed size
        WriteLE32(zipData, static_cast<uint32_t>(entry.data.size())); // Uncompressed size
        WriteLE16(zipData, static_cast<uint16_t>(entry.filename.size()));
        WriteLE16(zipData, 0);          // Extra field length

        zipData.insert(zipData.end(), entry.filename.begin(), entry.filename.end());
        zipData.insert(zipData.end(), entry.data.begin(), entry.data.end());
    }

    uint32_t centralDirOffset = static_cast<uint32_t>(zipData.size());

    // Write Central Directory File Headers
    for (const auto& entry : entries) {
        WriteLE32(zipData, 0x02014b50); // Central directory header signature
        WriteLE16(zipData, 20);         // Version made by: 2.0
        WriteLE16(zipData, 20);         // Version needed: 2.0
        WriteLE16(zipData, 0);          // General purpose bit flag
        WriteLE16(zipData, 0);          // Compression method: Store
        WriteLE16(zipData, 0);          // File last modification time
        WriteLE16(zipData, 0);          // File last modification date
        WriteLE32(zipData, entry.crc);
        WriteLE32(zipData, static_cast<uint32_t>(entry.data.size()));
        WriteLE32(zipData, static_cast<uint32_t>(entry.data.size()));
        WriteLE16(zipData, static_cast<uint16_t>(entry.filename.size()));
        WriteLE16(zipData, 0);          // Extra field length
        WriteLE16(zipData, 0);          // File comment length
        WriteLE16(zipData, 0);          // Disk number start
        WriteLE16(zipData, 0);          // Internal file attributes
        WriteLE32(zipData, 0);          // External file attributes
        WriteLE32(zipData, entry.localHeaderOffset);

        zipData.insert(zipData.end(), entry.filename.begin(), entry.filename.end());
    }

    uint32_t centralDirSize = static_cast<uint32_t>(zipData.size() - centralDirOffset);

    // Write End of Central Directory Record
    WriteLE32(zipData, 0x06054b50); // EOCD signature
    WriteLE16(zipData, 0);          // Number of this disk
    WriteLE16(zipData, 0);          // Disk with start of central directory
    WriteLE16(zipData, static_cast<uint16_t>(entries.size())); // Total entries on disk
    WriteLE16(zipData, static_cast<uint16_t>(entries.size())); // Total entries
    WriteLE32(zipData, centralDirSize);
    WriteLE32(zipData, centralDirOffset);
    WriteLE16(zipData, 0);          // ZIP comment length

    // Write out to outputZipPath
    std::ofstream out(outputZipPath, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(zipData.data()), zipData.size());
    return out.good();
}

} // namespace malicompat
