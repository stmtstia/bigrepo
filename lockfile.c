/*
 * Copyright (c) 2005, Junio C Hamano
 */
#include "cache.h"
#include "sigchain.h"

/*
 * File write-locks as used by Git.
 *
 * When a file at $FILENAME needs to be written, it is done as
 * follows:
 *
 * 1. Obtain a lock on the file by creating a lockfile at path
 *    $FILENAME.lock.  The file is opened for read/write using O_CREAT
 *    and O_EXCL mode to ensure that it doesn't already exist.  Such a
 *    lock file is respected by writers *but not by readers*.
 *
 *    Usually, if $FILENAME is a symlink, then it is resolved, and the
 *    file ultimately pointed to is the one that is locked and later
 *    replaced.  However, if LOCK_NODEREF is used, then $FILENAME
 *    itself is locked and later replaced, even if it is a symlink.
 *
 * 2. Write the new file contents to the lockfile.
 *
 * 3. Move the lockfile to its final destination using rename(2).
 *
 * Instead of (3), the change can be rolled back by deleting lockfile.
 *
 * This module keeps track of all locked files in lock_file_list.
 * When the first file is locked, it registers an atexit(3) handler;
 * when the program exits, the handler rolls back any files that have
 * been locked but were never committed or rolled back.
 *
 * A lock_file is owned by the process that created it. The lock_file
 * object has an "owner" field that records its owner. This field is
 * used to prevent a forked process from closing a lock_file of its
 * parent.
 *
 * A lock_file object can be in several states:
 *
 * - Uninitialized.  In this state the object's on_list field must be
 *   zero but the rest of its contents need not be initialized.  As
 *   soon as the object is used in any way, it is irrevocably
 *   registered in the lock_file_list, and on_list is set.
 *
 * - Locked, lockfile open (after hold_lock_file_for_update(),
 *   hold_lock_file_for_append(), or reopen_lock_file()). In this
 *   state, the lockfile exists, filename holds the filename of the
 *   lockfile, fd holds a file descriptor open for writing to the
 *   lockfile, and owner holds the PID of the process that locked the
 *   file.
 *
 * - Locked, lockfile closed (after close_lock_file()).  Same as the
 *   previous state, except that the lockfile is closed and fd is -1.
 *
 * - Unlocked (after commit_lock_file(), rollback_lock_file(), or a
 *   failed attempt to lock).  In this state, filename[0] == '\0' and
 *   fd is -1.  The object is left registered in the lock_file_list,
 *   and on_list is set.
 *
 * See Documentation/api-lockfile.txt for more information.
 */

static struct lock_file *lock_file_list;

static void remove_lock_file(void)
{
	pid_t me = getpid();

	while (lock_file_list) {
		if (lock_file_list->owner == me)
			rollback_lock_file(lock_file_list);
		lock_file_list = lock_file_list->next;
	}
}

static void remove_lock_file_on_signal(int signo)
{
	remove_lock_file();
	sigchain_pop(signo);
	raise(signo);
}

/*
 * p = absolute or relative path name
 *
 * Return a pointer into p showing the beginning of the last path name
 * element.  If p is empty or the root directory ("/"), just return p.
 */
static char *last_path_elm(char *p)
{
	/* r starts pointing to null at the end of the string */
	char *r = strchr(p, '\0');

	if (r == p)
		return p; /* just return empty string */

	r--; /* back up to last non-null character */

	/* back up past trailing slashes, if any */
	while (r > p && *r == '/')
		r--;

	/*
	 * then go backwards until I hit a slash, or the beginning of
	 * the string
	 */
	while (r > p && *(r-1) != '/')
		r--;
	return r;
}


/* We allow "recursive" symbolic links. Only within reason, though */
#define MAXDEPTH 5

/*
 * p = path that may be a symlink
 * s = full size of p
 *
 * If p is a symlink, attempt to overwrite p with a path to the real
 * file or directory (which may or may not exist), following a chain of
 * symlinks if necessary.  Otherwise, leave p unmodified.
 *
 * This is a best-effort routine.  If an error occurs, p will either be
 * left unmodified or will name a different symlink in a symlink chain
 * that started with p's initial contents.
 *
 * Always returns p.
 */

static char *resolve_symlink(char *p, size_t s)
{
	int depth = MAXDEPTH;

	while (depth--) {
		char link[PATH_MAX];
		int link_len = readlink(p, link, sizeof(link));
		if (link_len < 0) {
			/* not a symlink anymore */
			return p;
		}
		else if (link_len < sizeof(link))
			/* readlink() never null-terminates */
			link[link_len] = '\0';
		else {
			warning("%s: symlink too long", p);
			return p;
		}

		if (is_absolute_path(link)) {
			/* absolute path simply replaces p */
			if (link_len < s)
				strcpy(p, link);
			else {
				warning("%s: symlink too long", p);
				return p;
			}
		} else {
			/*
			 * link is a relative path, so I must replace the
			 * last element of p with it.
			 */
			char *r = (char *)last_path_elm(p);
			if (r - p + link_len < s)
				strcpy(r, link);
			else {
				warning("%s: symlink too long", p);
				return p;
			}
		}
	}
	return p;
}

