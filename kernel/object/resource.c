/* Resources. A resource's range never changes (docs/spec/ipc.md I6). */

#include "resource.h"
#include "../mm/page.h"

static void destroy(OBJECT *object)
{
    page_free_mapped(object);
}

static const OBJECT_TYPE resource_type = {TYPE_RESOURCE, 0, destroy, 0};

RESOURCE *resource_create_root_system(void)
{
    RESOURCE *resource = page_alloc_mapped();

    if (!resource)
        return 0;
    object_init(&resource->object, &resource_type);
    resource->base = 0;
    resource->size = SYSTEM_PRIVILEGES;
    return resource;
}

int resource_covers(const RESOURCE *resource, UINT64 privilege)
{
    return privilege >= resource->base &&
           privilege - resource->base < resource->size;
}
