#include <stdio.h>
#include <string.h>

#include "tools-util.h"
#include "cmds.h"

static const char *subcommands[] = {
	"format",
	"show-super",
	"recover-super",
	"journal-rewind-info",
	"damage",
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
	NULL
};

static const char *global_opts[] = {
	"-h", "--help",
	"-v", "--verbose",
	NULL
};

static void print_bash_completion(void)
{
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
	       "    global_opts=\"");
	for (int i = 0; global_opts[i]; i++)
		printf("%s ", global_opts[i]);
	printf("\"\n\n"
	       "    if [[ ${COMP_CWORD} -eq 1 ]] ; then\n"
	       "        COMPREPLY=( $(compgen -W \"${opts} ${global_opts}\" -- ${cur}) )\n"
	       "        return 0\n"
	       "    fi\n"
	       "\n"
	       "    # Subcommand-specific completion could be added here\n"
	       "}\n"
	       "complete -F _bcachefs bcachefs\n");
}

static void print_zsh_completion(void)
{
	printf("#compdef bcachefs\n"
	       "\n"
	       "_bcachefs() {\n"
	       "    local -a commands\n"
	       "    commands=(\n");
	for (int i = 0; subcommands[i]; i++)
		printf("        '%s'\n", subcommands[i]);
	printf("    )\n"
	       "    _describe -t commands 'bcachefs commands' commands\n"
	       "}\n"
	       "\n"
	       "_bcachefs\n");
}

static void print_fish_completion(void)
{
	printf("# bcachefs fish completion\n"
	       "\n");
	for (int i = 0; subcommands[i]; i++)
		printf("complete -c bcachefs -n \"not __fish_seen_subcommand\" -a \"%s\"\n", subcommands[i]);
	printf("\n"
	       "complete -c bcachefs -n \"__fish_seen_subcommand\" -a \"-h --help -v --verbose\"\n");
}

int cmd_completions(int argc, char *argv[])
{
	if (argc < 2) {
		puts("Usage: bcachefs completions <shell>");
		return -EINVAL;
	}

	if (!strcmp(argv[1], "bash")) {
		print_bash_completion();
	} else if (!strcmp(argv[1], "zsh")) {
		print_zsh_completion();
	} else if (!strcmp(argv[1], "fish")) {
		print_fish_completion();
	} else {
		die("shell %s not supported yet", argv[1]);
	}
	return 0;
}