/* Make sure errno contains a meaningful value on error */
static int lock_file(struct lock_file *lk, const char *path, int flags)
{
	/*
	 * subtract LOCK_SUFFIX_LEN from size to make sure there's
	 * room for adding ".lock" for the lock file name:
	 */
	static const size_t max_path_len = sizeof(lk->filename) -
					   LOCK_SUFFIX_LEN;

	if (!lock_file_list) {
		/* One-time initialization */
		sigchain_push_common(remove_lock_file_on_signal);
		atexit(remove_lock_file);
	}

	if (!lk->on_list) {
		/* Initialize *lk and add it to lock_file_list: */
		lk->fd = -1;
		lk->owner = 0;
		lk->on_list = 1;
		lk->filename[0] = 0;
		lk->next = lock_file_list;
		lock_file_list = lk;
	}

	if (strlen(path) >= max_path_len) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(lk->filename, path);
	if (!(flags & LOCK_NODEREF))
		resolve_symlink(lk->filename, max_path_len);
	strcat(lk->filename, LOCK_SUFFIX);
	lk->fd = open(lk->filename, O_RDWR | O_CREAT | O_EXCL, 0666);
	if (lk->fd < 0) {
		lk->filename[0] = 0;
		return -1;
	}
	lk->owner = getpid();
	if (adjust_shared_perm(lk->filename)) {
		int save_errno = errno;
		error("cannot fix permission bits on %s", lk->filename);
		rollback_lock_file(lk);
		errno = save_errno;
		return -1;
	}
	return lk->fd;
}

void unable_to_lock_message(const char *path, int err, struct strbuf *buf)
{
	if (err == EEXIST) {
		strbuf_addf(buf, "Unable to create '%s.lock': %s.\n\n"
		    "If no other git process is currently running, this probably means a\n"
		    "git process crashed in this repository earlier. Make sure no other git\n"
		    "process is running and remove the file manually to continue.",
			    absolute_path(path), strerror(err));
	} else
		strbuf_addf(buf, "Unable to create '%s.lock': %s",
			    absolute_path(path), strerror(err));
}

int unable_to_lock_error(const char *path, int err)
{
	struct strbuf buf = STRBUF_INIT;

	unable_to_lock_message(path, err, &buf);
	error("%s", buf.buf);
	strbuf_release(&buf);
	return -1;
}

NORETURN void unable_to_lock_die(const char *path, int err)
{
	struct strbuf buf = STRBUF_INIT;

	unable_to_lock_message(path, err, &buf);
	die("%s", buf.buf);
}

/* This should return a meaningful errno on failure */
int hold_lock_file_for_update(struct lock_file *lk, const char *path, int flags)
{
	int fd = lock_file(lk, path, flags);
	if (fd < 0 && (flags & LOCK_DIE_ON_ERROR))
		unable_to_lock_die(path, errno);
	return fd;
}

int hold_lock_file_for_append(struct lock_file *lk, const char *path, int flags)
{
	int fd, orig_fd;

	fd = lock_file(lk, path, flags);
	if (fd < 0) {
		if (flags & LOCK_DIE_ON_ERROR)
			unable_to_lock_die(path, errno);
		return fd;
	}

	orig_fd = open(path, O_RDONLY);
	if (orig_fd < 0) {
		if (errno != ENOENT) {
			if (flags & LOCK_DIE_ON_ERROR)
				die("cannot open '%s' for copying", path);
			rollback_lock_file(lk);
			return error("cannot open '%s' for copying", path);
		}
	} else if (copy_fd(orig_fd, fd)) {
		if (flags & LOCK_DIE_ON_ERROR)
			exit(128);
		rollback_lock_file(lk);
		return -1;
	}
	return fd;
}

int close_lock_file(struct lock_file *lk)
{
	int fd = lk->fd;
	lk->fd = -1;
	return close(fd);
}

int reopen_lock_file(struct lock_file *lk)
{
	if (0 <= lk->fd)
		die(_("BUG: reopen a lockfile that is still open"));
	if (!lk->filename[0])
		die(_("BUG: reopen a lockfile that has been committed"));
	lk->fd = open(lk->filename, O_WRONLY);
	return lk->fd;
}

int commit_lock_file(struct lock_file *lk)
{
	char result_file[PATH_MAX];

	if (!lk->filename[0])
		die("BUG: attempt to commit unlocked object");

	if (lk->fd >= 0 && close_lock_file(lk))
		return -1;

	strcpy(result_file, lk->filename);
	/* remove ".lock": */
	result_file[strlen(result_file) - LOCK_SUFFIX_LEN] = 0;

	if (rename(lk->filename, result_file))
		return -1;
	lk->filename[0] = 0;
	return 0;
}

int hold_locked_index(struct lock_file *lk, int die_on_error)
{
	return hold_lock_file_for_update(lk, get_index_file(),
					 die_on_error
					 ? LOCK_DIE_ON_ERROR
					 : 0);
}

void rollback_lock_file(struct lock_file *lk)
{
	if (!lk->filename[0])
		return;

	if (lk->fd >= 0)
		close_lock_file(lk);
	unlink_or_warn(lk->filename);
	lk->filename[0] = 0;
}
