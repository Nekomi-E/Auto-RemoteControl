#include "Authenticator.h"
#include <windows.h>
#include <bcrypt.h>
#include <random>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "bcrypt.lib")

Authenticator::Authenticator(Role role, const std::string& password)
    : m_role(role), m_password(password) {}

std::string Authenticator::GenerateChallenge() {
    m_state = State::ChallengeSent;
    std::vector<uint8_t> random(32);
    BCryptGenRandom(nullptr, random.data(), 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BytesToHex(random);
}

std::string Authenticator::ComputeResponse(const std::string& challenge) {
    auto challengeBytes = HexToBytes(challenge);
    std::vector<uint8_t> key(m_password.begin(), m_password.end());
    auto hmac = ComputeHmacSha256(key, challengeBytes);
    return BytesToHex(hmac);
}

bool Authenticator::VerifyResponse(const std::string& challenge, const std::string& response) {
    std::string expected = ComputeResponse(challenge);
    if (expected == response) {
        m_state = State::Authenticated;
        return true;
    }
    m_state = State::Failed;
    return false;
}

std::vector<uint8_t> Authenticator::ComputeHmacSha256(const std::vector<uint8_t>& key,
                                                       const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<uint8_t> hash(32);
    ULONG hashLen = 0;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                     BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return {};

    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                         (PUCHAR)key.data(), static_cast<ULONG>(key.size()), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    BCryptHashData(hHash, (PUCHAR)data.data(), static_cast<ULONG>(data.size()), 0);
    BCryptFinishHash(hHash, hash.data(), 32, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return hash;
}

std::string Authenticator::BytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (auto b : bytes) ss << std::setw(2) << static_cast<int>(b);
    return ss.str();
}

std::vector<uint8_t> Authenticator::HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        std::string byteStr = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(strtoul(byteStr.c_str(), nullptr, 16)));
    }
    return bytes;
}
