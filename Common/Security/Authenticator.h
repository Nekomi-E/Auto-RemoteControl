#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Challenge-response authentication using SHA-256
// Agent generates challenge -> Viewer responds with HMAC-SHA256(challenge, password)
class Authenticator {
public:
    enum class State { Idle, ChallengeSent, Authenticated, Failed };
    enum class Role { Agent, Viewer };

    Authenticator(Role role, const std::string& password);

    // Agent: generate a challenge
    std::string GenerateChallenge();

    // Viewer: compute response to challenge
    std::string ComputeResponse(const std::string& challenge);

    // Agent: verify the response from Viewer
    bool VerifyResponse(const std::string& challenge, const std::string& response);

    State GetState() const { return m_state; }
    const std::string& GetPassword() const { return m_password; }

    static std::vector<uint8_t> ComputeHmacSha256(const std::vector<uint8_t>& key,
                                                   const std::vector<uint8_t>& data);
    static std::string BytesToHex(const std::vector<uint8_t>& bytes);
    static std::vector<uint8_t> HexToBytes(const std::string& hex);

private:
    Role m_role;
    std::string m_password;
    State m_state = State::Idle;
};
