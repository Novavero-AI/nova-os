/* Resources. A resource's range never changes (docs/spec/ipc.md I6). */

#include "resource.h"
#include "../mm/page.h"
#include "../x86_64/serial.h"

/* Bus requester IDs as I8 gives them: the segment group in bits 31:16,
   the bus in 15:8, the device in 7:3 and the function in 2:0. */
#define REQUESTER_SEGMENT_SHIFT 16
#define REQUESTER_BUS_SHIFT 8
#define REQUESTER_DEVICE_SHIFT 3
#define REQUESTER_SEGMENT_MASK 0xffff
#define REQUESTER_BUS_MASK 0xff
#define REQUESTER_DEVICE_MASK 0x1f
#define REQUESTER_FUNCTION_MASK 0x7
#define SEGMENT_DIGITS 4
#define BUS_DIGITS 2
#define DEVICE_DIGITS 2
#define FUNCTION_DIGITS 1

static void destroy(OBJECT *object)
{
    page_free_mapped(object);
}

static const OBJECT_TYPE resource_type = {TYPE_RESOURCE, 0, destroy, 0, 0};

static RESOURCE *create(RESOURCE_KIND kind, UINT64 base, UINT64 size)
{
    RESOURCE *resource = page_alloc_mapped();

    if (!resource)
        return 0;
    object_init(&resource->object, &resource_type);
    resource->kind = kind;
    resource->base = base;
    resource->size = size;
    return resource;
}

RESOURCE *resource_create_root(RESOURCE_KIND kind, UINT64 base, UINT64 size)
{
    return create(kind, base, size);
}

/* The comparisons are arranged so that no sum can wrap. */
static int within(UINT64 base, UINT64 size, UINT64 outer_base,
                  UINT64 outer_size)
{
    return base >= outer_base && size <= outer_size &&
           base - outer_base <= outer_size - size;
}

STATUS resource_derive(const RESOURCE *parent, UINT64 base, UINT64 size,
                       RESOURCE **child)
{
    if (size == 0 || base + size < base)
        return ERR_INVALID_ARGS;
    if (!within(base, size, parent->base, parent->size))
        return ERR_OUT_OF_RANGE;
    *child = create(parent->kind, base, size);
    return *child ? OK : ERR_NO_MEMORY;
}

int resource_covers(const RESOURCE *resource, RESOURCE_KIND kind,
                    UINT64 base, UINT64 size)
{
    return resource->kind == kind && within(base, size, resource->base,
                                            resource->size);
}

void resource_log_requester(UINT64 requester)
{
    serial_hex_width(requester >> REQUESTER_SEGMENT_SHIFT &
                         REQUESTER_SEGMENT_MASK,
                     SEGMENT_DIGITS);
    serial_write(":");
    serial_hex_width(requester >> REQUESTER_BUS_SHIFT & REQUESTER_BUS_MASK,
                     BUS_DIGITS);
    serial_write(":");
    serial_hex_width(requester >> REQUESTER_DEVICE_SHIFT &
                         REQUESTER_DEVICE_MASK,
                     DEVICE_DIGITS);
    serial_write(".");
    serial_hex_width(requester & REQUESTER_FUNCTION_MASK, FUNCTION_DIGITS);
}
