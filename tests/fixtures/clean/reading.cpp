#include <cstdint>
#include <optional>
#include <string>

namespace power {

struct ChargeReading {
    std::uint8_t percent{0};
    bool discharging{false};
};

// Parses the "<percent> <state>" line reported by the power service. An empty result means the
// service reported nothing, and the caller renders the empty state instead of a stand-in value.
std::optional<ChargeReading> parse_reading(const std::string& line) {
    const auto separator = line.find(' ');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    ChargeReading reading;
    reading.percent = static_cast<std::uint8_t>(std::stoi(line.substr(0, separator)));
    reading.discharging = line.substr(separator + 1) == "discharging";
    return reading;
}

}  // namespace power
