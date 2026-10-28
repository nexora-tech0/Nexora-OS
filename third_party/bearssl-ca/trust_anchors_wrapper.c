#include "bearssl.h"

#include "trust_anchors.c"

const br_x509_trust_anchor *BrowserTrustAnchors(size_t *count)
{
    if (count)
        *count = TAs_NUM;
    return TAs;
}
