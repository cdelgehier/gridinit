/*
gridinit, a monitor for non-daemon processes.
Copyright (C) 2013 AtoS Worldline, original work aside of Redcurrant
Copyright (C) 2015 OpenIO, modified for OpenIO Software Defined Storage

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
# include "../config.h"
#endif
#ifndef LOG_DOMAIN
# define LOG_DOMAIN "gridinit_cmd"
#endif

#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <strings.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <glib.h>

#include "./gridinit_internals.h"
#include "../lib/gridinit-internals.h"

#define MINI 0
#define MEDIUM 1

static gboolean flag_help = FALSE;

static gchar sock_path[1024];
static gchar line[65536];
static gboolean flag_color = FALSE;

#define BOOL(i) (i?1:0)

struct dump_status_arg_s {
	int how;
	int argc;
	char **args;
	guint count_faulty;
	guint count_all;
	guint count_misses;
};

struct dump_as_is_arg_s {
	guint count_success;
	guint count_errors;
};

struct command_s {
	const gchar *name;
	int (*action) (int argc, char **args);
};

struct child_info_s {
	char *key;
	char *group;
	char *cmd;
	gint pid;
	guint uid;
	guint gid;
	gboolean enabled;
	gboolean respawn;
	gboolean broken;
	gboolean breakable;
	guint32 user_flags;
	time_t last_start_attempt;
	time_t last_kill_attempt;
	guint counter_started;
	guint counter_died;
	struct {
		long core_size;
		long stack_size;
		long nb_files;
	} rlimits;
};

struct keyword_set_s {
	const gchar *already;
	const gchar *done;
	const gchar *failed;

	const gchar *broken;
	const gchar *down;
	const gchar *disabled;
	const gchar *up;
};

static struct keyword_set_s KEYWORDS_NORMAL = {
	"ALREADY ",
	"DONE    ",
	"FAILED  ",
	"BROKEN  ",
	"DOWN    ",
	"DISABLED",
	"UP      "
};

static struct keyword_set_s KEYWORDS_COLOR = {
	"[33mALREADY[0m ",
	"[32mDONE[0m    ",
	"[31mFAILED[0m  ",
	"[31mBROKEN[0m  ",
	"[33mDOWN[0m    ",
	"[36mDISABLED[0m",
	"[32mUP[0m      "
};

static gint
compare_child_info(gconstpointer p1, gconstpointer p2)
{
	const struct child_info_s *c1, *c2;
	c1 = p1;
	c2 = p2;
	return g_ascii_strcasecmp(c1->key, c2->key);
}

static const char *
get_child_status(struct child_info_s *ci, gboolean *faulty)
{
	struct keyword_set_s *kw;

	kw = flag_color ? &KEYWORDS_COLOR : &KEYWORDS_NORMAL;

	if (ci->broken) {
		*faulty = TRUE;
		return kw->broken;
	}
	if (!ci->enabled) {
		*faulty = FALSE;
		return kw->disabled;
	}
	if (ci->pid <= 0) {
		*faulty = TRUE;
		return kw->down;
	}

	*faulty = FALSE;
	return kw->up;
}

static size_t
get_longest_group(GList *all_jobs)
{
	size_t maxlen = 5;
	GList *l;
	for (l=all_jobs; l ;l=l->next) {
		struct child_info_s *ci = l->data;
		size_t len = strlen(ci->group);
		if (len > maxlen)
			maxlen = len;
	}
	return maxlen;
}

static size_t
get_longest_key(GList *all_jobs)
{
	size_t maxlen = 4;
	GList *l;
	for (l=all_jobs; l ;l=l->next) {
		struct child_info_s *ci = l->data;
		size_t len = strlen(ci->key);
		if (len > maxlen)
			maxlen = len;
	}
	return maxlen;
}

static size_t
my_chomp(gchar *str)
{
	gchar c;
	size_t len;

	len = strlen(str);
	while (len && (c=str[len-1]) && g_ascii_isspace(c))
		str[--len] = '\0';
	return len;
}

static void
unpack_line(gchar *str, gchar **start, int *code)
{
	gchar c, *p = NULL;

	*start = str;
	*code = EINVAL;
	if (!str || !*str)
		return ;
	if (!my_chomp(str))
		return ;
	*code = g_ascii_strtoll(str, &p, 10);

	if (p) {
		while ((c = *p) && g_ascii_isspace(c))
			p++;
		*start = p;
	}
}

static GList*
read_services_list(FILE *in_stream)
{
	GList *all_jobs = NULL;

	while (!feof(in_stream) && !ferror(in_stream)) {
		if (NULL != fgets(line, sizeof(line), in_stream)) {

			(void) my_chomp(line);

			gchar **tokens = g_strsplit_set(line, " \t\r\n", 15);
			if (tokens) {
				if (g_strv_length(tokens) == 15) {
					struct child_info_s ci;
					ci.pid = atoi(tokens[0]);
					ci.enabled = BOOL(atoi(tokens[1]));
					ci.broken = BOOL(atoi(tokens[2]));
					ci.respawn = BOOL(atoi(tokens[3]));
					ci.counter_started = atoi(tokens[4]);
					ci.counter_died = atoi(tokens[5]);
					ci.last_start_attempt = atol(tokens[6]);
					ci.rlimits.core_size = atol(tokens[7]);
					ci.rlimits.stack_size = atol(tokens[8]);
					ci.rlimits.nb_files = atol(tokens[9]);
					ci.uid = atol(tokens[10]);
					ci.gid = atol(tokens[11]);
					ci.key = g_strdup(tokens[12]);
					ci.group = g_strdup(tokens[13]);
					ci.cmd = g_strdup(tokens[14]);
					all_jobs = g_list_prepend(all_jobs,
						g_memdup(&ci, sizeof(struct child_info_s)));
				}
				g_strfreev(tokens);
			}
		}
	}

	return g_list_sort(all_jobs, compare_child_info);
}


static void
dump_as_is(FILE *in_stream, void *udata)
{
	int code;
	gchar *start;
	struct dump_as_is_arg_s *dump_args;
	struct keyword_set_s *kw;

	kw = flag_color ? &KEYWORDS_COLOR : &KEYWORDS_NORMAL;

	dump_args = udata;
	while (!feof(in_stream) && !ferror(in_stream)) {
		bzero(line, sizeof(line));
		if (NULL != fgets(line, sizeof(line), in_stream)) {
			start = NULL;
			(void)unpack_line(line, &start, &code);

			if (dump_args) {
				if (code==0 || code==EALREADY)
					dump_args->count_success ++;
				else
					dump_args->count_errors ++;
			}

			fprintf(stdout, "%s\t%s\t%s\n",
					(code==0 ? kw->done : (code==EALREADY?kw->already:kw->failed)),
					start, strerror(code));
		}
	}
	fflush(stdout);
}

static void
dump_status(FILE *in_stream, void *udata)
{
	char fmt_title[256], fmt_line[256];
	size_t maxkey, maxgroup;
	struct dump_status_arg_s *status_args;
	GList *all_jobs = NULL, *l;
	GHashTable *ht_counts;

	gboolean matches(struct child_info_s *ci, int argc, gchar **args) {
		int i;
		gchar *s;
		gpointer p;
		gboolean rc;

		if (!*args)
			return TRUE;
		rc = FALSE;
		for (i=0; i<argc && (s=args[i]) ;i++) {
			p = g_hash_table_lookup(ht_counts, s);
			if (s[0]=='@' && gridinit_group_in_set(s+1, ci->group)) {
				*((guint32*)p) = *((guint32*)p) + 1;
				rc = TRUE;
			}
			if (s[0]!='@' && !g_ascii_strcasecmp(ci->key, s)) {
				*((guint32*)p) = *((guint32*)p) + 1;
				rc = TRUE;
			}
		}
		return rc;
	}

	status_args = udata;
	all_jobs = read_services_list(in_stream);

	maxkey = get_longest_key(all_jobs);
	maxgroup = get_longest_group(all_jobs);

	do { /* Prepares a hash of match counters associated to the pattern strings */
		int i = 0;
		ht_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		for (i = 0; i < status_args->argc ; i++)
			g_hash_table_insert(ht_counts, g_strdup(status_args->args[i]), g_malloc0(sizeof(guint32)));
	} while (0);

	/* write the title line */
	switch (status_args->how) {
	case MINI:
		g_snprintf(fmt_title, sizeof(fmt_title),
				"%%-%us %%-8s %%6s %%s\n",
				(guint)maxkey);
		g_snprintf(fmt_line, sizeof(fmt_line),
				"%%-%us %%s %%6d %%s\n",
				(guint)maxkey);
		fprintf(stdout, fmt_title, "KEY", "STATUS", "PID", "GROUP");
		break;
	case MEDIUM:
		g_snprintf(fmt_title, sizeof(fmt_title),
				"%%-%us %%-8s %%5s %%6s %%5s %%19s %%%us %%s\n",
				(guint)maxkey, (guint)maxgroup);
		g_snprintf(fmt_line, sizeof(fmt_line),
				"%%-%us %%s %%5d %%6d %%5d %%19s %%%us %%s\n",
				(guint)maxkey, (guint)maxgroup);
		fprintf(stdout, fmt_title, "KEY", "STATUS", "PID", "#START",
				"#DIED", "SINCE", "GROUP", "CMD");
		break;
	default:
		g_snprintf(fmt_title, sizeof(fmt_title),
				"%%-%us %%-8s %%5s %%6s %%5s %%8s %%8s %%8s %%19s %%%us %%s\n",
				(guint)maxkey, (guint)maxgroup);
		g_snprintf(fmt_line, sizeof(fmt_line),
				"%%-%us %%s %%5d %%6d %%5d %%8ld %%8ld %%8ld %%19s %%%us %%s\n",
				(guint)maxkey, (guint)maxgroup);

		fprintf(stdout, fmt_title, "KEY", "STATUS", "PID", "#START",
				"#DIED", "CSZ", "SSZ", "MFD", "SINCE", "GROUP", "CMD");
		break;
	}

	/* Dump the list */
	for (l=all_jobs; l ;l=l->next) {
		char str_time[20] = "---------- --------";
		const char * str_status = "-";
		struct child_info_s *ci = NULL;
		gboolean faulty = FALSE;
		
		ci = l->data;
		if (status_args->argc && status_args->args) {
			if (!matches(ci, status_args->argc, status_args->args))
				continue;
		}

		/* Prepare some fields */
		if (ci->pid > 0)
			strftime(str_time, sizeof(str_time), "%Y-%m-%d %H:%M:%S",
				gmtime(&(ci->last_start_attempt)));
		str_status = get_child_status(ci, &faulty);
		
		/* Manage counters */
		status_args->count_all ++;
		if (faulty)
			status_args->count_faulty ++;

		/* Print now! */
		switch (status_args->how) {
		case MINI:
			fprintf(stdout, fmt_line, ci->key, str_status, ci->pid, ci->group);
			break;
		case MEDIUM:
			fprintf(stdout, fmt_line,
				ci->key, str_status, ci->pid,
				ci->counter_started, ci->counter_died,
				str_time, ci->group, ci->cmd);
			break;
		default:
			fprintf(stdout, fmt_line,
				ci->key, str_status, ci->pid,
				ci->counter_started, ci->counter_died,
				ci->rlimits.core_size, ci->rlimits.stack_size, ci->rlimits.nb_files,
				str_time, ci->group, ci->cmd);
			break;
		}
	}
	fflush(stdout);

	/* check that all the input patterns have been used */
	do {
		GHashTableIter iter;
		gpointer k, v;
		g_hash_table_iter_init(&iter, ht_counts);
		while (g_hash_table_iter_next(&iter, &k, &v)) {
			if (!*((guint32*)v)) {
				g_printerr("# Group/Key [%s] matched no service\n", (gchar*)k);
				++ status_args->count_misses;
			}
		}
	} while (0);

	/* free anything */
	g_hash_table_destroy(ht_counts);
	for (l=all_jobs; l ;l=l->next) {
		struct child_info_s *ci = l->data;
		g_free(ci->key);
		g_free(ci->cmd);
		g_free(ci->group);
		g_free(ci);
		l->data = NULL;
	}
	
	g_list_free(all_jobs);
	all_jobs = NULL;
}

