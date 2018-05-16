/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "volumes.h"
#include "crc32c.h"
#include "commands.h"
#include "utils.h"
#include "help.h"

static const char * const btrfs_cmd_group_usage[] = {
	"btrfs [--help] [--version] [--format <format>] <group> [<group>...] <command> [<args>]",
	NULL
};

static const char btrfs_cmd_group_info[] =
	"Use --help as an argument for information on a specific group or command.";

static inline const char *skip_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) ? NULL : str + len;
}

static int parse_one_token(const char *arg, const struct cmd_group *grp,
			   const struct cmd_struct **cmd_ret)
{
	const struct cmd_struct *abbrev_cmd = NULL, *ambiguous_cmd = NULL;
	int i = 0;

	for (i = 0; grp->commands[i]; i++) {
		const struct cmd_struct *cmd = grp->commands[i];
		const char *rest;

		rest = skip_prefix(arg, cmd->token);
		if (!rest) {
			if (!prefixcmp(cmd->token, arg)) {
				if (abbrev_cmd) {
					/*
					 * If this is abbreviated, it is
					 * ambiguous. So when there is no
					 * exact match later, we need to
					 * error out.
					 */
					ambiguous_cmd = abbrev_cmd;
				}
				abbrev_cmd = cmd;
			}
			continue;
		}
		if (*rest)
			continue;

		*cmd_ret = cmd;
		return 0;
	}

	if (ambiguous_cmd)
		return -2;

	if (abbrev_cmd) {
		*cmd_ret = abbrev_cmd;
		return 0;
	}

	return -1;
}

static const struct cmd_struct *
parse_command_token(const char *arg, const struct cmd_group *grp)
{
	const struct cmd_struct *cmd = NULL;

	switch(parse_one_token(arg, grp, &cmd)) {
	case -1:
		help_unknown_token(arg, grp);
	case -2:
		help_ambiguous_token(arg, grp);
	}

	return cmd;
}

static bool cmd_provides_output_format(const struct cmd_struct *cmd,
				       const struct cmd_context *cmdcxt)
{
	if (!cmdcxt->output_mode)
		return true;

	return (1 << cmdcxt->output_mode) & cmd->cmd_format_flags;
}

static void handle_help_options_next_level(const struct cmd_struct *cmd,
					   const struct cmd_context *cmdcxt,
					   int argc, char **argv)
{
	int err = 0;

	if (argc < 2)
		return;

	/* Check if the command can provide the requested output format */
	if (!cmd->next && !cmd_provides_output_format(cmd, cmdcxt)) {
		ASSERT(cmdcxt->output_mode >= 0);
		ASSERT(cmdcxt->output_mode < CMD_OUTPUT_MAX);
		fprintf(stderr,
			"error: %s output is unsupported for this command.\n\n",
			cmd_outputs[cmdcxt->output_mode]);

		err = 1;
	}

	if (!strcmp(argv[1], "--help") || err) {
		if (cmd->next) {
			argc--;
			argv++;
			help_command_group(cmd->next, argc, argv);
		} else {
			usage_command(cmd, true, false);
		}

		exit(err);
	}
}

int handle_command_group(const struct cmd_group *grp,
			 const struct cmd_context *cmdcxt,
			 int argc, char **argv)

{
	const struct cmd_struct *cmd;

	argc--;
	argv++;
	if (argc < 1) {
		usage_command_group(grp, false, false);
		exit(1);
	}

	cmd = parse_command_token(argv[0], grp);

	handle_help_options_next_level(cmd, cmdcxt, argc, argv);

	fixup_argv0(argv, cmd->token);
	return cmd_execute(cmd, cmdcxt, argc, argv);
}

static const struct cmd_group btrfs_cmd_group;

static const char * const cmd_help_usage[] = {
	"btrfs help [--full]",
	"Display help information",
	"",
	"--full     display detailed help on every command",
	NULL
};

static int cmd_help(const struct cmd_struct *unused,
		    const struct cmd_context *cmdcxt, int argc, char **argv)
{
	help_command_group(&btrfs_cmd_group, argc, argv);
	return 0;
}

static DEFINE_SIMPLE_COMMAND(help, "help");

static const char * const cmd_version_usage[] = {
	"btrfs version",
	"Display btrfs-progs version",
	NULL
};

static int cmd_version(const struct cmd_struct *unused,
		       const struct cmd_context *cmdcxt, int argc, char **argv)
{
	printf("%s\n", PACKAGE_STRING);
	return 0;
}
static DEFINE_SIMPLE_COMMAND(version, "version");

static void print_output_formats(FILE *outf)
{
	int i;

	fputs("Options for --format are:", outf);
	for (i = 0; i < CMD_OUTPUT_MAX; i++)
		fprintf(outf, "%s\"%s\"", i ? ", " : " ", cmd_outputs[i]);
	fputs("\n", outf);

	/* No extended formats anywhere */
	if (CMD_OUTPUT_TEXT + 1 != CMD_OUTPUT_MAX)
		fputs("Extended output formats may not be available for all commands.\n",
		      outf);
}

