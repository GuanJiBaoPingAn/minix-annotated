/* Implement entry point to select system call.
 *
 * The entry points into this file are
 *   do_select:	       perform the SELECT system call
 *   select_callback:  notify select system of possible fd operation
 *   select_unsuspend_by_endpt: cancel a blocking select on exiting driver
 *
 * The select code uses minimal locking, so that the replies from character
 * drivers can be processed without blocking. Filps are locked only for pipes.
 * We make the assumption that any other structures and fields are safe to
 * check (and possibly change) as long as we know that a process is blocked on
 * a select(2) call, meaning that all involved filps are guaranteed to stay
 * open until either we finish the select call, it the process gets interrupted
 * by a signal.
 */

#include "fs.h"
#include <sys/fcntl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <minix/callnr.h>
#include <minix/u64.h>
#include <string.h>
#include <assert.h>

#include "file.h"
#include "vnode.h"

/* max. number of simultaneously pending select() calls */
#define MAXSELECTS 25
#define FROM_PROC 0
#define TO_PROC   1

#define USECPERSEC 1000000	/* number of microseconds in a second */

typedef fd_set *ixfer_fd_set_ptr;

static struct selectentry {
  struct fproc *requestor;	/* slot is free iff this is NULL */
  endpoint_t req_endpt;
  fd_set readfds, writefds, errorfds;
  fd_set ready_readfds, ready_writefds, ready_errorfds;
  ixfer_fd_set_ptr vir_readfds, vir_writefds, vir_errorfds;
  struct filp *filps[OPEN_MAX];
  int type[OPEN_MAX];
  int nfds, nreadyfds;
  int error;
  char block;
  char starting;
  clock_t expiry;
  minix_timer_t timer;	/* if expiry > 0 */
} selecttab[MAXSELECTS];

static int copy_fdsets(struct selectentry *se, int nfds, int direction);
static void filp_status(struct filp *fp, int status);
static int is_deferred(struct selectentry *se);
static void restart_proc(struct selectentry *se);
static void ops2tab(int ops, int fd, struct selectentry *e);
static int is_regular_file(struct filp *f);
static int is_pipe(struct filp *f);
static int is_char_device(struct filp *f);
static int is_sock_device(struct filp *f);
static void select_lock_filp(struct filp *f, int ops);
static int select_request_file(struct filp *f, int *ops, int block,
	struct fproc *rfp);
static int select_request_char(struct filp *f, int *ops, int block,
	struct fproc *rfp);
static int select_request_sock(struct filp *f, int *ops, int block,
	struct fproc *rfp);
static int select_request_pipe(struct filp *f, int *ops, int block,
	struct fproc *rfp);
static void select_cancel_all(struct selectentry *e);
static void select_cancel_filp(struct filp *f);
static void select_return(struct selectentry *);
static void select_restart_filps(void);
static int tab2ops(int fd, struct selectentry *e);
static void wipe_select(struct selectentry *s);
void select_timeout_check(int s);

static struct fdtype {
	int (*select_request)(struct filp *, int *ops, int block,
		struct fproc *rfp);
	int (*type_match)(struct filp *f);
} fdtypes[] = {
	{ select_request_char, is_char_device },
	{ select_request_sock, is_sock_device },
	{ select_request_file, is_regular_file },
	{ select_request_pipe, is_pipe },
};
#define SEL_FDS		(sizeof(fdtypes) / sizeof(fdtypes[0]))

/*===========================================================================*
 *				do_select				     *
 *===========================================================================*/
