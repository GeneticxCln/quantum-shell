#include <cstdint>
#include <filesystem>

std::uintmax_t size_or_zero(const std::filesystem::path& file) {
    try {
        return std::filesystem::file_size(file);
    } catch (const std::filesystem::filesystem_error&) {}
    return 0;
}
