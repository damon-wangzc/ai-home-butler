cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai_orb)

# ── Compatibility shim ────────────────────────────────────────────────────────
# esp_websocket_client 1.7.0 calls esp_transport_ws_get_redir_uri(), which is
# absent from IDF 5.5's bundled TCP transport. Force-include a header that
# replaces the call with ((const char*)NULL) so redirect handling compiles away.
if(TARGET __idf_espressif__esp_websocket_client)
    target_compile_options(__idf_espressif__esp_websocket_client PRIVATE
        "-include" "${CMAKE_SOURCE_DIR}/main/ws_transport_shim.h"
    )
endif()
