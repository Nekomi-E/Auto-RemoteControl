#include "SecureChannel.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>

SecureChannel::SecureChannel() {}

bool SecureChannel::Initialize(const std::vector<uint8_t>& aesKey) {
    return m_aes.SetKey(aesKey);
}

std::vector<uint8_t> SecureChannel::EncryptFrame(const Protocol::FrameHeader& header,
                                                   const uint8_t* payload, size_t payloadLen) {
    if (!m_aes.IsReady()) return {};

    // Generate random IV
    std::vector<uint8_t> iv(AesGcm::IV_SIZE);
    BCryptGenRandom(nullptr, iv.data(), static_cast<ULONG>(iv.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    // Serialize header for AAD
    auto headerBytes = header.serialize();

    // Encrypt with IV and AAD
    auto encrypted = m_aes.EncryptWithIv(payload, payloadLen,
                                          iv.data(), iv.size(),
                                          headerBytes.data(), headerBytes.size());
    if (encrypted.empty()) return {};

    // Prepend IV to form wire format: [IV][ciphertext][tag]
    std::vector<uint8_t> result;
    result.reserve(iv.size() + encrypted.size());
    result.insert(result.end(), iv.begin(), iv.end());
    result.insert(result.end(), encrypted.begin(), encrypted.end());

    m_encryptedCount++;
    return result;
}

bool SecureChannel::DecryptFrame(const uint8_t* wireData, size_t wireLen,
                                   const Protocol::FrameHeader& header,
                                   std::vector<uint8_t>& plaintext) {
    if (!m_aes.IsReady() || wireLen < AesGcm::IV_SIZE + AesGcm::TAG_SIZE) return false;

    auto headerBytes = header.serialize();

    const uint8_t* iv = wireData;
    const uint8_t* ciphertext = wireData + AesGcm::IV_SIZE;
    size_t cipherLen = wireLen - AesGcm::IV_SIZE;

    bool ok = m_aes.DecryptWithIv(ciphertext, cipherLen,
                                    iv, AesGcm::IV_SIZE,
                                    headerBytes.data(), headerBytes.size(),
                                    plaintext);
    if (ok) {
        m_decryptedCount++;
    } else {
        m_authFailCount++;
    }
    return ok;
}
