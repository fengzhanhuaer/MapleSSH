#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace maple {
using Bytes = std::vector<std::uint8_t>;
std::string RandomId();
Bytes Encrypt(std::string_view plaintext, std::string_view password, std::string_view purpose);
std::string Decrypt(std::span<const std::uint8_t> encrypted, std::string_view password, std::string_view purpose);
std::string Sha256(std::span<const std::uint8_t> content);
}
