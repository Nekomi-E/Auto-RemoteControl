set(AGENT_SOURCES
    AgentMain.cpp
    AgentSession.cpp
    Capture/DxgiScreenCapture.cpp
    Capture/WasapiAudioCapture.cpp
    Capture/CaptureManager.cpp
    Encoder/MfVideoEncoder.cpp
    Encoder/MfAudioEncoder.cpp
    Encoder/EncoderManager.cpp
    Network/AgentNetworkImpl.cpp
    Network/ControlChannel.cpp
    Network/DataChannel.cpp
    Input/InputInjectorImpl.cpp
)

add_executable(Agent ${AGENT_SOURCES})
target_include_directories(Agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                         ${CMAKE_SOURCE_DIR})
target_link_libraries(Agent PRIVATE
    CommonLib
    dxgi d3d11
    mfplat mf mfuuid
    strmiids
    ole32
    user32 gdi32
    winmm
)
set_target_properties(Agent PROPERTIES FOLDER "Agent")
