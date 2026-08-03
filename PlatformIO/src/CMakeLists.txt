# PlatformIO registers this directory as the project's "main" component, which
# in ESP-IDF implicitly depends on every other component — so the managed
# components (esp_peer, esp32-camera) and the Arduino component need no
# explicit REQUIRES here.
idf_component_register(
    SRCS "main.cpp" "anedya_signaling.cpp" "webrtc_peer.cpp"
    INCLUDE_DIRS "." "../include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
