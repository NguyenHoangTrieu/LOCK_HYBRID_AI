/*
 * backend_discovery.c - see backend_discovery.h.
 */
#include "backend_discovery.h"

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "backend_discovery";

#define DISCOVERY_REQUEST "LOCK_HYBRID_DISCOVER_V1"
#define DISCOVERY_REPLY_PREFIX "LOCK_HYBRID_BACKEND_V1"
#define DISCOVERY_REPLY_BUF_SIZE 96U
#define DISCOVERY_ATTEMPTS 3U
#define DISCOVERY_TIMEOUT_MS 700U

/* Parses "...PORT=<digits>..." out of a NUL-terminated reply buffer.
 * Returns 0 if the key isn't present or the value doesn't parse. */
static uint16_t ParsePortField(const char *reply)
{
    const char *p = strstr(reply, "PORT=");
    if (p == NULL)
    {
        return 0U;
    }
    long port = strtol(p + strlen("PORT="), NULL, 10);
    return (port > 0 && port <= 65535) ? (uint16_t)port : 0U;
}

bool BACKEND_DISCOVERY_Resolve(char *outBaseUrl, size_t outSize)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGW(TAG, "socket() failed: errno %d", errno);
        return false;
    }

    int broadcastEnable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

    struct timeval timeout = {
        .tv_sec  = (long)(DISCOVERY_TIMEOUT_MS / 1000U),
        .tv_usec = (long)((DISCOVERY_TIMEOUT_MS % 1000U) * 1000U),
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in destAddr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)CONFIG_LOCK_HYBRID_BACKEND_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    bool found = false;
    uint16_t foundPort = 0U;
    char foundIp[16] = {0};

    for (uint32_t attempt = 0U; attempt < DISCOVERY_ATTEMPTS && !found; attempt++)
    {
        if (sendto(sock, DISCOVERY_REQUEST, strlen(DISCOVERY_REQUEST), 0,
                   (struct sockaddr *)&destAddr, sizeof(destAddr)) < 0)
        {
            ESP_LOGW(TAG, "sendto() failed: errno %d", errno);
            continue;
        }

        char replyBuf[DISCOVERY_REPLY_BUF_SIZE];
        struct sockaddr_in fromAddr;
        socklen_t fromLen = sizeof(fromAddr);
        int n = recvfrom(sock, replyBuf, sizeof(replyBuf) - 1, 0, (struct sockaddr *)&fromAddr, &fromLen);
        if (n <= 0)
        {
            continue; /* timed out or error - try again */
        }
        replyBuf[n] = '\0';

        if (strncmp(replyBuf, DISCOVERY_REPLY_PREFIX, strlen(DISCOVERY_REPLY_PREFIX)) != 0)
        {
            ESP_LOGW(TAG, "unrecognized discovery reply, ignoring: %s", replyBuf);
            continue;
        }
        uint16_t port = ParsePortField(replyBuf);
        if (port == 0U)
        {
            ESP_LOGW(TAG, "discovery reply missing/invalid PORT=, ignoring: %s", replyBuf);
            continue;
        }

        inet_ntoa_r(fromAddr.sin_addr, foundIp, sizeof(foundIp));
        foundPort = port;
        found     = true;
    }

    close(sock);

    if (!found)
    {
        ESP_LOGW(TAG, "no discovery reply after %u attempts - caller should fall back to its static URL",
                 (unsigned)DISCOVERY_ATTEMPTS);
        return false;
    }

    (void)snprintf(outBaseUrl, outSize, "http://%s:%u", foundIp, (unsigned)foundPort);
    ESP_LOGI(TAG, "discovered backend at %s", outBaseUrl);
    return true;
}
