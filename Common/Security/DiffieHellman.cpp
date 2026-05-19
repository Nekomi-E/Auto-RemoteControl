#include "DiffieHellman.h"
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

class DiffieHellman::Impl {
public:
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ready = false;
};

DiffieHellman::DiffieHellman() : m_impl(new Impl()) {
    BCryptOpenAlgorithmProvider(&m_impl->hAlg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0);
}

DiffieHellman::~DiffieHellman() {
    if (m_impl->hKey) BCryptDestroyKey(m_impl->hKey);
    if (m_impl->hAlg) BCryptCloseAlgorithmProvider(m_impl->hAlg, 0);
    delete m_impl;
}

bool DiffieHellman::GenerateKeyPair() {
    if (!m_impl->hAlg) return false;
    if (m_impl->hKey) {
        BCryptDestroyKey(m_impl->hKey);
        m_impl->hKey = nullptr;
    }
    NTSTATUS status = BCryptGenerateKeyPair(m_impl->hAlg, &m_impl->hKey, 256, 0);
    if (status != 0) return false;
    status = BCryptFinalizeKeyPair(m_impl->hKey, 0);
    if (status != 0) return false;
    m_impl->ready = true;
    return true;
}

std::vector<uint8_t> DiffieHellman::GetPublicKey() const {
    if (!m_impl->ready || !m_impl->hKey) return {};

    ULONG keySize = 0;
    BCryptExportKey(m_impl->hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &keySize, 0);
    std::vector<uint8_t> blob(keySize);
    BCryptExportKey(m_impl->hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), keySize, &keySize, 0);

    // BCRYPT_ECCKEY_BLOB: X and Y are at offset 8, 32 bytes each
    // Convert to uncompressed point: 0x04 || X || Y
    if (keySize < 72) return {};

    std::vector<uint8_t> pubKey(65);
    pubKey[0] = 0x04;
    memcpy(pubKey.data() + 1, blob.data() + 8, 32);   // X
    memcpy(pubKey.data() + 33, blob.data() + 40, 32);  // Y
    return pubKey;
}

std::vector<uint8_t> DiffieHellman::ComputeSharedSecret(const std::vector<uint8_t>& peerPublicKey) {
    if (!m_impl->ready || !m_impl->hKey || peerPublicKey.size() < 65) return {};
    if (peerPublicKey[0] != 0x04) return {}; // Only support uncompressed

    // Import peer's public key
    BCRYPT_KEY_HANDLE hPeerKey = nullptr;
    // Build ECC public blob from raw point
    const ULONG blobSize = sizeof(BCRYPT_ECCKEY_BLOB) + 32 + 32;
    std::vector<uint8_t> blob(blobSize, 0);
    BCRYPT_ECCKEY_BLOB* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = 32;

    uint8_t* keyData = blob.data() + sizeof(BCRYPT_ECCKEY_BLOB);
    memcpy(keyData, peerPublicKey.data() + 1, 32);      // X
    memcpy(keyData + 32, peerPublicKey.data() + 33, 32); // Y

    NTSTATUS status = BCryptImportKeyPair(m_impl->hAlg, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                           &hPeerKey, blob.data(), blobSize, 0);
    if (status != 0) return {};

    // Compute shared secret
    ULONG secretSize = 0;
    BCryptSecretAgreement(m_impl->hKey, hPeerKey, &m_impl->hKey, 0);
    // We need a different handle for the secret
    BCRYPT_SECRET_HANDLE hSecret = nullptr;
    status = BCryptSecretAgreement(m_impl->hKey, hPeerKey, &hSecret, 0);
    if (status != 0) {
        BCryptDestroyKey(hPeerKey);
        return {};
    }

    BCryptDeriveKey(hSecret, BCRYPT_KDF_RAW_SECRET, nullptr, nullptr, 0, &secretSize, 0);
    std::vector<uint8_t> secret(secretSize);
    status = BCryptDeriveKey(hSecret, BCRYPT_KDF_RAW_SECRET, nullptr, secret.data(), secretSize, &secretSize, 0);

    BCryptDestroySecret(hSecret);
    BCryptDestroyKey(hPeerKey);

    if (status != 0) return {};
    return secret;
}

std::vector<uint8_t> DiffieHellman::GetAesKey(const std::vector<uint8_t>& sharedSecret) {
    // Take first 32 bytes of the shared secret as AES-256 key
    if (sharedSecret.size() < 32) return {};
    return std::vector<uint8_t>(sharedSecret.begin(), sharedSecret.begin() + 32);
}