static FILE*
open_cnx(void)
{
	int req_fd = -1;
	FILE *req_stream = NULL;
	if (-1 == (req_fd = __open_unix_client(sock_path))) {
		g_printerr("Connection to UNIX socket [%s] failed : %s\n", sock_path, strerror(errno));
		return NULL;
	}

	if (NULL == (req_stream = fdopen(req_fd, "a+"))) {
		g_printerr("Connection to UNIX socket [%s] failed : %s\n", sock_path, strerror(errno));
		close(req_fd);
		return NULL;
	}

	return req_stream;
}

static int
send_commandf(void (*dumper)(FILE *, void *), void *udata, const char *fmt, ...)
{
	va_list va;
	FILE *req_stream;
	
	if (NULL != (req_stream = open_cnx())) {

		va_start(va, fmt);
		vfprintf(req_stream, fmt, va);
		va_end(va);

		fflush(req_stream);
		dumper(req_stream, udata);
		fclose(req_stream);
		return 1;
	}

	return 0;
}

static int
send_commandv(void (*dumper)(FILE *, void*), void *udata, int argc, char **args)
{
	int i;
	FILE *req_stream;
	
	if (NULL != (req_stream = open_cnx())) {
		for (i=0; i<argc ;i++) {
			fputs(args[i], req_stream);
			fputc(' ', req_stream);
		}
		fputc('\n', req_stream);

		fflush(req_stream);
		dumper(req_stream, udata);
		fclose(req_stream);
		return 1;
	}

	return 0;
}

