#include "driver_archive_packer.h"
#include <sstream>

namespace malicompat {

std::string DriverArchivePacker::GenerateMetaJson(const DriverArchiveMeta& meta) {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"name\": \"" << meta.name << "\",\n"
       << "  \"description\": \"" << meta.description << "\",\n"
       << "  \"author\": \"" << meta.author << "\",\n"
       << "  \"vendor\": \"" << meta.vendor << "\",\n"
       << "  \"driverVersion\": \"" << meta.driverVersion << "\",\n"
       << "  \"libraryName\": \"" << meta.libraryName << "\",\n"
       << "  \"minApi\": " << meta.minApi << ",\n"
       << "  \"schemaVersion\": " << meta.schemaVersion << "\n"
       << "}\n";
    return ss.str();
}

DriverArchiveMeta DriverArchivePacker::CreateDefaultMeta() {
    DriverArchiveMeta meta;
    meta.name = "MaliCompat-G71-v1.0";
    meta.description = "MaliCompat Vulkan compatibility layer for ARM Mali-G71 / Bifrost";
    meta.author = "CodeAbhi826";
    meta.vendor = "MaliCompat";
    meta.driverVersion = "1.0.0-compat";
    meta.libraryName = "libmalicompat.so";
    meta.minApi = 28;
    meta.schemaVersion = 1;
    return meta;
}

} // namespace malicompat