int do_select(void)
{
/* Implement the select(nfds, readfds, writefds, errorfds, timeout) system
 * call. First we copy the arguments and verify their sanity. Then we check
 * whether there are file descriptors that satisfy the select call right off
 * the bat. If so, or if there are no ready file descriptors but the process
 * requested to return immediately, we return the result. Otherwise we set a
 * timeout and wait for either the file descriptors to become ready or the
 * timer to go off. If no timeout value was provided, we wait indefinitely.
 */
  int r, nfds, do_timeout, fd, type, s;
  struct filp *f;
  unsigned int ops;
  struct timeval timeout;
  struct selectentry *se;
  vir_bytes vtimeout;
  clock_t ticks;

  nfds = job_m_in.m_lc_vfs_select.nfds;
  vtimeout = job_m_in.m_lc_vfs_select.timeout;

  /* Sane amount of file descriptors? */
  if (nfds < 0 || nfds > OPEN_MAX) return(EINVAL);

  /* Find a slot to store this select request */
  for (s = 0; s < MAXSELECTS; s++)
	if (selecttab[s].requestor == NULL) /* Unused slot */
		break;
  if (s >= MAXSELECTS) return(ENOSPC);

  se = &selecttab[s];
  wipe_select(se);	/* Clear results of previous usage */
  se->requestor = fp;
  se->req_endpt = who_e;
  se->vir_readfds = job_m_in.m_lc_vfs_select.readfds;
  se->vir_writefds = job_m_in.m_lc_vfs_select.writefds;
  se->vir_errorfds = job_m_in.m_lc_vfs_select.errorfds;

  /* Copy fdsets from the process */
  if ((r = copy_fdsets(se, nfds, FROM_PROC)) != OK) {
	se->requestor = NULL;
	return(r);
  }

  /* Did the process set a timeout value? If so, retrieve it. */
  if (vtimeout != 0) {
	r = sys_datacopy_wrapper(who_e, vtimeout, SELF, (vir_bytes) &timeout,
		sizeof(timeout));

	/* No nonsense in the timeval */
	if (r == OK && (timeout.tv_sec < 0 || timeout.tv_usec < 0 ||
	    timeout.tv_usec >= USECPERSEC))
		r = EINVAL;

	if (r != OK) {
		se->requestor = NULL;
		return(r);
	}
	do_timeout = 1;
  } else
	do_timeout = 0;

  /* If there is no timeout, we block forever. Otherwise, we block up to the
   * specified time interval.
   */
  if (!do_timeout)	/* No timeout value set */
	se->block = 1;
  else if (do_timeout && (timeout.tv_sec > 0 || timeout.tv_usec > 0))
	se->block = 1;
  else			/* timeout set as (0,0) - this effects a poll */
	se->block = 0;
  se->expiry = 0;	/* no timer set (yet) */

  /* We are going to lock filps, and that means that while locking a second
   * filp, we might already get the results for the first one. In that case,
   * the incoming results must not cause the select call to finish prematurely.
   */
  se->starting = TRUE;

  /* Verify that file descriptors are okay to select on */
  for (fd = 0; fd < nfds; fd++) {
	/* Because the select() interface implicitly includes file descriptors
	 * you might not want to select on, we have to figure out whether we're
	 * interested in them. Typically, these file descriptors include fd's
	 * inherited from the parent proc and file descriptors that have been
	 * close()d, but had a lower fd than one in the current set.
	 */
	if (!(ops = tab2ops(fd, se)))
		continue; /* No operations set; nothing to do for this fd */

	/* Get filp belonging to this fd. If this fails, there are two causes:
	 * either the given file descriptor was bad, or the associated filp is
	 * closed (in the FILP_CLOSED sense) as a result of invalidation. Only
	 * the former is a select error. The latter should result in operations
	 * being returned as ready on the file descriptor, since subsequent
	 * I/O calls are guaranteed to return I/O errors on such descriptors.
	 */
	f = se->filps[fd] = get_filp(fd, VNODE_READ);
	if (f == NULL && err_code != EIO) {
		assert(err_code == EBADF);

		/* We may already have adjusted filp_selectors on previous
		 * file pointers in the set, so do not simply return here.
		 */
		se->error = EBADF;
		break;
	}

	/* Check file types. According to POSIX 2008:
	 * "The pselect() and select() functions shall support regular files,
	 * terminal and pseudo-terminal devices, FIFOs, pipes, and sockets. The
	 * behavior of pselect() and select() on file descriptors that refer to
	 * other types of file is unspecified."
	 *
	 * In our case, terminal and pseudo-terminal devices are handled by the
	 * TTY and PTY character drivers respectively.  Sockets are handled by
	 * by their respective socket drivers.  Additionally, we give other
	 * character drivers the chance to handle select for any of their
	 * device nodes. Some may not implement support for select and let
	 * libchardriver return EBADF, which we then pass to the calling
	 * process once we receive the reply.
	 *
	 * If we could not access the file pointer at all, it will have been
	 * closed due to invalidation after a service crash. In that case, we
	 * skip type matching and simply return pending operations as ready.
	 */
	se->type[fd] = -1;
	if (f == NULL)
		continue; /* closed, skip type matching */
	for (type = 0; type < SEL_FDS; type++) {
		if (fdtypes[type].type_match(f)) {
			se->type[fd] = type;
			se->nfds = fd+1;
			se->filps[fd]->filp_selectors++;
			break;
		}
	}
	unlock_filp(f);
	if (se->type[fd] == -1) { /* Type not found */
		se->error = EBADF;
		break;
	}
  }

  /* If an error occurred already, undo any changes so far and return. */
  if (se->error != OK) {
	select_cancel_all(se);
	se->requestor = NULL;
	return(se->error);
  }

  /* Check all file descriptors in the set whether one is 'ready' now */
  for (fd = 0; fd < nfds; fd++) {
	/* Again, check for involuntarily selected fd's */
	if (!(ops = tab2ops(fd, se)))
		continue; /* No operations set; nothing to do for this fd */

	/* File descriptors selected for reading that are not opened for
	 * reading should be marked as readable, as read calls would fail
	 * immediately. The same applies to writing. For file descriptors for
	 * which the file pointer is already closed (f==NULL), return readable
	 * and writable operations (if requested) and skip the rest.
	 */
	f = se->filps[fd];
	if (f == NULL) {
		ops2tab(SEL_RD | SEL_WR, fd, se);
		continue;
	}
	if ((ops & SEL_RD) && !(f->filp_mode & R_BIT)) {
		ops2tab(SEL_RD, fd, se);
		ops &= ~SEL_RD;
	}
	if ((ops & SEL_WR) && !(f->filp_mode & W_BIT)) {
		ops2tab(SEL_WR, fd, se);
		ops &= ~SEL_WR;
	}
	/* Test filp for select operations if not already done so. e.g.,
	 * processes sharing a filp and both doing a select on that filp. */
	if ((f->filp_select_ops & ops) != ops) {
		int wantops;

		wantops = (f->filp_select_ops |= ops);
		type = se->type[fd];
		assert(type >= 0);
		select_lock_filp(f, wantops);
		r = fdtypes[type].select_request(f, &wantops, se->block, fp);
		unlock_filp(f);
		if (r != OK && r != SUSPEND) {
			se->error = r;
			break; /* Error or bogus return code; abort */
		}

		/* The select request above might have turned on/off some
		 * operations because they were 'ready' or not meaningful.
		 * Either way, we might have a result and we need to store them
		 * in the select table entry. */
		if (wantops & ops) ops2tab(wantops, fd, se);
	}
  }

  /* At this point there won't be any blocking calls anymore. */
  se->starting = FALSE;

  if ((se->nreadyfds > 0 || se->error != OK || !se->block) &&
		!is_deferred(se)) {
	/* An error occurred, or fd's were found that were ready to go right
	 * away, and/or we were instructed not to block at all. Must return
	 * immediately. Do not copy FD sets if an error occurred.
	 */
	if (se->error != OK)
		r = se->error;
	else
		r = copy_fdsets(se, se->nfds, TO_PROC);
	select_cancel_all(se);
	se->requestor = NULL;

	if (r != OK)
		return(r);
	return(se->nreadyfds);
  }

  /* Convert timeval to ticks and set the timer. If it fails, undo
   * all, return error.
   */
  if (do_timeout && se->block) {
	/* Open Group:
	 * "If the requested timeout interval requires a finer
	 * granularity than the implementation supports, the
	 * actual timeout interval shall be rounded up to the next
	 * supported value."
	 */
	if (timeout.tv_sec >= (TMRDIFF_MAX - 1) / system_hz) {
		ticks = TMRDIFF_MAX; /* silently truncate */
	} else {
		ticks = timeout.tv_sec * system_hz +
		    (timeout.tv_usec * system_hz + USECPERSEC-1) / USECPERSEC;
	}
	assert(ticks != 0 && ticks <= TMRDIFF_MAX);
	se->expiry = ticks;
	set_timer(&se->timer, ticks, select_timeout_check, s);
  }

  /* process now blocked */
  suspend(FP_BLOCKED_ON_SELECT);
  return(SUSPEND);
}

