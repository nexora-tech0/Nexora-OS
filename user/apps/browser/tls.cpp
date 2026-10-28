#include "tls.h"

#include "bearssl.h"
#include "hzsys.h"

#include <string.h>

extern "C" const br_x509_trust_anchor *BrowserTrustAnchors(size_t *count);

namespace nexora::browser {
namespace {

constexpr uint32_t TicksPerSecond = 10000000;
constexpr uint64_t FileTimeEpochDays = 584754;
constexpr uint32_t MinimumPlausibleDays = 737790; // 2020-01-01
constexpr uint32_t MaximumPlausibleDays = 767375; // 2101-01-01
constexpr uint32_t NetworkReadTimeoutMs = 3000;
constexpr unsigned int MaximumResponseReads = 256;

struct Transport {
    int32_t Socket;
    STATUS LastReadStatus;
    uint32_t ReadCalls;
    uint32_t BytesRead;
    bool PeerClosed;
};

void Wipe(void *memory, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)memory;
    while (length--)
        *bytes++ = 0;
}

void SetError(char *error, size_t capacity, const char *message)
{
    if (!error || !capacity)
        return;
    size_t length = 0;
    while (message[length] && length + 1 < capacity) {
        error[length] = message[length];
        ++length;
    }
    error[length] = 0;
}

int SocketRead(void *context, unsigned char *buffer, size_t length)
{
    Transport *transport = (Transport *)context;
    if (!length)
        return -1;
    uint32_t capacity = (uint32_t)(length < HZ_TCP_MAX_TRANSFER ? length : HZ_TCP_MAX_TRANSFER);
    uint32_t received = 0;
    STATUS status = HzTcpReceive(transport->Socket, buffer, capacity,
                                 NetworkReadTimeoutMs, &received);
    transport->LastReadStatus = status;
    transport->ReadCalls++;
    transport->PeerClosed = HZ_SUCCESS(status) && received == 0;
    if (!HZ_SUCCESS(status) || !received || received > capacity || received > INT32_MAX)
        return -1;
    transport->BytesRead += received;
    return (int)received;
}

int SocketWrite(void *context, const unsigned char *buffer, size_t length)
{
    Transport *transport = (Transport *)context;
    if (!length || length > INT32_MAX)
        return -1;
    size_t sent = 0;
    while (sent < length) {
        uint32_t capacity = (uint32_t)((length - sent) < HZ_TCP_MAX_TRANSFER ?
                                       (length - sent) : HZ_TCP_MAX_TRANSFER);
        uint32_t written = 0;
        STATUS status = HzTcpSend(transport->Socket, buffer + sent,
                                  capacity, &written);
        if (!HZ_SUCCESS(status) || !written || written > capacity)
            return -1;
        sent += written;
    }
    return (int)sent;
}

bool GetValidationTime(uint32_t *days, uint32_t *seconds, char *error, size_t errorCapacity)
{
    uint64_t ticks = 0;
    STATUS status = NtQuerySystemTime(&ticks);
    if (!HZ_SUCCESS(status)) {
        HzFormat(error, errorCapacity, "HTTPS blocked: the system clock is unavailable (0x%08X).",
                 (uint32_t)status);
        return false;
    }
    uint64_t epochSeconds = ticks / TicksPerSecond;
    uint64_t absoluteDays = FileTimeEpochDays + epochSeconds / 86400ull;
    if (absoluteDays < MinimumPlausibleDays || absoluteDays >= MaximumPlausibleDays) {
        SetError(error, errorCapacity,
                 "HTTPS blocked: system time is unset or outside the supported 2020-2100 range.");
        return false;
    }
    *days = (uint32_t)absoluteDays;
    *seconds = (uint32_t)(epochSeconds % 86400ull);
    return true;
}

void DescribeTlsError(unsigned error, char *text, size_t capacity)
{
    switch (error) {
    case BR_ERR_NO_RANDOM:
        SetError(text, capacity, "HTTPS failed: TLS could not obtain secure random bytes.");
        break;
    case BR_ERR_X509_BAD_SERVER_NAME:
        SetError(text, capacity, "HTTPS certificate hostname does not match the requested host.");
        break;
    case BR_ERR_X509_EXPIRED:
        SetError(text, capacity, "HTTPS certificate is not valid at the current system time.");
        break;
    case BR_ERR_X509_NOT_TRUSTED:
        SetError(text, capacity, "HTTPS certificate chain is not trusted by the bundled CA store.");
        break;
    case BR_ERR_X509_CRITICAL_EXTENSION:
        SetError(text, capacity,
                 "HTTPS certificate contains a critical extension unsupported by the TLS verifier.");
        break;
    default:
        HzFormat(text, capacity, "HTTPS TLS handshake or certificate validation failed (BearSSL error %u).",
                 error);
        break;
    }
}

}

