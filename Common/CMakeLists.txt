set(COMMON_SOURCES
    Protocol/FrameHeader.cpp
    Protocol/ControlMessage.cpp
    Security/AesGcm.cpp
    Security/DiffieHellman.cpp
    Security/Authenticator.cpp
    Security/SecureChannel.cpp
    Utils/Logger.cpp
    Utils/Config.cpp
    Utils/Timer.cpp
    Utils/DebugScreenshot.cpp
)

add_library(CommonLib STATIC ${COMMON_SOURCES})
target_include_directories(CommonLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(CommonLib PUBLIC ws2_32 bcrypt ncrypt)
set_target_properties(CommonLib PROPERTIES FOLDER "Common")
