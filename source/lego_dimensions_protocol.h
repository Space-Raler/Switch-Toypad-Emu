// LEGO Dimensions Toy Pad wire-protocol primitives.
// Research reference: SegerEnd/Flipper-Zero-LD-Toypad-Emulator and
// woodenphone/lego_dimensions_protocol. This module is deliberately transport-agnostic.
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace sky::lego {
constexpr size_t kReportSize = 32;
constexpr size_t kPositionCount = 7;
using Report = std::array<uint8_t, kReportSize>;

enum class Pad : uint8_t { All = 0, Center = 1, Left = 2, Right = 3 };
enum class Colour : uint8_t { Off = 0, Red = 1, Green = 2, Blue = 3, Yellow = 4, Cyan = 5, Magenta = 6, White = 7 };

uint8_t checksum(const uint8_t* data, size_t len);
// Reference host-to-pad initialization payload (not an unsolicited IN report).
Report initReport();
Report colourReport(Pad pad, uint8_t r, uint8_t g, uint8_t b);
// Emits the 32-byte tag-arrival/removal notification used by the Flipper reference.
// UID must be the seven-byte UID from a user's own tag dump.
Report tokenReport(Pad pad, uint8_t tokenIndex, bool placed, const std::array<uint8_t, 7>& uid);
} // namespace sky::lego