/*===========================================================================*
 *				is_deferred				     *
 *===========================================================================*/
static int is_deferred(struct selectentry *se)
{
/* Find out whether this select has pending initial replies */

  int fd;
  struct filp *f;

  /* The select call must have finished its initialization at all. */
  if (se->starting) return(TRUE);

  for (fd = 0; fd < se->nfds; fd++) {
	if ((f = se->filps[fd]) == NULL) continue;
	if (f->filp_select_flags & (FSF_UPDATE|FSF_BUSY)) return(TRUE);
  }

  return(FALSE);
}


/*===========================================================================*
 *				is_regular_file				     *
 *===========================================================================*/
static int is_regular_file(struct filp *f)
{
  return(f && f->filp_vno && S_ISREG(f->filp_vno->v_mode));
}

/*===========================================================================*
 *				is_pipe					     *
 *===========================================================================*/
static int is_pipe(struct filp *f)
{
/* Recognize either anonymous pipe or named pipe (FIFO) */
  return(f && f->filp_vno && S_ISFIFO(f->filp_vno->v_mode));
}

/*===========================================================================*
 *				is_char_device				     *
 *===========================================================================*/
static int is_char_device(struct filp *f)
{
/* See if this filp is a handle on a character device. This function MUST NOT
 * block its calling thread. The given filp may or may not be locked.
 */

  return (f && f->filp_vno && S_ISCHR(f->filp_vno->v_mode));
}

/*===========================================================================*
 *				is_sock_device				     *
 *===========================================================================*/
static int is_sock_device(struct filp *f)
{
/* See if this filp is a handle on a socket device. This function MUST NOT
 * block its calling thread. The given filp may or may not be locked.
 */

  return (f && f->filp_vno && S_ISSOCK(f->filp_vno->v_mode));
}

/*===========================================================================*
 *				select_filter				     *
 *===========================================================================*/
static int select_filter(struct filp *f, int *ops, int block)
{
/* Determine which select operations can be satisfied immediately and which
 * should be requested.  Used for character and socket devices.  This function
 * MUST NOT block its calling thread.
 */
  int rops;

  rops = *ops;

  /* By default, nothing to do */
  *ops = 0;

  /*
   * If we have previously asked the driver to notify us about certain ready
   * operations, but it has not notified us yet, then we can safely assume that
   * those operations are not ready right now.  Therefore, if this call is not
   * supposed to block, we can disregard the pending operations as not ready.
   * We must make absolutely sure that the flags are "stable" right now though:
   * we are neither waiting to query the driver about them (FSF_UPDATE) nor
   * querying the driver about them right now (FSF_BUSY).  This is a dangerous
   * case of premature optimization and may be removed altogether if it proves
   * to continue to be a source of bugs.
   */
  if (!block && !(f->filp_select_flags & (FSF_UPDATE | FSF_BUSY)) &&
      (f->filp_select_flags & FSF_BLOCKED)) {
	if ((rops & SEL_RD) && (f->filp_select_flags & FSF_RD_BLOCK))
		rops &= ~SEL_RD;
	if ((rops & SEL_WR) && (f->filp_select_flags & FSF_WR_BLOCK))
		rops &= ~SEL_WR;
	if ((rops & SEL_ERR) && (f->filp_select_flags & FSF_ERR_BLOCK))
		rops &= ~SEL_ERR;
	if (!(rops & (SEL_RD|SEL_WR|SEL_ERR)))
		return(0);
  }

  f->filp_select_flags |= FSF_UPDATE;
  if (block) {
	rops |= SEL_NOTIFY;
	if (rops & SEL_RD)	f->filp_select_flags |= FSF_RD_BLOCK;
	if (rops & SEL_WR)	f->filp_select_flags |= FSF_WR_BLOCK;
	if (rops & SEL_ERR)	f->filp_select_flags |= FSF_ERR_BLOCK;
  }

  if (f->filp_select_flags & FSF_BUSY)
	return(SUSPEND);

  return rops;
}

/*===========================================================================*
 *				select_request_char			     *
 *===========================================================================*/
static int select_request_char(struct filp *f, int *ops, int block,
	struct fproc *rfp)
{
/* Check readiness status on a character device. Unless suitable results are
 * available right now, this will only initiate the polling process, causing
 * result processing to be deferred. This function MUST NOT block its calling
 * thread. The given filp may or may not be locked.
 */
  dev_t dev;
  int r, rops;
  struct dmap *dp;

  /* Start by remapping the device node number to a "real" device number. Those
   * two are different only for CTTY_MAJOR aka /dev/tty, but that one single
   * exception requires quite some extra effort here: the select code matches
   * character driver replies to their requests based on the device number, so
   * it needs to be aware that device numbers may be mapped. The idea is to
   * perform the mapping once and store the result in the filp object, so that
   * at least we don't run into problems when a process loses its controlling
   * terminal while doing a select (see also free_proc). It should be noted
   * that it is possible that multiple processes share the same /dev/tty filp,
   * and they may not all have a controlling terminal. The ctty-less processes
   * should never pass the mapping; a more problematic case is checked below.
   *
   * The cdev_map call also checks the major number for rough validity, so that
   * we can use it to index the dmap array safely a bit later.
   */
  if ((dev = cdev_map(f->filp_vno->v_sdev, rfp)) == NO_DEV)
	return(ENXIO);

