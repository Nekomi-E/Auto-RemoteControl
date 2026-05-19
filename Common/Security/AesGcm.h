#pragma once
#include <vector>
#include <cstdint>
#include <string>

class AesGcm {
public:
    AesGcm();
    ~AesGcm();

    bool SetKey(const std::vector<uint8_t>& key); // 32 bytes for AES-256
    bool SetKey(const uint8_t* key, size_t len);

    // Encrypt plaintext with random IV. Returns IV + ciphertext + tag.
    std::vector<uint8_t> Encrypt(const uint8_t* plaintext, size_t len);
    std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plaintext);

    // Decrypt. Input: IV + ciphertext + tag. Output: plaintext.
    // IV is first 12 bytes, tag is last 16 bytes.
    bool Decrypt(const uint8_t* ciphertext, size_t len, std::vector<uint8_t>& plaintext);
    bool Decrypt(const std::vector<uint8_t>& ciphertext, std::vector<uint8_t>& plaintext);

    // Encrypt with explicit IV (for use with FrameHeader as AAD)
    std::vector<uint8_t> EncryptWithIv(const uint8_t* plaintext, size_t plainLen,
                                        const uint8_t* iv, size_t ivLen,
                                        const uint8_t* aad, size_t aadLen);

    // Decrypt with explicit IV and AAD
    bool DecryptWithIv(const uint8_t* ciphertext, size_t cipherLen,
                       const uint8_t* iv, size_t ivLen,
                       const uint8_t* aad, size_t aadLen,
                       std::vector<uint8_t>& plaintext);

    bool IsReady() const { return m_keySet; }

    static constexpr size_t IV_SIZE = 12;
    static constexpr size_t TAG_SIZE = 16;
    static constexpr size_t KEY_SIZE = 32;

private:
    bool m_keySet = false;
    struct Impl;
    Impl* m_impl = nullptr;
    void* m_hAlg = nullptr;
    void* m_hKey = nullptr;
};
