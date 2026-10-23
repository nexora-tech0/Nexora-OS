/*
 * Nexora OS - io/iop.h
 * I/O manager internals shared by the files in io/.
 */
#ifndef IOP_H
#define IOP_H

#include "ex.h"
#include "io.h"
#include "ke.h"
#include "mm.h"
#include "ob.h"
#include "rtl.h"

#define IOP_FILE_TAG HZ_POOL_TAG('I', 'o', 'F', 'i')
#define IOP_BUFFER_TAG HZ_POOL_TAG('I', 'o', 'B', 'f')
#define IOP_READ_FILE_TAG HZ_POOL_TAG('I', 'o', 'R', 'f')

#define FO_OPENED 0x8000 /* the target driver accepted IRP_MJ_CREATE: it gets IRP_MJ_CLOSE */

/* Open parameters carried through the object manager to IopParseDevice. */
typedef struct _IOP_OPEN_PARAMETERS {
    ACCESS_MASK DesiredAccess; /* already mapped (no GENERIC_*) */
    uint32_t Disposition;
    uint32_t Options;
    KPROCESSOR_MODE Mode;
    uint32_t Result;           /* FILE_OPENED, ... */
} IOP_OPEN_PARAMETERS;

STATUS IopParseDevice(void *Object, const char *RemainingPath, void *ParseContext, void **Result);
STATUS IopCreateFileObject(DEVICE_OBJECT *Device, VPB *Vpb, const char *Path, FILE_OBJECT **File);
ACCESS_MASK IopMapFileAccess(ACCESS_MASK Access);

#endif /* IOP_H */
