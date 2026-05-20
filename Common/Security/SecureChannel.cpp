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
    std::vector<uint8_t> result;
    EncryptFrameOut(header, payload, payloadLen, result);
    return result;
}

bool SecureChannel::EncryptFrameOut(const Protocol::FrameHeader& header,
                                     const uint8_t* payload, size_t payloadLen,
                                     std::vector<uint8_t>& outWireData) {
    if (!m_aes.IsReady()) return false;

    // Reuse IV buffer
    m_iv.resize(AesGcm::IV_SIZE);
    BCryptGenRandom(nullptr, m_iv.data(), static_cast<ULONG>(m_iv.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    // Serialize header for AAD
    auto headerBytes = header.serialize();

    // Encrypt with IV and AAD
    auto encrypted = m_aes.EncryptWithIv(payload, payloadLen,
                                          m_iv.data(), m_iv.size(),
                                          headerBytes.data(), headerBytes.size());
    if (encrypted.empty()) return false;

    // Assemble wire format: [IV][ciphertext][tag] in output buffer
    size_t totalSize = m_iv.size() + encrypted.size();
    outWireData.resize(totalSize);
    memcpy(outWireData.data(), m_iv.data(), m_iv.size());
    memcpy(outWireData.data() + m_iv.size(), encrypted.data(), encrypted.size());

    m_encryptedCount++;
    return true;
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
