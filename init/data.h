/* init and the data disk (docs/spec/storage.md F18): the virtio block
   driver serving it, the file system service over it, /data and /users
   published in the namespace, and the program that checks them. */

#ifndef NOVA_DATA_H
#define NOVA_DATA_H

#include "platform.h"

/* Find the data disk, the second virtio block function, and start
   \nova\virtblk.exe serving it and \nova\fs.exe over it, in job. A
   machine with no data disk, or a disk whose service does not start, is
   logged and leaves the data disk absent; init goes on either way. */
void data_start(const PLATFORM *platform, UINT32 job);

/* With the data disk's service running, create /data and /users in its
   volume if they are missing, publish both through the namespace
   server's root, and run \nova\fstest.exe in job with a view of /data,
   reached through the namespace, read-write and read-only, and of the
   namespace's root without WRITE, each step logged. A data disk that fails any of them is logged and init goes
   on, as without one. */
void data_mount(UINT32 job, UINT32 debug_log, UINT32 namespace_root);

#endif