static int
command_status(int lvl, int argc, char **args)
{
	int rc;
	struct dump_status_arg_s status_args;
	gchar *real_args[] = {"status",NULL};

	bzero(&status_args, sizeof(status_args));
	switch (lvl) {
		case 0:  status_args.how = MINI; break;
		case 1:  status_args.how = MEDIUM; break;
		default: status_args.how = MEDIUM+1; break;
	}
	status_args.argc = argc-1;
	status_args.args = args+1;

	rc = send_commandv(dump_status, &status_args, 1, real_args);
	return !rc
		|| status_args.count_faulty != 0
		|| status_args.count_misses != 0
		|| (status_args.argc > 0 && status_args.count_all == 0);
}

static int
command_status0(int argc, char **args)
{
	return command_status(0, argc, args);
}

static int
command_status1(int argc, char **args)
{
	return command_status(1, argc, args);
}

static int
command_status2(int argc, char **args)
{
	return command_status(2, argc, args);
}

static int
command_start(int argc, char **args)
{
	int rc;
	struct dump_as_is_arg_s dump_args;

	bzero(&dump_args, sizeof(dump_args));
	rc = send_commandv(dump_as_is, &dump_args, argc, args);
	return !rc
		|| dump_args.count_errors != 0
		|| dump_args.count_success == 0;
}

