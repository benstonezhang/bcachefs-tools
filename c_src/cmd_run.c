
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "libbcachefs/bcachefs_ioctl.h"
#include "cmds.h"
#include "libbcachefs.h"

#if 0
int cmd_run(int argc, char *argv[])
{
	return 0;
}

int cmd_stop(int argc, char *argv[])
{
	if (argc != 2)
		die("Please supply a filesystem");

	struct bchfs_handle fs = bcache_fs_open(argv[1]);
	xioctl(fs.ioctl_fd, BCH_IOCTL_STOP);
	return 0;
}
#endif
