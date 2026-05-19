#pragma once
#include <vector>
#include <cstdint>

// ECDH key exchange using P-256 curve via BCrypt
class DiffieHellman {
public:
    DiffieHellman();
    ~DiffieHellman();

    bool GenerateKeyPair();

    // Export the public key in raw format (65 bytes: 0x04 || X || Y)
    std::vector<uint8_t> GetPublicKey() const;

    // Import peer's public key and compute shared secret
    std::vector<uint8_t> ComputeSharedSecret(const std::vector<uint8_t>& peerPublicKey);

    // Get the shared secret as a 32-byte key for AES-256
    std::vector<uint8_t> GetAesKey(const std::vector<uint8_t>& sharedSecret);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