  if (f->filp_select_dev != NO_DEV && f->filp_select_dev != dev) {
	/* Currently, this case can occur as follows: a process with a
	 * controlling terminal opens /dev/tty and forks, the new child starts
	 * a new session, opens a new controlling terminal, and both parent and
	 * child call select on the /dev/tty file descriptor. If this case ever
	 * becomes real, a better solution may be to force-close a filp for
	 * /dev/tty when a new controlling terminal is opened.
	 */
	printf("VFS: file pointer has multiple controlling TTYs!\n");
	return(EIO);
  }
  f->filp_select_dev = dev; /* set before possibly suspending */

  if ((rops = select_filter(f, ops, block)) <= 0)
	return(rops); /* OK or suspend: nothing to do for now */

  dp = &dmap[major(dev)];
  if (dp->dmap_sel_busy)
	return(SUSPEND);

  f->filp_select_flags &= ~FSF_UPDATE;
  r = cdev_select(dev, rops);
  if (r != OK)
	return(r);

  dp->dmap_sel_busy = TRUE;
  dp->dmap_sel_filp = f;
  f->filp_select_flags |= FSF_BUSY;

  return(SUSPEND);
}

/*===========================================================================*
 *				select_request_sock			     *
 *===========================================================================*/
static int select_request_sock(struct filp *f, int *ops, int block,
	struct fproc *rfp __unused)
{
/* Check readiness status on a socket device. Unless suitable results are
 * available right now, this will only initiate the polling process, causing
 * result processing to be deferred. This function MUST NOT block its calling
 * thread. The given filp may or may not be locked.
 */
  struct smap *sp;
  dev_t dev;
  int r, rops;

  dev = f->filp_vno->v_sdev;

  if ((sp = get_smap_by_dev(dev, NULL)) == NULL)
	return(ENXIO);	/* this should not happen */

  f->filp_select_dev = dev; /* set before possibly suspending */

  if ((rops = select_filter(f, ops, block)) <= 0)
	return(rops); /* OK or suspend: nothing to do for now */

  if (sp->smap_sel_busy)
	return(SUSPEND);

  f->filp_select_flags &= ~FSF_UPDATE;
  r = sdev_select(dev, rops);
  if (r != OK)
	return(r);

  sp->smap_sel_busy = TRUE;
  sp->smap_sel_filp = f;
  f->filp_select_flags |= FSF_BUSY;

  return(SUSPEND);
}

/*===========================================================================*
 *				select_request_file			     *
 *===========================================================================*/
static int select_request_file(struct filp *UNUSED(f), int *UNUSED(ops),
  int UNUSED(block), struct fproc *UNUSED(rfp))
{
  /* Files are always ready, so output *ops is input *ops */
  return(OK);
}

/*===========================================================================*
 *				select_request_pipe			     *
 *===========================================================================*/
static int select_request_pipe(struct filp *f, int *ops, int block,
	struct fproc *UNUSED(rfp))
{
/* Check readiness status on a pipe. The given filp is locked. This function
 * may block its calling thread if necessary.
 */
  int orig_ops, r = 0, err;

  orig_ops = *ops;

  if ((*ops & (SEL_RD|SEL_ERR))) {
	/* Check if we can read 1 byte */
	err = pipe_check(f, READING, f->filp_flags & ~O_NONBLOCK, 1,
			 1 /* Check only */);

	if (err != SUSPEND)
		r |= SEL_RD;
	if (err < 0 && err != SUSPEND)
		r |= SEL_ERR;
  }

  if ((*ops & (SEL_WR|SEL_ERR))) {
	/* Check if we can write 1 byte */
	err = pipe_check(f, WRITING, f->filp_flags & ~O_NONBLOCK, 1,
			 1 /* Check only */);

	if (err != SUSPEND)
		r |= SEL_WR;
	if (err < 0 && err != SUSPEND)
		r |= SEL_ERR;
  }

  /* Some options we collected might not be requested. */
  *ops = r & orig_ops;

  if (!*ops && block)
	f->filp_pipe_select_ops |= orig_ops;

  return(OK);
}

/*===========================================================================*
 *				tab2ops					     *
 *===========================================================================*/
static int tab2ops(int fd, struct selectentry *e)
{
  int ops = 0;
  if (FD_ISSET(fd, &e->readfds))  ops |= SEL_RD;
  if (FD_ISSET(fd, &e->writefds)) ops |= SEL_WR;
  if (FD_ISSET(fd, &e->errorfds)) ops |= SEL_ERR;

  return(ops);
}


/*===========================================================================*
 *				ops2tab					     *
 *===========================================================================*/
static void ops2tab(int ops, int fd, struct selectentry *e)
{
  if ((ops & SEL_RD) && e->vir_readfds && FD_ISSET(fd, &e->readfds) &&
      !FD_ISSET(fd, &e->ready_readfds)) {
	FD_SET(fd, &e->ready_readfds);
	e->nreadyfds++;
  }

  if ((ops & SEL_WR) && e->vir_writefds && FD_ISSET(fd, &e->writefds) &&
      !FD_ISSET(fd, &e->ready_writefds)) {
	FD_SET(fd, &e->ready_writefds);
	e->nreadyfds++;
  }

  if ((ops & SEL_ERR) && e->vir_errorfds && FD_ISSET(fd, &e->errorfds) &&
      !FD_ISSET(fd, &e->ready_errorfds)) {
	FD_SET(fd, &e->ready_errorfds);
	e->nreadyfds++;
  }
}


/*===========================================================================*
 *				copy_fdsets				     *
 *===========================================================================*/