static int
command_stop(int argc, char **args)
{
	int rc;
	struct dump_as_is_arg_s dump_args;

	bzero(&dump_args, sizeof(dump_args));
	rc = send_commandv(dump_as_is, &dump_args, argc, args);
	return !rc
		|| dump_args.count_errors != 0
		|| dump_args.count_success == 0;
}

static int
command_restart(int argc, char **args)
{
	int rc;
	struct dump_as_is_arg_s dump_args;

	bzero(&dump_args, sizeof(dump_args));
	rc = send_commandv(dump_as_is, &dump_args, argc, args);
	return !rc
		|| dump_args.count_errors != 0
		|| dump_args.count_success == 0;
}

static int
command_repair(int argc, char **args)
{
	int rc;
	struct dump_as_is_arg_s dump_args;

	bzero(&dump_args, sizeof(dump_args));
	rc = send_commandv(dump_as_is, &dump_args, argc, args);
	return !rc
		|| dump_args.count_errors != 0
		|| dump_args.count_success == 0;
}

static int
command_reload(int argc, char **args)
{
	int rc;
	struct dump_as_is_arg_s dump_args;

	(void) argc;
	(void) args;
	bzero(&dump_args, sizeof(dump_args));
	rc = send_commandf(dump_as_is, &dump_args, "reload\n");
	return !rc
		|| dump_args.count_errors != 0
		|| dump_args.count_success == 0;
}


