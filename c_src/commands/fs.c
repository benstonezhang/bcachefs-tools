#include <stdio.h>
#include <string.h>

#include "cmds.h"
#include "tools-util.h"

static int usage_fs(void)
{
	puts("bcachefs fs - manage a running filesystem\n"
	     "Usage: bcachefs fs <CMD> [OPTIONS]\n"
	     "\n"
	     "Commands:\n"
	     "  usage                   Display detailed filesystem usage\n"
	     "  top                     Show runtime performance information\n"
	     "  timestats               Show operation latency statistics\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
	return 0;
}

int cmd_fs(int argc, char *argv[])
{
	char *cmd = pop_cmd(&argc, argv);

	if (!cmd)
		return usage_fs();
	if (!strcmp(cmd, "usage"))
		return cmd_fs_usage(argc, argv);
	if (!strcmp(cmd, "top"))
		return cmd_fs_top(argc, argv);
	if (!strcmp(cmd, "timestats"))
		return cmd_timestats(argc, argv);

	usage_fs();
	return -EINVAL;
}
