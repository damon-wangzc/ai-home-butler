/**
 * @file ws_transport_shim.h
 * @brief Compatibility shim for esp_websocket_client 1.7.0 on IDF 5.5
 *
 * esp_websocket_client 1.7.0 added WebSocket redirect handling and calls
 * esp_transport_ws_get_redir_uri(), which was added in esp-protocols but
 * is NOT present in IDF 5.5's bundled TCP transport layer.
 *
 * This file is force-included into the managed component's compilation via
 * the project CMakeLists.txt. Returning NULL disables redirect following,
 * which is safe for local AI-server connections that never issue 3xx redirects.
 */
#pragma once

/* Replace the missing function call with a NULL literal (no redirect). */
#define esp_transport_ws_get_redir_uri(transport)  ((const char *)NULL)