static void handle_output_format(struct cmd_context *cmdcxt,
				 const char *format)
{
	int i;

	for (i = 0; i < CMD_OUTPUT_MAX; i++) {
		if (!strcasecmp(format, cmd_outputs[i])) {
			cmdcxt->output_mode = i;
			break;
		}
	}

	/* Print error and usage for invalid format */
	if (i == CMD_OUTPUT_MAX) {
		cmdcxt->output_mode = CMD_OUTPUT_TEXT;
		fprintf(stderr, "error: invalid output format \"%s\"\n\n",
			format);
		usage_command_group(&btrfs_cmd_group, false, true);
		print_output_formats(stderr);
		exit(1);
	}
}

/*
 * Parse global options, between binary name and first non-option argument
 * after processing all valid options (including those with arguments).
 *
 * Returns index to argv where parsting stopped, optind is reset to 1
 */
static int handle_global_options(struct cmd_context *cmdcxt,
				 int argc, char **argv)
{
	enum { OPT_HELP = 256, OPT_VERSION, OPT_FULL, OPT_FORMAT };
	static const struct option long_options[] = {
		{ "help", no_argument, NULL, OPT_HELP },
		{ "version", no_argument, NULL, OPT_VERSION },
		{ "format", required_argument, NULL, OPT_FORMAT },
		{ "full", no_argument, NULL, OPT_FULL },
		{ NULL, 0, NULL, 0}
	};
	int shift;

	if (argc == 0)
		return 0;

	opterr = 0;
	while (1) {
		int c;

		c = getopt_long(argc, argv, "+", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case OPT_HELP: break;
		case OPT_VERSION: break;
		case OPT_FULL: break;
		case OPT_FORMAT:
			handle_output_format(cmdcxt, optarg);
			break;
		default:
			fprintf(stderr, "Unknown global option: %s\n",
					argv[optind - 1]);
			exit(129);
		}
	}

	shift = optind;
	optind = 1;

	return shift;
}

void handle_special_globals(const struct cmd_context *cmdcxt, int shift,
			    int argc, char **argv)
{
	bool has_help = false;
	bool has_full = false;
	int i;

	for (i = 0; i < shift; i++) {
		if (strcmp(argv[i], "--help") == 0)
			has_help = true;
		else if (strcmp(argv[i], "--full") == 0)
			has_full = true;
	}

	if (has_help) {
		if (has_full)
			usage_command_group(&btrfs_cmd_group, true, false);
		else
			cmd_execute(&cmd_struct_help, cmdcxt, argc, argv);
		print_output_formats(stdout);
		exit(0);
	}

	for (i = 0; i < shift; i++)
		if (strcmp(argv[i], "--version") == 0) {
			cmd_execute(&cmd_struct_version, cmdcxt, argc, argv);
			exit(0);
		}
}

static const struct cmd_group btrfs_cmd_group = {
	btrfs_cmd_group_usage, btrfs_cmd_group_info, {
		&cmd_struct_subvolume,
		&cmd_struct_filesystem,
		&cmd_struct_balance,
		&cmd_struct_device,
		&cmd_struct_scrub,
		&cmd_struct_check,
		&cmd_struct_rescue,
		&cmd_struct_restore,
		&cmd_struct_inspect,
		&cmd_struct_property,
		&cmd_struct_send,
		&cmd_struct_receive,
		&cmd_struct_quota,
		&cmd_struct_qgroup,
		&cmd_struct_replace,
		&cmd_struct_help,
		&cmd_struct_version,
		NULL
	},
};

int main(int argc, char **argv)
{
	const struct cmd_struct *cmd;
	const char *bname;
	struct cmd_context cmdcxt = { .output_mode = CMD_OUTPUT_TEXT, };
	int ret;

	btrfs_config_init();

	if ((bname = strrchr(argv[0], '/')) != NULL)
		bname++;
	else
		bname = argv[0];

	if (!strcmp(bname, "btrfsck")) {
		argv[0] = "check";
	} else {
		int shift;

		shift = handle_global_options(&cmdcxt, argc, argv);
		handle_special_globals(&cmdcxt, shift, argc, argv);
		while (shift-- > 0) {
			argc--;
			argv++;
		}
		if (argc == 0) {
			usage_command_group_short(&btrfs_cmd_group);
			exit(1);
		}
	}

	cmd = parse_command_token(argv[0], &btrfs_cmd_group);

	handle_help_options_next_level(cmd, &cmdcxt, argc, argv);

	crc32c_optimization_init();

	fixup_argv0(argv, cmd->token);

	ret = cmd_execute(cmd, &cmdcxt, argc, argv);

	btrfs_close_all_devices();

	exit(ret);
}