static int copy_fdsets(struct selectentry *se, int nfds, int direction)
{
/* Copy FD sets from or to the user process calling select(2). This function
 * MUST NOT block the calling thread.
 */
  int r;
  size_t fd_setsize;
  endpoint_t src_e, dst_e;
  fd_set *src_fds, *dst_fds;

  if (nfds < 0 || nfds > OPEN_MAX)
	panic("select copy_fdsets: nfds wrong: %d", nfds);

  /* Only copy back as many bits as the user expects. */
  fd_setsize = (size_t) (howmany(nfds, __NFDBITS) * sizeof(__fd_mask));

  /* Set source and destination endpoints */
  src_e = (direction == FROM_PROC) ? se->req_endpt : SELF;
  dst_e = (direction == FROM_PROC) ? SELF : se->req_endpt;

  /* read set */
  src_fds = (direction == FROM_PROC) ? se->vir_readfds : &se->ready_readfds;
  dst_fds = (direction == FROM_PROC) ? &se->readfds : se->vir_readfds;
  if (se->vir_readfds) {
	r = sys_datacopy_wrapper(src_e, (vir_bytes) src_fds, dst_e,
			(vir_bytes) dst_fds, fd_setsize);
	if (r != OK) return(r);
  }

  /* write set */
  src_fds = (direction == FROM_PROC) ? se->vir_writefds : &se->ready_writefds;
  dst_fds = (direction == FROM_PROC) ? &se->writefds : se->vir_writefds;
  if (se->vir_writefds) {
	r = sys_datacopy_wrapper(src_e, (vir_bytes) src_fds, dst_e,
			(vir_bytes) dst_fds, fd_setsize);
	if (r != OK) return(r);
  }

  /* error set */
  src_fds = (direction == FROM_PROC) ? se->vir_errorfds : &se->ready_errorfds;
  dst_fds = (direction == FROM_PROC) ? &se->errorfds : se->vir_errorfds;
  if (se->vir_errorfds) {
	r = sys_datacopy_wrapper(src_e, (vir_bytes) src_fds, dst_e,
			(vir_bytes) dst_fds, fd_setsize);
	if (r != OK) return(r);
  }

  return(OK);
}


/*===========================================================================*
 *				select_cancel_all			     *
 *===========================================================================*/
static void select_cancel_all(struct selectentry *se)
{
/* Cancel select, possibly on success. Decrease select usage and cancel timer.
 * This function MUST NOT block its calling thread.
 */

  int fd;
  struct filp *f;

  for (fd = 0; fd < se->nfds; fd++) {
	if ((f = se->filps[fd]) == NULL) continue;
	se->filps[fd] = NULL;
	select_cancel_filp(f);
  }

  if (se->expiry > 0) {
	cancel_timer(&se->timer);
	se->expiry = 0;
  }

  se->requestor = NULL;
}

/*===========================================================================*
 *				select_cancel_filp			     *
 *===========================================================================*/
static void select_cancel_filp(struct filp *f)
{
/* Reduce the number of select users of this filp. This function MUST NOT block
 * its calling thread.
 */
  devmajor_t major;
  struct smap *sp;

  assert(f);
  assert(f->filp_selectors > 0);
  assert(f->filp_count > 0);

  f->filp_selectors--;
  if (f->filp_selectors == 0) {
	/* No one selecting on this filp anymore, forget about select state */
	f->filp_select_ops = 0;
	f->filp_select_flags = 0;
	f->filp_pipe_select_ops = 0;

	/* If this filp is the subject of an ongoing select query to a
	 * character or socket device, mark the query as stale, so that this
	 * filp will not be checked when the result arrives. The filp select
	 * device may still be NO_DEV if do_select fails on the initial fd
	 * check.
	 */
	if (is_char_device(f) && f->filp_select_dev != NO_DEV) {
		major = major(f->filp_select_dev);
		if (dmap[major].dmap_sel_busy &&
			dmap[major].dmap_sel_filp == f)
			dmap[major].dmap_sel_filp = NULL; /* leave _busy set */
		f->filp_select_dev = NO_DEV;
	} else if (is_sock_device(f) && f->filp_select_dev != NO_DEV) {
		if ((sp = get_smap_by_dev(f->filp_select_dev, NULL)) != NULL &&
		    sp->smap_sel_busy && sp->smap_sel_filp == f)
			sp->smap_sel_filp = NULL;	/* leave _busy set */
		f->filp_select_dev = NO_DEV;
	}
  }
}

/*===========================================================================*
 *				select_return				     *
 *===========================================================================*/
static void select_return(struct selectentry *se)
{
/* Return the results of a select call to the user process and revive the
 * process. This function MUST NOT block its calling thread.
 */
  int r;

  assert(!is_deferred(se));	/* Not done yet, first wait for async reply */

  select_cancel_all(se);

  if (se->error != OK)
	r = se->error;
  else
	r = copy_fdsets(se, se->nfds, TO_PROC);
  if (r == OK)
	r = se->nreadyfds;

  revive(se->req_endpt, r);
}


/*===========================================================================*
 *				select_callback			             *
 *===========================================================================*/
void select_callback(struct filp *f, int status)
{
/* The status of a filp has changed, with the given ready operations or error.
 * This function is currently called only for pipes, and holds the lock to
 * the filp.
 */

  filp_status(f, status);
}

/*===========================================================================*
 *				init_select  				     *
 *===========================================================================*/
void init_select(void)
{
  int s;

  for (s = 0; s < MAXSELECTS; s++)
	init_timer(&selecttab[s].timer);
}


/*===========================================================================*
 *				select_forget			             *
 *===========================================================================*/
void select_forget(void)
{
/* The calling thread's associated process is expected to be unpaused, due to
 * a signal that is supposed to interrupt the current system call. Totally
 * forget about the select(). This function may block its calling thread if
 * necessary (but it doesn't).
 */
  int slot;
  struct selectentry *se;

  for (slot = 0; slot < MAXSELECTS; slot++) {
	se = &selecttab[slot];
	if (se->requestor == fp)
		break;
  }

  if (slot >= MAXSELECTS) return;	/* Entry not found */

  assert(se->starting == FALSE);

  /* Do NOT test on is_deferred here. We can safely cancel ongoing queries. */
  select_cancel_all(se);
}


