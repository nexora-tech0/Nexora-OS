#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nexora::browser {

bool FetchTlsResponse(int32_t socket, const char *host, const char *request, size_t requestLength,
                      char *wire, size_t wireCapacity, size_t *wireLength,
                      bool *cleanClose,
                      char *error, size_t errorCapacity);

}
