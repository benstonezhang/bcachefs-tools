#include <stdio.h>
#include <string.h>

#include "tools-util.h"
#include "cmds.h"

static const char *subcommands[] = { "format",
				     "show-super",
				     "recover-super",
				     "set-fs-option",
				     "reset-counters",
				     "strip-alloc",
				     "image",
				     "mount",
				     "fusemount",
				     "wait-devices",
				     "fsck",
				     "recovery-pass",
				     "fs",
				     "device",
				     "subvolume",
				     "reconcile",
				     "scrub",
				     "unlock",
				     "set-passphrase",
				     "remove-passphrase",
				     "migrate",
				     "migrate-superblock",
				     "set-file-option",
				     "reflink-option-propagate",
				     "dump",
				     "undump",
				     "list",
				     "list_journal",
				     "kill_btree_node",
				     "data-read",
				     "unpoison",
				     "completions",
				     "version",
				     NULL };

int cmd_completions(int argc, char *argv[])
{
	if (argc < 2) {
		puts("Usage: bcachefs completions <shell>");
		return -EINVAL;
	}

	if (!strcmp(argv[1], "bash")) {
		printf("_bcachefs()\n"
		       "{\n"
		       "    local cur prev opts\n"
		       "    COMPREPLY=()\n"
		       "    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
		       "    prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
		       "    opts=\"");
		for (int i = 0; subcommands[i]; i++)
			printf("%s ", subcommands[i]);
		printf("\"\n"
		       "\n"
		       "    if [[ ${COMP_CWORD} -eq 1 ]] ; then\n"
		       "        COMPREPLY=( $(compgen -W \"${opts}\" -- ${cur}) )\n"
		       "        return 0\n"
		       "    fi\n"
		       "}\n"
		       "complete -F _bcachefs bcachefs\n");
	} else {
		die("shell %s not supported yet", argv[1]);
	}
	return 0;
}