/* ------------------------------------------------------------------------- */

static struct command_s COMMANDS[] = {
	{ "status",   command_status0 },
	{ "status2",  command_status1 },
	{ "status3",  command_status2 },
	{ "start",    command_start  },
	{ "stop",     command_stop   },
	{ "restart",  command_restart },
	{ "reload",  command_reload },
	{ "repair",  command_repair },
	{ NULL, NULL }
};

static int
main_options(int argc, char **args)
{
	int opt;

	g_strlcpy(sock_path, GRIDINIT_SOCK_PATH, sizeof(sock_path));
	
	while ((opt = getopt(argc, args, "chS:")) != -1) {
		switch (opt) {
			case 'c':
				flag_color = TRUE;
				break;
			case 'S':
				if (optarg)
					g_strlcpy(sock_path, optarg, sizeof(sock_path));
				break;
			case 'h':
				flag_help = TRUE;
				break;
		}
	}

	return optind;
}

static void
help(char **args)
{
	close(2);
	g_print("Usage: %s [-h|-c|-S SOCK]... (status{,2,3}|start|stop|reload|repair) [ID...]\n", args[0]);
	g_print("\n OPTIONS:\n");
	g_print("  -c      : coloured display\n");
	g_print("  -h      : displays a little help section\n");
	g_print("  -S SOCK : explicit unix socket path\n");
	g_print("\n COMMANDS:\n");
	g_print("  status* : Displays the status of the given processes or groups\n");
	g_print("  start   : Starts the given processes or groups, even if broken\n");
	g_print("  stop    : Stops the given processes or groups, they won't be automatically\n");
	g_print("            restarted even after a configuration reload\n");
	g_print("  restart : Restart the given processes or groups\n");
	g_print("  reload  : reloads the configuration, stopping obsolete processes, starting\n");
	g_print("            the newly discovered. Broken or stopped processes are not restarted\n");
	g_print("  repair  : removes the broken flag set on a process. Start must be called to\n");
	g_print("            restart the process.\n");
	g_print("with ID the key of a process, or '@GROUP', with GROUP the name of a process\n");
	g_print("group\n");
	close(1);
	exit(0);
}

int
main(int argc, char ** args)
{
	struct command_s *cmd;
	int opt_index;
	
	close(0);
	opt_index = main_options(argc, args);

	if (flag_help)
		help(args);
	if (opt_index >= argc)
		help(args);
	
	for (cmd=COMMANDS; cmd->name ;cmd++) {
		if (0 == g_ascii_strcasecmp(cmd->name, args[opt_index])) {
			int rc = cmd->action(argc-opt_index, args+opt_index);
			close(1);
			close(2);
			return rc;
		}
	}

	fprintf(stderr, "\n*** Invalid command ***\n\n");
	help(args);

	close(1);
	close(2);
	return 1;
}

