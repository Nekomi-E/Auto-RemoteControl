#pragma once
#include "AesGcm.h"
#include "../Protocol/FrameHeader.h"
#include <vector>
#include <cstdint>
#include <atomic>

// Per-UDP-packet encryption using AES-256-GCM.
// Each frame is encrypted with a random IV, and the FrameHeader is used as
// Additional Authenticated Data (AAD) for integrity protection.
class SecureChannel {
public:
    SecureChannel();

    bool Initialize(const std::vector<uint8_t>& aesKey);

    // Encrypt a frame payload. Returns the encrypted wire format:
    // [IV 12 bytes][ciphertext][GCM tag 16 bytes]
    // The FrameHeader is used as AAD (authenticated but not encrypted).
    std::vector<uint8_t> EncryptFrame(const Protocol::FrameHeader& header,
                                       const uint8_t* payload, size_t payloadLen);

    // Fill outWireData instead of returning a new vector (avoids per-frame allocation).
    bool EncryptFrameOut(const Protocol::FrameHeader& header,
                         const uint8_t* payload, size_t payloadLen,
                         std::vector<uint8_t>& outWireData);

    // Decrypt a frame from wire format.
    // Input: data from network (IV + ciphertext + tag)
    // header: parsed FrameHeader used as AAD
    // Returns false on auth failure (tampering detected).
    bool DecryptFrame(const uint8_t* wireData, size_t wireLen,
                      const Protocol::FrameHeader& header,
                      std::vector<uint8_t>& plaintext);

    bool IsReady() const { return m_aes.IsReady(); }

private:
    AesGcm m_aes;
    std::vector<uint8_t> m_iv;      // reusable IV buffer (12 bytes)
    std::atomic<uint64_t> m_encryptedCount{0};
    std::atomic<uint64_t> m_decryptedCount{0};
    std::atomic<uint64_t> m_authFailCount{0};

public:
    uint64_t GetEncryptedCount() const { return m_encryptedCount; }
    uint64_t GetDecryptedCount() const { return m_decryptedCount; }
    uint64_t GetAuthFailCount() const { return m_authFailCount; }
};