/*===========================================================================*
 *				select_timeout_check	  	     	     *
 *===========================================================================*/
void select_timeout_check(int s)
{
/* An alarm has gone off for one of the select queries. This function MUST NOT
 * block its calling thread.
 */
  struct selectentry *se;

  if (s < 0 || s >= MAXSELECTS) return;	/* Entry does not exist */

  se = &selecttab[s];
  if (se->requestor == NULL) return;
  if (se->expiry == 0) return;	/* Strange, did we even ask for a timeout? */
  se->expiry = 0;
  if (!is_deferred(se))
	select_return(se);
  else
	se->block = 0;	/* timer triggered "too soon", treat as nonblocking */
}


/*===========================================================================*
 *				select_unsuspend_by_endpt  	     	     *
 *===========================================================================*/
void select_unsuspend_by_endpt(endpoint_t proc_e)
{
/* Revive blocked processes when a driver has disappeared */
  struct dmap *dp;
  struct smap *sp;
  devmajor_t major;
  int fd, s, is_driver, restart;
  struct selectentry *se;
  struct filp *f;

  /* Either or both of these may be NULL. */
  dp = get_dmap_by_endpt(proc_e);
  sp = get_smap_by_endpt(proc_e);

  is_driver = (dp != NULL || sp != NULL);

  for (s = 0; s < MAXSELECTS; s++) {
	se = &selecttab[s];
	if (se->requestor == NULL) continue;
	if (se->requestor->fp_endpoint == proc_e) {
		assert(se->requestor->fp_flags & FP_EXITING);
		select_cancel_all(se);
		continue;
	}

	/* Skip the more expensive "driver died" checks for non-drivers. */
	if (!is_driver)
		continue;

	restart = FALSE;

	for (fd = 0; fd < se->nfds; fd++) {
		if ((f = se->filps[fd]) == NULL)
			continue;
		if (is_char_device(f)) {
			assert(f->filp_select_dev != NO_DEV);
			major = major(f->filp_select_dev);
			if (dmap_driver_match(proc_e, major)) {
				ops2tab(SEL_RD | SEL_WR, fd, se);
				se->filps[fd] = NULL;
				select_cancel_filp(f);
				restart = TRUE;
			}
		} else if (sp != NULL && is_sock_device(f)) {
			assert(f->filp_select_dev != NO_DEV);
			if (get_smap_by_dev(f->filp_select_dev, NULL) == sp) {
				ops2tab(SEL_RD | SEL_WR, fd, se);
				se->filps[fd] = NULL;
				select_cancel_filp(f);
				restart = TRUE;
			}
		}
	}

	if (restart)
		restart_proc(se);
  }

  /* Any outstanding queries will never be answered, so forget about them. */
  if (dp != NULL) {
	assert(dp->dmap_sel_filp == NULL);
	dp->dmap_sel_busy = FALSE;
  }
  if (sp != NULL) {
	assert(sp->smap_sel_filp == NULL);
	sp->smap_sel_busy = FALSE;
  }
}

/*===========================================================================*
 *				select_reply1				     *
 *===========================================================================*/
static void select_reply1(struct filp *f, int status)
{
/* Handle the initial reply to a character or socket select request.  This
 * function MUST NOT block its calling thread.
 */

  assert(f->filp_count >= 1);
  assert(f->filp_select_flags & FSF_BUSY);

  f->filp_select_flags &= ~FSF_BUSY;

  /* The select call is done now, except when
   * - another process started a select on the same filp with possibly a
   *   different set of operations.
   * - a process does a select on the same filp but using different file
   *   descriptors.
   * - the select has a timeout. Upon receiving this reply the operations
   *   might not be ready yet, so we want to wait for that to ultimately
   *   happen.
   *   Therefore we need to keep remembering what the operations are.
   */
  if (!(f->filp_select_flags & (FSF_UPDATE|FSF_BLOCKED)))
	f->filp_select_ops = 0;		/* done selecting */
  else if (status > 0 && !(f->filp_select_flags & FSF_UPDATE))
	/* there may be operations pending */
	f->filp_select_ops &= ~status;

  /* Record new filp status */
  if (!(status == 0 && (f->filp_select_flags & FSF_BLOCKED))) {
	if (status > 0) {	/* operations ready */
		if (status & SEL_RD)
			f->filp_select_flags &= ~FSF_RD_BLOCK;
		if (status & SEL_WR)
			f->filp_select_flags &= ~FSF_WR_BLOCK;
		if (status & SEL_ERR)
			f->filp_select_flags &= ~FSF_ERR_BLOCK;
	} else if (status < 0) { /* error */
		/* Always unblock upon error */
		f->filp_select_flags &= ~FSF_BLOCKED;
	}
  }

  filp_status(f, status); /* Tell filp owners about the results */
}

/*===========================================================================*
 *				select_cdev_reply1			     *
 *===========================================================================*/
void select_cdev_reply1(endpoint_t driver_e, devminor_t minor, int status)
{
/* Handle the initial reply to a CDEV_SELECT request. This function MUST NOT
 * block its calling thread.
 */
  devmajor_t major;
  dev_t dev;
  struct filp *f;
  struct dmap *dp;

  /* Figure out which device is replying */
  if ((dp = get_dmap_by_endpt(driver_e)) == NULL) return;

  major = dp-dmap;
  dev = makedev(major, minor);

  /* Get filp belonging to character special file */
  if (!dp->dmap_sel_busy) {
	printf("VFS (%s:%d): major %d was not expecting a CDEV_SELECT reply\n",
		__FILE__, __LINE__, major);
	return;
  }

  /* The select filp may have been set to NULL if the requestor has been
   * unpaused in the meantime. In that case, we ignore the result, but we do
   * look for other filps to restart later.
   */
  if ((f = dp->dmap_sel_filp) != NULL) {
	/* Find vnode and check we got a reply from the device we expected */
	assert(is_char_device(f));
	assert(f->filp_select_dev != NO_DEV);
	if (f->filp_select_dev != dev) {
		/* This should never happen. The driver may be misbehaving.
		 * For now we assume that the reply we want will arrive later..
		 */
		printf("VFS (%s:%d): expected reply from dev %llx not %llx\n",
			__FILE__, __LINE__, f->filp_select_dev, dev);
		return;
	}
  }

  /* No longer waiting for a reply from this device */
  dp->dmap_sel_busy = FALSE;
  dp->dmap_sel_filp = NULL;

  /* Process the status change, if still applicable. */
  if (f != NULL)
	select_reply1(f, status);

  /* See if we should send a select request for another filp now. */
  select_restart_filps();
}

