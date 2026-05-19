#include "AesGcm.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

class AesGcm::Impl {
public:
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ready = false;
};

AesGcm::AesGcm() : m_impl(new Impl()) {
    NTSTATUS status = BCryptOpenAlgorithmProvider(&m_impl->hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (status == 0) {
        BCryptSetProperty(m_impl->hAlg, BCRYPT_CHAINING_MODE,
                         (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    }
}

AesGcm::~AesGcm() {
    if (m_impl->hKey) BCryptDestroyKey(m_impl->hKey);
    if (m_impl->hAlg) BCryptCloseAlgorithmProvider(m_impl->hAlg, 0);
    delete m_impl;
}

bool AesGcm::SetKey(const std::vector<uint8_t>& key) {
    return SetKey(key.data(), key.size());
}

bool AesGcm::SetKey(const uint8_t* key, size_t len) {
    if (len < KEY_SIZE || !m_impl->hAlg) return false;

    if (m_impl->hKey) {
        BCryptDestroyKey(m_impl->hKey);
        m_impl->hKey = nullptr;
    }

    NTSTATUS status = BCryptGenerateSymmetricKey(m_impl->hAlg, &m_impl->hKey, nullptr, 0,
                                                  (PUCHAR)key, static_cast<ULONG>(KEY_SIZE), 0);
    if (status != 0) return false;
    m_keySet = true;
    m_impl->ready = true;
    return true;
}

std::vector<uint8_t> AesGcm::Encrypt(const uint8_t* plaintext, size_t len) {
    // Generate random IV
    std::vector<uint8_t> iv(IV_SIZE);
    BCryptGenRandom(nullptr, iv.data(), IV_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    auto enc = EncryptWithIv(plaintext, len, iv.data(), iv.size(), nullptr, 0);
    // Prepend IV
    std::vector<uint8_t> result;
    result.reserve(IV_SIZE + enc.size());
    result.insert(result.end(), iv.begin(), iv.end());
    result.insert(result.end(), enc.begin(), enc.end());
    return result;
}

std::vector<uint8_t> AesGcm::Encrypt(const std::vector<uint8_t>& plaintext) {
    return Encrypt(plaintext.data(), plaintext.size());
}

bool AesGcm::Decrypt(const uint8_t* ciphertext, size_t len, std::vector<uint8_t>& plaintext) {
    if (len < IV_SIZE + TAG_SIZE) return false;
    return DecryptWithIv(ciphertext + IV_SIZE, len - IV_SIZE,
                         ciphertext, IV_SIZE,
                         nullptr, 0,
                         plaintext);
}

bool AesGcm::Decrypt(const std::vector<uint8_t>& ciphertext, std::vector<uint8_t>& plaintext) {
    return Decrypt(ciphertext.data(), ciphertext.size(), plaintext);
}

std::vector<uint8_t> AesGcm::EncryptWithIv(const uint8_t* plaintext, size_t plainLen,
                                            const uint8_t* iv, size_t ivLen,
                                            const uint8_t* aad, size_t aadLen) {
    if (!m_keySet || !m_impl->hKey || ivLen < IV_SIZE) return {};

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = (PUCHAR)iv;
    authInfo.cbNonce = static_cast<ULONG>(IV_SIZE);
    authInfo.pbAuthData = (PUCHAR)aad;
    authInfo.cbAuthData = static_cast<ULONG>(aadLen);

    std::vector<uint8_t> cipherBuf(plainLen);
    ULONG cipherLen = 0;
    memcpy(cipherBuf.data(), plaintext, plainLen);

    std::vector<uint8_t> tag(TAG_SIZE);

    authInfo.pbTag = tag.data();
    authInfo.cbTag = TAG_SIZE;

    NTSTATUS status = BCryptEncrypt(m_impl->hKey, (PUCHAR)cipherBuf.data(), static_cast<ULONG>(plainLen),
                                     &authInfo, nullptr, 0,
                                     (PUCHAR)cipherBuf.data(), static_cast<ULONG>(plainLen),
                                     &cipherLen, 0);
    if (status != 0) return {};

    cipherBuf.resize(cipherLen);
    std::vector<uint8_t> result;
    result.reserve(cipherLen + TAG_SIZE);
    result.insert(result.end(), cipherBuf.begin(), cipherBuf.end());
    result.insert(result.end(), tag.begin(), tag.end());
    return result;
}

bool AesGcm::DecryptWithIv(const uint8_t* ciphertext, size_t cipherLen,
                            const uint8_t* iv, size_t ivLen,
                            const uint8_t* aad, size_t aadLen,
                            std::vector<uint8_t>& plaintext) {
    if (!m_keySet || !m_impl->hKey || ivLen < IV_SIZE || cipherLen < TAG_SIZE) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = (PUCHAR)iv;
    authInfo.cbNonce = static_cast<ULONG>(IV_SIZE);
    authInfo.pbAuthData = (PUCHAR)aad;
    authInfo.cbAuthData = static_cast<ULONG>(aadLen);

    size_t dataLen = cipherLen - TAG_SIZE;
    plaintext.resize(dataLen);
    memcpy(plaintext.data(), ciphertext, dataLen);

    authInfo.pbTag = (PUCHAR)(ciphertext + dataLen);
    authInfo.cbTag = TAG_SIZE;

    ULONG plainLen = 0;
    NTSTATUS status = BCryptDecrypt(m_impl->hKey, (PUCHAR)plaintext.data(), static_cast<ULONG>(dataLen),
                                     &authInfo, nullptr, 0,
                                     (PUCHAR)plaintext.data(), static_cast<ULONG>(dataLen),
                                     &plainLen, 0);
    if (status != 0) {
        plaintext.clear();
        return false;
    }
    plaintext.resize(plainLen);
    return true;
}
