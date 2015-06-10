#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <linux/limits.h>

#include "extern.h"
#include "watch_err.h"

struct process {
	char proc_name[PATH_MAX];
	pid_t pid;
	time_t time;
	struct process *next;
};

static struct process *process_head = NULL;

/*
 * Add a process to the list. We index by PID primarily to act on child exit
 * values, but check the process name when attempting to start a new child.
 */

static int add_process(const char *name, pid_t pid)
{
	struct process *node = (struct process *)malloc(sizeof(struct process));

	if (node == NULL) {
		log_message(LOG_ALERT, "out of memory adding test binary");
		free_process();
		return (ENOMEM);
	}

	snprintf(node->proc_name, sizeof(node->proc_name), "%s", name);
	node->pid = pid;
	node->time = time(NULL);
	node->next = process_head;
	process_head = node;

	return (ENOERR);
}

/*
 * Free the whole chain. Used on out-of-memory case to hopefully to have enough
 * heap left to create the process kill-list for an orderly shut-down.
 */

void free_process(void)
{
	struct process *last, *current;
	current = process_head;

	while (current != NULL) {
		last = current;
		current = current->next;
		free(last);
	}

	process_head = NULL;
}

/*
 * Remove a finished process from the list, indexed by PID.
 */

static void remove_process(pid_t pid)
{
	struct process *last, *current;
	last = NULL;
	current = process_head;
	while (current != NULL && current->pid != pid) {
		last = current;
		current = current->next;
	}
	if (current != NULL) {
		if (last == NULL)
			process_head = current->next;
		else
			last->next = current->next;
		free(current);
	}
}

/* See if any test processes have exceeded the timeout */
static int check_processes(const char *name, time_t timeout)
{
	struct process *current;
	time_t now = time(NULL);

	current = process_head;
	while (current != NULL) {
		if (!strcmp(current->proc_name, name) && now - current->time > timeout) {
			remove_process(current->pid);
			return (ETOOLONG);
		}
		current = current->next;
	}
	return (ENOERR);
}

/* execute test binary */
int check_bin(char *tbinary, time_t timeout, int version)
{
	pid_t child_pid;
	int result, res = 0;

	if (tbinary == NULL)
		return ENOERR;

	if (timeout > 0)
		res = check_processes(tbinary, timeout);
	if (res == ETOOLONG) {
		log_message(LOG_ERR, "test-binary %s exceeded time limit %ld", tbinary, timeout);
		return res;
	}

	child_pid = fork();
	if (!child_pid) {

		/* Don't want the stdout and stderr of our test program
		 * to cause trouble, so make them go to their respective files */
		strcpy(filename_buf, logdir);
		strcat(filename_buf, "/test-bin.stdout");
		if (!freopen(filename_buf, "a+", stdout))
			exit(errno);
		strcpy(filename_buf, logdir);
		strcat(filename_buf, "/test-bin.stderr");
		if (!freopen(filename_buf, "a+", stderr))
			exit(errno);

		/* now start binary */
		if (version == 0) {
			execl(tbinary, tbinary, NULL);
		} else {
			execl(tbinary, tbinary, "test", NULL);
		}

		/* execl should only return in case of an error */
		/* so we return that error */
		exit(errno);
	} else if (child_pid < 0) {	/* fork failed */
		int err = errno;
		log_message(LOG_ERR, "process fork failed with error = %d = '%s'", err, strerror(err));
		return (EREBOOT);
	} else {
		int ret, err;

		/* fork was okay, add child to process list */
		add_process(tbinary, child_pid);

		/* wait for child(s) to stop, but only after a short sleep */
		/* only sleep for <tint>/2 seconds to make sure we finish on time */
		usleep(tint * 500000);

		do {
			ret = waitpid(-1, &result, WNOHANG);
			err = errno;
			if (ret > 0)
				remove_process(ret);
		} while (ret > 0 && WIFEXITED(result) != 0 && WEXITSTATUS(result) == 0);

		/* check result: */
		/* ret < 0                      => error */
		/* ret == 0                     => no more child returned, however we may already have caught the actual child */
		/* WIFEXITED(result) == 0       => child did not exit normally but was killed by signal which was not caught */
		/* WEXITSTATUS(result) != 0     => child returned an error code */
		if (ret > 0) {
			if (WIFEXITED(result) != 0) {
				/* if one of the scripts returns an error code just return that code */
				ret = WEXITSTATUS(result);
				log_message(LOG_ERR, "test binary %s returned %d = '%s'", tbinary, ret, wd_strerror(ret));
				return (ret);
			} else if (WIFSIGNALED(result) != 0) {
				/* if one of the scripts was killed return ECHKILL */
				log_message(LOG_ERR, "test binary %s was killed by uncaught signal %d", tbinary, WTERMSIG(result));
				return (ECHKILL);
			}
		} else {
			/* in case there are still old childs running due to an error */
			/* log that error */
			if (ret != 0 && err != 0 && err != ECHILD) {
				err = errno;
				log_message(LOG_ERR, "child %d did not exit immediately (error = %d = '%s')", child_pid, err, strerror(err));
				if (softboot)
					return (err);
			}
		}
	}
	return (ENOERR);
}