/*===========================================================================*
 *				select_sdev_reply1			     *
 *===========================================================================*/
void select_sdev_reply1(dev_t dev, int status)
{
/* Handle the initial reply to a SDEV_SELECT request. This function MUST NOT
 * block its calling thread.
 */
  struct smap *sp;
  struct filp *f;

  if ((sp = get_smap_by_dev(dev, NULL)) == NULL)
	return;

  /* Get the file pointer for the socket device. */
  if (!sp->smap_sel_busy) {
	printf("VFS: was not expecting a SDEV_SELECT reply from %d\n",
	    sp->smap_endpt);
	return;
  }

  /* The select filp may have been set to NULL if the requestor has been
   * unpaused in the meantime. In that case, we ignore the result, but we do
   * look for other filps to restart later.
   */
  if ((f = sp->smap_sel_filp) != NULL) {
	/* Find vnode and check we got a reply from the device we expected */
	assert(is_sock_device(f));
	assert(f->filp_select_dev != NO_DEV);
	if (f->filp_select_dev != dev) {
		/* This should never happen. The driver may be misbehaving.
		 * For now we assume that the reply we want will arrive later..
		 */
		printf("VFS: expected reply from sock dev %llx, not %llx\n",
		    f->filp_select_dev, dev);
		return;
	}
  }

  /* We are no longer waiting for a reply from this socket driver. */
  sp->smap_sel_busy = FALSE;
  sp->smap_sel_filp = NULL;

  /* Process the status change, if still applicable. */
  if (f != NULL)
	select_reply1(f, status);

  /* See if we should send a select request for another filp now. */
  select_restart_filps();
}

/*===========================================================================*
 *				select_reply2				     *
 *===========================================================================*/
static void select_reply2(int is_char, dev_t dev, int status)
{
/* Find all file descriptors selecting for the given character (is_char==TRUE)
 * or socket (is_char==FALSE) device, update their statuses, and resume
 * activities accordingly.
 */
  int slot, found, fd;
  struct filp *f;
  struct selectentry *se;

  for (slot = 0; slot < MAXSELECTS; slot++) {
	se = &selecttab[slot];
	if (se->requestor == NULL) continue;	/* empty slot */

	found = FALSE;
	for (fd = 0; fd < se->nfds; fd++) {
		if ((f = se->filps[fd]) == NULL) continue;
		if (is_char && !is_char_device(f)) continue;
		if (!is_char && !is_sock_device(f)) continue;
		assert(f->filp_select_dev != NO_DEV);
		if (f->filp_select_dev != dev) continue;

		if (status > 0) {	/* Operations ready */
			/* Clear the replied bits from the request
			 * mask unless FSF_UPDATE is set.
			 */
			if (!(f->filp_select_flags & FSF_UPDATE))
				f->filp_select_ops &= ~status;
			if (status & SEL_RD)
				f->filp_select_flags &= ~FSF_RD_BLOCK;
			if (status & SEL_WR)
				f->filp_select_flags &= ~FSF_WR_BLOCK;
			if (status & SEL_ERR)
				f->filp_select_flags &= ~FSF_ERR_BLOCK;

			ops2tab(status, fd, se);
		} else {
			f->filp_select_flags &= ~FSF_BLOCKED;
			se->error = status;
		}
		found = TRUE;
	}
	/* Even if 'found' is set now, nothing may have changed for this call,
	 * as it may not have been interested in the operations that were
	 * reported as ready. Let restart_proc check.
	 */
	if (found)
		restart_proc(se);
  }

  select_restart_filps();
}

/*===========================================================================*
 *				select_cdev_reply2			     *
 *===========================================================================*/
void select_cdev_reply2(endpoint_t driver_e, devminor_t minor, int status)
{
/* Handle a secondary reply to a CDEV_SELECT request. A secondary reply occurs
 * when the select request is 'blocking' until an operation becomes ready. This
 * function MUST NOT block its calling thread.
 */
  devmajor_t major;
  struct dmap *dp;
  dev_t dev;

  if (status == 0) {
	printf("VFS (%s:%d): weird status (%d) to report\n",
		__FILE__, __LINE__, status);
	return;
  }

  /* Figure out which device is replying */
  if ((dp = get_dmap_by_endpt(driver_e)) == NULL) {
	printf("VFS (%s:%d): endpoint %d is not a known driver endpoint\n",
		__FILE__, __LINE__, driver_e);
	return;
  }
  major = dp-dmap;
  dev = makedev(major, minor);

  select_reply2(TRUE /*is_char*/, dev, status);
}

/*===========================================================================*
 *				select_sdev_reply2			     *
 *===========================================================================*/
void select_sdev_reply2(dev_t dev, int status)
{
/* Handle a secondary reply to a SDEV_SELECT request. A secondary reply occurs
 * when the select request is 'blocking' until an operation becomes ready. This
 * function MUST NOT block its calling thread.
 */

  if (status == 0) {
	printf("VFS: weird socket device status (%d)\n", status);

	return;
  }

  select_reply2(FALSE /*is_char*/, dev, status);
}

/*===========================================================================*
 *				select_restart_filps			     *
 *===========================================================================*/