bool FetchTlsResponse(int32_t socket, const char *host, const char *request, size_t requestLength,
                      char *wire, size_t wireCapacity, size_t *wireLength,
                      bool *cleanTlsClose,
                      char *error, size_t errorCapacity)
{
    if (!host || !request || !wire || !wireLength || !cleanTlsClose || !wireCapacity ||
        requestLength > INT32_MAX) {
        SetError(error, errorCapacity, "HTTPS request parameters exceed the supported bounds.");
        return false;
    }
    *wireLength = 0;
    *cleanTlsClose = false;

    uint32_t validationDays = 0;
    uint32_t validationSeconds = 0;
    if (!GetValidationTime(&validationDays, &validationSeconds, error, errorCapacity))
        return false;

    unsigned char entropy[48];
    STATUS randomStatus = HzGenerateRandomBytes(entropy, sizeof(entropy));
    if (!HZ_SUCCESS(randomStatus)) {
        HzFormat(error, errorCapacity,
                 "HTTPS blocked: the kernel secure-random service is unavailable (0x%08X).",
                 (uint32_t)randomStatus);
        Wipe(entropy, sizeof(entropy));
        return false;
    }

    size_t trustAnchorCount = 0;
    const br_x509_trust_anchor *trustAnchors = BrowserTrustAnchors(&trustAnchorCount);
    if (!trustAnchors || !trustAnchorCount) {
        Wipe(entropy, sizeof(entropy));
        SetError(error, errorCapacity, "HTTPS blocked: the bundled trusted-CA store is empty.");
        return false;
    }

    br_ssl_client_context client;
    br_x509_minimal_context x509;
    unsigned char ioBuffer[BR_SSL_BUFSIZE_BIDI];
    br_sslio_context io;
    Transport transport = { socket, STATUS_SUCCESS, 0, 0, false };

    br_ssl_client_init_full(&client, &x509, trustAnchors, trustAnchorCount);
    br_ssl_engine_set_versions(&client.eng, BR_TLS12, BR_TLS12);
    client.eng.flags |= BR_OPT_NO_RENEGOTIATION;
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
    };
    br_ssl_engine_set_suites(&client.eng, suites, sizeof(suites) / sizeof(suites[0]));
    br_ssl_engine_inject_entropy(&client.eng, entropy, sizeof(entropy));
    Wipe(entropy, sizeof(entropy));
    br_x509_minimal_set_time(&x509, validationDays, validationSeconds);
    br_ssl_engine_set_buffer(&client.eng, ioBuffer, sizeof(ioBuffer), 1);
    if (!br_ssl_client_reset(&client, host, 0)) {
        DescribeTlsError(br_ssl_engine_last_error(&client.eng), error, errorCapacity);
        Wipe(ioBuffer, sizeof(ioBuffer));
        Wipe(&client, sizeof(client));
        Wipe(&x509, sizeof(x509));
        return false;
    }

    br_sslio_init(&io, &client.eng, SocketRead, &transport, SocketWrite, &transport);
    if (br_sslio_write_all(&io, request, requestLength) < 0 || br_sslio_flush(&io) < 0) {
        unsigned tlsError = br_ssl_engine_last_error(&client.eng);
        if (tlsError)
            DescribeTlsError(tlsError, error, errorCapacity);
        else
            SetError(error, errorCapacity, "HTTPS request could not be sent over the TLS connection.");
        Wipe(ioBuffer, sizeof(ioBuffer));
        Wipe(&client, sizeof(client));
        Wipe(&x509, sizeof(x509));
        return false;
    }

    unsigned char chunk[4096];
    bool receivedCloseNotify = false;
    bool peerClosed = false;
    unsigned int responseReads = 0;
    for (; responseReads < MaximumResponseReads; ++responseReads) {
        int received = br_sslio_read(&io, chunk, sizeof(chunk));
        if (received < 0) {
            receivedCloseNotify = client.eng.shutdown_recv != 0 &&
                                  br_ssl_engine_last_error(&client.eng) == 0;
            peerClosed = !receivedCloseNotify && transport.PeerClosed;
            break;
        }
        if (!received || (size_t)received > wireCapacity - *wireLength) {
            SetError(error, errorCapacity, "HTTPS response exceeded the bounded response buffer.");
            Wipe(chunk, sizeof(chunk));
            Wipe(ioBuffer, sizeof(ioBuffer));
            Wipe(&client, sizeof(client));
            Wipe(&x509, sizeof(x509));
            return false;
        }
        memcpy(wire + *wireLength, chunk, (size_t)received);
        *wireLength += (size_t)received;
    }
    Wipe(chunk, sizeof(chunk));

    if (!receivedCloseNotify && !peerClosed) {
        unsigned tlsError = br_ssl_engine_last_error(&client.eng);
        if (tlsError)
            DescribeTlsError(tlsError, error, errorCapacity);
        else if (!HZ_SUCCESS(transport.LastReadStatus))
            HzFormat(error, errorCapacity,
                     "HTTPS transport read failed (0x%08X) after %u encrypted bytes across %u reads.",
                     (uint32_t)transport.LastReadStatus, transport.BytesRead, transport.ReadCalls);
        else
            HzFormat(error, errorCapacity,
                     "HTTPS stream ended without TLS close_notify (BearSSL state %u, error %u, %u response reads, %u encrypted bytes).",
                     br_ssl_engine_current_state(&client.eng), tlsError, responseReads, transport.BytesRead);
        Wipe(ioBuffer, sizeof(ioBuffer));
        Wipe(&client, sizeof(client));
        Wipe(&x509, sizeof(x509));
        return false;
    }
    *cleanTlsClose = receivedCloseNotify;
    Wipe(ioBuffer, sizeof(ioBuffer));
    Wipe(&client, sizeof(client));
    Wipe(&x509, sizeof(x509));
    return true;
}

}
