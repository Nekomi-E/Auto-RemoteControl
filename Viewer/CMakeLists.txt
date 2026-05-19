set(VIEWER_SOURCES
    ViewerMain.cpp
    ViewerWindow.cpp
    ViewerSession.cpp
    Network/ViewerNetworkImpl.cpp
    Network/ViewerControlChannel.cpp
    Network/ViewerDataChannel.cpp
    Decoder/MfVideoDecoder.cpp
    Decoder/MfAudioDecoder.cpp
    Render/D3d11Renderer.cpp
    Render/D2dOverlay.cpp
    Render/WasapiAudioRenderer.cpp
    Input/RawInputCapture.cpp
)

add_executable(Viewer WIN32 ${VIEWER_SOURCES})
target_include_directories(Viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                          ${CMAKE_SOURCE_DIR})
target_link_libraries(Viewer PRIVATE
    CommonLib
    dxgi d3d11 d2d1 dwrite
    mfplat mf mfuuid
    strmiids
    ole32
    user32 gdi32
)
set_target_properties(Viewer PROPERTIES FOLDER "Viewer")