static void select_restart_filps(void)
{
/* We got a result from a character driver, and now we need to check if we can
 * restart deferred polling operations. This function MUST NOT block its
 * calling thread.
 */
  int fd, slot;
  struct filp *f;
  struct selectentry *se;

  /* Locate filps that can be restarted */
  for (slot = 0; slot < MAXSELECTS; slot++) {
	se = &selecttab[slot];
	if (se->requestor == NULL) continue; /* empty slot */

	/* Only 'deferred' processes are eligible to restart */
	if (!is_deferred(se)) continue;

	/* Find filps that are not waiting for a reply, but have an updated
	 * status (i.e., another select on the same filp with possibly a
	 * different set of operations is to be done), and thus requires the
	 * select request to be sent again).
	 */
	for (fd = 0; fd < se->nfds; fd++) {
		int r, wantops, ops;
		if ((f = se->filps[fd]) == NULL) continue;
		if (f->filp_select_flags & FSF_BUSY) /* Still waiting for */
			continue;		     /* initial reply */
		if (!(f->filp_select_flags & FSF_UPDATE)) /* Must be in  */
			continue;			  /* 'update' state */

		/* This function is suitable only for character and socket
		 * devices. In particular, checking pipes the same way would
		 * introduce a serious locking problem.
		 */
		assert(is_char_device(f) || is_sock_device(f));

		wantops = ops = f->filp_select_ops;
		if (is_char_device(f))
			r = select_request_char(f, &wantops, se->block,
			    se->requestor);
		else
			r = select_request_sock(f, &wantops, se->block,
			    se->requestor);
		if (r != OK && r != SUSPEND) {
			se->error = r;
			restart_proc(se);
			break; /* Error or bogus return code; abort */
		}
		if (wantops & ops) ops2tab(wantops, fd, se);
	}
  }
}

/*===========================================================================*
 *				filp_status				     *
 *===========================================================================*/
static void
filp_status(struct filp *f, int status)
{
/* Tell processes that need to know about the status of this filp. This
 * function MUST NOT block its calling thread.
 */
  int fd, slot, found;
  struct selectentry *se;

  for (slot = 0; slot < MAXSELECTS; slot++) {
	se = &selecttab[slot];
	if (se->requestor == NULL) continue; /* empty slot */

	found = FALSE;
	for (fd = 0; fd < se->nfds; fd++) {
		if (se->filps[fd] != f) continue;
		if (status < 0)
			se->error = status;
		else
			ops2tab(status, fd, se);
		found = TRUE;
	}
	if (found)
		restart_proc(se);
  }
}

/*===========================================================================*
 *				restart_proc				     *
 *===========================================================================*/
static void
restart_proc(struct selectentry *se)
{
/* Tell process about select results (if any) unless there are still results
 * pending. This function MUST NOT block its calling thread.
 */

  if ((se->nreadyfds > 0 || se->error != OK || !se->block) && !is_deferred(se))
	select_return(se);
}

/*===========================================================================*
 *				wipe_select				     *
 *===========================================================================*/
static void wipe_select(struct selectentry *se)
{
  se->nfds = 0;
  se->nreadyfds = 0;
  se->error = OK;
  se->block = 0;
  memset(se->filps, 0, sizeof(se->filps));

  FD_ZERO(&se->readfds);
  FD_ZERO(&se->writefds);
  FD_ZERO(&se->errorfds);
  FD_ZERO(&se->ready_readfds);
  FD_ZERO(&se->ready_writefds);
  FD_ZERO(&se->ready_errorfds);
}

/*===========================================================================*
 *				select_lock_filp			     *
 *===========================================================================*/
static void select_lock_filp(struct filp *f, int ops)
{
/* Lock a filp and vnode based on which operations are requested. This function
 * may block its calling thread, obviously.
 */
  tll_access_t locktype;

  locktype = VNODE_READ; /* By default */

  if (ops & (SEL_WR|SEL_ERR))
	/* Selecting for error or writing requires exclusive access */
	locktype = VNODE_WRITE;

  lock_filp(f, locktype);
}

/*
 * Dump the state of the entire select table, for debugging purposes.
 */
void
select_dump(void)
{
	struct selectentry *se;
	struct filp *f;
	struct dmap *dp;
	struct smap *sp;
	dev_t dev;
	sockid_t sockid;
	int s, fd;

	for (s = 0; s < MAXSELECTS; s++) {
		se = &selecttab[s];
		if (se->requestor == NULL)
			continue;

		printf("select %d: endpt %d nfds %d nreadyfds %d error %d "
		    "block %d starting %d expiry %u is_deferred %d\n",
		    s, se->req_endpt, se->nfds, se->nreadyfds, se->error,
		    se->block, se->starting, se->expiry, is_deferred(se));

		for (fd = 0; !se->starting && fd < se->nfds; fd++) {
			/* Save on output: do not print NULL filps at all. */
			if ((f = se->filps[fd]) == NULL)
				continue;

			printf("- [%d] filp %p flags %x type ", fd, f,
			    f->filp_select_flags);
			if (is_regular_file(f))
				printf("regular\n");
			else if (is_pipe(f))
				printf("pipe\n");
			else if (is_char_device(f)) {
				dev = cdev_map(f->filp_vno->v_sdev,
				    se->requestor);
				printf("char (dev <%d,%d>, dmap ",
				    major(dev), minor(dev));
				if (dev != NO_DEV) {
					dp = &dmap[major(dev)];
					printf("busy %d filp %p)\n",
					    dp->dmap_sel_busy,
					    dp->dmap_sel_filp);
				} else
					printf("unknown)\n");
			} else if (is_sock_device(f)) {
				dev = f->filp_vno->v_sdev;
				printf("sock (dev ");
				sp = get_smap_by_dev(dev, &sockid);
				if (sp != NULL) {
					printf("<%d,%d>, smap busy %d filp "
					    "%p)\n", sp->smap_num, sockid,
					    sp->smap_sel_busy,
					    sp->smap_sel_filp);
				} else
					printf("<0x%"PRIx64">, smap "
					    "unknown)\n", dev);
			} else
				printf("unknown\n");
		}
	}
}
