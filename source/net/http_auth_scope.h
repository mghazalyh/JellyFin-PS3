#pragma once
#include <stddef.h>
#include <string.h>

// Only an authenticated request in the configured Jellyfin API scope can
// invalidate its saved session. Sidecars and stale worker tokens cannot.
static inline bool jf_auth_expiry_scope(const char *url, const char *server,
                                        const char *request_token,
                                        const char *current_token) {
    if (!url || !server || !server[0] || !request_token || !request_token[0] ||
        !current_token || !current_token[0] ||
        strcmp(request_token, current_token) != 0) return false;
    if (strncmp(server, "http://", 7) != 0 &&
        strncmp(server, "https://", 8) != 0) return false;
    if (strchr(server, '?') || strchr(server, '#')) return false;
    size_t n = strlen(server);
    while (n && server[n-1] == '/') --n;
    // The configured prefix must terminate at a URL path boundary; matching
    // just a hostname prefix would also match other ports and hostnames.
    if (strncmp(url, server, n) != 0 ||
        (url[n] != '/' && url[n] != '\0' && url[n] != '?')) return false;
    const char *path = url + n;
    // Surf is an independently authenticated sidecar even if reverse-proxied
    // under the Jellyfin origin.
    if (strncmp(path, "/api/surf", 9) == 0 &&
        (path[9] == '/' || path[9] == '\0' || path[9] == '?')) return false;
    return true;
}
