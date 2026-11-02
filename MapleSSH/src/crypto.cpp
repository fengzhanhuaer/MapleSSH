#include "crypto.hpp"
#include "model.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <array>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace maple {
namespace {
constexpr std::size_t HeaderSize = 44;
constexpr std::size_t TagSize = 16;
constexpr std::array<std::uint8_t, 8> Magic{'M','S','V','L','T','0','0','1'};
struct Key {
    std::array<unsigned char, 32> bytes{};
    ~Key() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};
void Check(int result) {
    if (result != 1) throw std::runtime_error("加密操作失败");
}
void Derive(Key& key, std::string_view password, const std::uint8_t* salt) {
    if (password.empty() || password.size() > 65536) throw std::runtime_error("主密码为空或过长");
    Check(EVP_PBE_scrypt(password.data(), password.size(), salt, 16, 131072, 8, 1,
        256ULL * 1024 * 1024, key.bytes.data(), key.bytes.size()));
}
using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
Context NewContext() {
    Context context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context) throw std::runtime_error("无法分配加密上下文");
    return context;
}
std::string Hex(std::span<const unsigned char> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    for (const auto byte : bytes) { output += digits[byte >> 4]; output += digits[byte & 15]; }
    return output;
}
}
std::string RandomId() {
    std::array<unsigned char, 16> bytes{};
    Check(RAND_bytes(bytes.data(), static_cast<int>(bytes.size())));
    return Hex(bytes);
}
Bytes Encrypt(std::string_view plaintext, std::string_view password, std::string_view purpose) {
    if (plaintext.size() > MaxFileBytes - HeaderSize - TagSize || purpose.size() > 32)
        throw std::runtime_error("加密内容过大");
    Bytes output(HeaderSize + plaintext.size() + TagSize);
    std::copy(Magic.begin(), Magic.end(), output.begin());
    Check(RAND_bytes(output.data() + 8, 28));
    for (unsigned int offset = 0; offset < 8; ++offset)
        output[36 + offset] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(plaintext.size()) >> (offset * 8));
    Key key;
    Derive(key, password, output.data() + 8);
    auto context = NewContext();
    Check(EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key.bytes.data(), output.data() + 24));
    int written = 0;
    Check(EVP_EncryptUpdate(context.get(), nullptr, &written, output.data(), static_cast<int>(HeaderSize)));
    Check(EVP_EncryptUpdate(context.get(), nullptr, &written, reinterpret_cast<const unsigned char*>(purpose.data()), static_cast<int>(purpose.size())));
    Check(EVP_EncryptUpdate(context.get(), output.data() + HeaderSize, &written,
        reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())));
    int final_bytes = 0;
    Check(EVP_EncryptFinal_ex(context.get(), output.data() + HeaderSize + written, &final_bytes));
    Check(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(TagSize), output.data() + HeaderSize + plaintext.size()));
    return output;
}
std::string Decrypt(std::span<const std::uint8_t> encrypted, std::string_view password, std::string_view purpose) {
    if (encrypted.size() < HeaderSize + TagSize || encrypted.size() > MaxFileBytes ||
        purpose.size() > 32 || !std::equal(Magic.begin(), Magic.end(), encrypted.begin()))
        throw std::runtime_error("加密文件格式无效");
    std::uint64_t length = 0;
    for (unsigned int offset = 0; offset < 8; ++offset) length |= static_cast<std::uint64_t>(encrypted[36 + offset]) << (offset * 8);
    if (length != encrypted.size() - HeaderSize - TagSize) throw std::runtime_error("加密文件长度无效");
    Key key;
    Derive(key, password, encrypted.data() + 8);
    auto context = NewContext();
    Check(EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key.bytes.data(), encrypted.data() + 24));
    int written = 0;
    Check(EVP_DecryptUpdate(context.get(), nullptr, &written, encrypted.data(), static_cast<int>(HeaderSize)));
    Check(EVP_DecryptUpdate(context.get(), nullptr, &written, reinterpret_cast<const unsigned char*>(purpose.data()), static_cast<int>(purpose.size())));
    std::string output(static_cast<std::size_t>(length) + 16, '\0');
    Check(EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(output.data()), &written,
        encrypted.data() + HeaderSize, static_cast<int>(length)));
    Check(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(TagSize),
        const_cast<std::uint8_t*>(encrypted.data() + HeaderSize + length)));
    int final_bytes = 0;
    if (EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(output.data()) + written, &final_bytes) != 1) {
        OPENSSL_cleanse(output.data(), output.size());
        throw std::runtime_error("主密码错误或加密文件已损坏");
    }
    output.resize(static_cast<std::size_t>(written + final_bytes));
    return output;
}
std::string Sha256(std::span<const std::uint8_t> content) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    Check(EVP_Digest(content.data(), content.size(), digest.data(), &size, EVP_sha256(), nullptr));
    return Hex(std::span(digest.data(), size));
}
}
