/*
   Copyright (c) 2007, 2008 Z RESEARCH, Inc. <http://www.zresearch.com>
   This file is part of GlusterFS.

   GlusterFS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3 of the License,
   or (at your option) any later version.

   GlusterFS is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.
*/

#include <libgen.h>
#include <unistd.h>
#include <fnmatch.h>
#include <sys/time.h>
#include <stdlib.h>
#include <signal.h>

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "glusterfs.h"
#include "afr.h"
#include "dict.h"
#include "xlator.h"
#include "hashfn.h"
#include "logging.h"
#include "stack.h"
#include "list.h"
#include "call-stub.h"
#include "defaults.h"
#include "common-utils.h"
#include "compat-errno.h"
#include "compat.h"
#include "byte-order.h"

#include "inode-read.h"
#include "inode-write.h"
#include "dir-read.h"
#include "dir-write.h"
#include "transaction.h"

void
loc_wipe (loc_t *loc)
{
  if (!loc)
    return;

  if (loc->path) 
	  FREE (loc->path);

  if (loc->inode)
    inode_unref (loc->inode);
}

/*
void
loc_copy (loc_t *dest, loc_t *src)
{
  if (!dest || !src)
    return;

  dest->path = strdup (src->path);
  dest->ino = src->ino;
  if (src->inode)
    dest->inode = inode_ref (src->inode);
}
*/

/**
 * first_up_child - return the index of the first child that is up
 */

int
first_up_child (afr_private_t *priv)
{
	xlator_t ** children = NULL;
	int         ret      = -1;
	int         i        = 0;

	LOCK (&priv->lock);
	{
		children = priv->children;
		for (i = 0; i < priv->child_count; i++) {
			if (priv->child_up[i]) {
				ret = i;
				break;
			}
		}
	}
	UNLOCK (&priv->lock);

	return ret;
}


/**
 * up_children_count - return the number of children that are up
 */

int
up_children_count (int child_count, unsigned char *child_up)
{
	int i   = 0;
	int ret = 0;

	for (i = 0; i < child_count; i++)
		if (child_up[i])
			ret++;
	return ret;
}


static ino64_t
itransform (ino64_t ino, int child_count, int child_index)
{
	ino64_t scaled_ino = -1;

	if (ino == ((uint64_t) -1)) {
		scaled_ino = ((uint64_t) -1);
		goto out;
	}

	scaled_ino = (ino * child_count) + child_index;

out:
	return scaled_ino;
}


static int
deitransform (ino64_t ino, int child_count)
{
	int index = -1;

	index = ino % child_count;

	return index;
}


int32_t
afr_lookup_cbk (call_frame_t *frame, void *cookie,
		xlator_t *this,	int32_t op_ret,	int32_t op_errno,
		inode_t *inode,	struct stat *buf, dict_t *xattr)
{
	afr_local_t *   local = NULL;
	afr_private_t * priv  = NULL;

	int call_count = -1;
	
	int child_index = (int) cookie;

	priv = this->private;

	LOCK (&frame->lock);
	{
		local = frame->local;
		call_count = --local->call_count;

		if ((op_ret == 0) && !local->success_count) {
			local->cont.lookup.inode = inode;
			local->cont.lookup.buf   = *buf;

			local->cont.lookup.buf.st_ino = itransform (buf->st_ino, 
								    priv->child_count, 
								    child_index);
			gf_log (this->name, GF_LOG_TRACE,
				"scaling inode %"PRId64" to %"PRId64,
				buf->st_ino, local->cont.lookup.buf.st_ino);

			local->cont.lookup.xattr = xattr;

			local->success_count = 1;
		}
	}
	UNLOCK (&frame->lock);

	if (call_count == 0)
		STACK_UNWIND (frame, op_ret, op_errno, inode, 
			      &local->cont.lookup.buf, xattr);

	return 0;
}


int32_t 
afr_lookup (call_frame_t *frame, xlator_t *this,
	    loc_t *loc, int32_t need_xattr)
{
	afr_private_t *priv = NULL;
	afr_local_t *local = NULL;

	int i = 0;
	int32_t op_errno = 0;

	int child_index = -1;

	priv = this->private;

	ALLOC_OR_GOTO (local, afr_local_t, out);

	frame->local = local;

	if (loc->inode->ino != 0) {
		/* revalidate */

		child_index = deitransform (loc->inode->ino, priv->child_count);

		gf_log (this->name, GF_LOG_TRACE,
			"revalidate on node %d",
			child_index);

		local->call_count = 1;

		STACK_WIND_COOKIE (frame, afr_lookup_cbk, (void *) i,
				   priv->children[child_index],
				   priv->children[child_index]->fops->lookup,
				   loc, need_xattr);
	} else {
		/* fresh lookup */

		local->call_count = priv->child_count;

		for (i = 0; i < priv->child_count; i++) {
			STACK_WIND_COOKIE (frame, afr_lookup_cbk, (void *) i,
					   priv->children[i],
					   priv->children[i]->fops->lookup,
					   loc, need_xattr);
		}
	}

out:
	return 0;
}


/* {{{ open */

int32_t
afr_open_cbk (call_frame_t *frame, void *cookie,
	      xlator_t *this, int32_t op_ret, int32_t op_errno,
	      fd_t *fd)
{
	afr_local_t *local = NULL;
	
	int call_count = -1;

	local = frame->local;

	LOCK (&frame->lock);
	{
		call_count = --local->call_count;
	}
	UNLOCK (&frame->lock);

	if (call_count == 0)
		STACK_UNWIND (frame, 0, op_errno, fd);

	return 0;
}


int32_t afr_open (call_frame_t *frame, xlator_t *this,
		  loc_t *loc, int32_t flags, fd_t *fd)
{
	afr_private_t *priv = NULL;
	afr_local_t *local = NULL;

	int i = 0;
	int32_t op_errno = 0;

	priv = this->private;

	ALLOC_OR_GOTO (local, afr_local_t, out);

	local->call_count = priv->child_count;
	frame->local = local;

	for (i = 0; i < priv->child_count; i++) {
		STACK_WIND (frame, afr_open_cbk,
			    priv->children[i],
			    priv->children[i]->fops->open,
			    loc, flags, fd);
	}

out:
	return 0;
}

/* }}} */


/* {{{ flush */

int32_t
afr_flush_cbk (call_frame_t *frame, void *cookie,
	       xlator_t *this, int32_t op_ret, int32_t op_errno)
{
	afr_local_t *local = NULL;
	
	int call_count = -1;

	local = frame->local;

	LOCK (&frame->lock);
	{
		call_count = --local->call_count;
	}
	UNLOCK (&frame->lock);

	if (call_count == 0)
		STACK_UNWIND (frame, 0, op_errno);

	return 0;
}


int32_t afr_flush (call_frame_t *frame, xlator_t *this,
		   fd_t *fd)
{
	afr_private_t *priv = NULL;
	afr_local_t *local = NULL;

	int i = 0;
	int32_t op_errno = 0;

	priv = this->private;

	ALLOC_OR_GOTO (local, afr_local_t, out);

	local->call_count = priv->child_count;
	frame->local = local;

	for (i = 0; i < priv->child_count; i++) {
		STACK_WIND (frame, afr_flush_cbk,
			    priv->children[i],
			    priv->children[i]->fops->flush,
			    fd);
	}

out:
	return 0;
}

/* }}} */

int32_t
afr_statfs_cbk (call_frame_t *frame, void *cookie,
		xlator_t *this, int32_t op_ret, int32_t op_errno,
		struct statvfs *statvfs)
{
	afr_local_t *local = NULL;

	LOCK (&frame->lock);
	{
		local = frame->local;
		local->call_count--;

		if (local->cont.statfs.buf_set) {
			if (statvfs->f_bavail < local->cont.statfs.buf.f_bavail)
				local->cont.statfs.buf = *statvfs;
		} else {
			local->cont.statfs.buf = *statvfs;
			local->cont.statfs.buf_set = 1;
		}
	}
	UNLOCK (&frame->lock);


	if (local->call_count == 0)
		STACK_UNWIND (frame, op_ret, op_errno, &local->cont.statfs.buf);

	return 0;
}


int32_t
afr_statfs (call_frame_t *frame, xlator_t *this,
	    loc_t *loc)
{
	afr_private_t *  priv        = NULL;
	int              child_count = 0;
	afr_local_t   *  local       = NULL;
	int              i           = 0;

	int32_t          op_ret      = -1;
	int32_t          op_errno    = 0;

	VALIDATE_OR_GOTO (this, out);
	VALIDATE_OR_GOTO (this->private, out);
	VALIDATE_OR_GOTO (loc, out);

	priv = this->private;
	child_count = priv->child_count;

	ALLOC_OR_GOTO (local, afr_local_t, out);

	local->call_count = child_count;

	frame->local = local;

	for (i = 0; i < child_count; i++) {
		STACK_WIND (frame, afr_statfs_cbk,
			    priv->children[i],
			    priv->children[i]->fops->statfs, 
			    loc);
	}
	
	op_ret = 0;
out:
	if (op_ret == -1) {
		STACK_UNWIND (frame, op_ret, op_errno, NULL);
	}
	return 0;
}


/**
 * find_child_index - find the child's index in the array of subvolumes
 * @this: AFR
 * @child: child
 */

static int
find_child_index (xlator_t *this, xlator_t *child)
{
	afr_private_t *priv = NULL;

	int i = -1;

	priv = this->private;

	for (i = 0; i < priv->child_count; i++) {
		if ((xlator_t *) child == priv->children[i])
			break;
	}

	return i;
}


static int32_t
afr_test_gf_lk_cbk (call_frame_t *frame, void *cookie,
		    xlator_t *this, int32_t op_ret, int32_t op_errno)
{
	afr_private_t * priv  = NULL;
	int             i     = -1;
	xlator_t      * child = (xlator_t *) cookie;

	priv = this->private;

	if (op_errno == ENOSYS) {
		gf_log (this->name, GF_LOG_CRITICAL,
			"'locks' translator has not been loaded on server corresponding to subvolume '%s', neglecting the subvolume. Data WILL NOT be replicated on it.",
			child->name);
	} else {
		gf_log (this->name, GF_LOG_DEBUG,
			"'%s' supports GF_*_LK", child->name);

		i = find_child_index (this, child);
		priv->child_up[i] = 1;
	}

	STACK_DESTROY (frame->root);

	return 0;
}


static void
afr_test_gf_lk (xlator_t *this, xlator_t *child)
{
	call_ctx_t *  cctx = NULL;
	call_pool_t * pool = this->ctx->pool;
	loc_t         loc  = {
		.inode = NULL,
		.path = "/",
	};

	struct        flock lock = {
		.l_start = -1,
		.l_len   = -1,
	};

	cctx = calloc (1, sizeof (*cctx));

	cctx->frames.root  = cctx;
	cctx->frames.this  = this;    
	cctx->pool         = pool;

	LOCK (&pool->lock);
	{
		list_add (&cctx->all_frames, &pool->all_frames);
	}
	UNLOCK (&pool->lock);
  
	STACK_WIND_COOKIE ((&cctx->frames), 
			   afr_test_gf_lk_cbk,
			   child, child,
			   child->fops->gf_file_lk,
			   &loc, F_SETLK, &lock);
	return;
}


int32_t
notify (xlator_t *this, int32_t event,
	void *data, ...)
{
	afr_private_t *     priv     = NULL;
	unsigned char *     child_up = NULL;

	int i           = -1;
	int up_children = 0;

	priv  = this->private;

	child_up = priv->child_up;

	switch (event) {
	case GF_EVENT_CHILD_UP:
		i = find_child_index (this, data);

		afr_test_gf_lk (this, data);

		/* 
		   if all the children were down, and one child came up, 
		   send notify to parent
		*/

		for (i = 0; i < priv->child_count; i++)
			if (child_up[i])
				up_children++;

		if (up_children == 1)
			default_notify (this, event, data);

		break;

	case GF_EVENT_CHILD_DOWN:
		i = find_child_index (this, data);

		child_up[i] = 0;
		
		/* 
		   if all children are down, and this was the last to go down,
		   send notify to parent
		*/

		for (i = 0; i < priv->child_count; i++)
			if (child_up[i])
				up_children++;

		if (up_children == 0)
			default_notify (this, event, data);

		break;

	case GF_EVENT_PARENT_UP:
		for (i = 0; i < priv->child_count; i++)
			priv->children[i]->notify (priv->children[i], 
						   GF_EVENT_PARENT_UP, this);
		break;
		
	default:
		default_notify (this, event, data);
	}

	return 0;
}


int32_t 
init (xlator_t *this)
{
	afr_private_t * priv        = NULL;
	int             child_count = 0;
	xlator_list_t * trav        = NULL;
	int             i           = 0;
	int             ret         = -1;
	int             op_errno    = 0;

	char * read_subvol = NULL;
	int    dict_ret    = -1;

	ALLOC_OR_GOTO (this->private, afr_private_t, out);

	priv = this->private;

	dict_ret = dict_get_str (this->options, "read-subvolume", &read_subvol);
	priv->read_child = -1;

	trav = this->children;
	while (trav) {
		if (dict_ret == 0 && !strcmp (read_subvol, trav->xlator->name)) {
			priv->read_child = child_count;
		}

		child_count++;
		trav = trav->next;
	}

	priv->child_count = child_count;
	LOCK_INIT (&priv->lock);

	priv->child_up = calloc (sizeof (unsigned char), child_count);
	if (!priv->child_up) {
		gf_log (this->name, GF_LOG_ERROR,	
			"out of memory :(");		
		op_errno = ENOMEM;			
		goto out;
	}

	priv->children = calloc (sizeof (xlator_t *), child_count);
	if (!priv->children) {
		gf_log (this->name, GF_LOG_ERROR,	
			"out of memory :(");		
		op_errno = ENOMEM;			
		goto out;
	}

	priv->pending_inc_array = calloc (sizeof (int32_t), child_count);
	if (!priv->pending_inc_array) {
		gf_log (this->name, GF_LOG_ERROR,	
			"out of memory :(");		
		op_errno = ENOMEM;			
		goto out;
	}

	priv->pending_dec_array = calloc (sizeof (int32_t), child_count);
	if (!priv->pending_dec_array) {
		gf_log (this->name, GF_LOG_ERROR,	
			"out of memory :(");		
		op_errno = ENOMEM;			
		goto out;
	}

	trav = this->children;
	while (i < child_count) {
		priv->children[i] = trav->xlator;
		priv->pending_inc_array[i] = hton32 (1);
		priv->pending_dec_array[i] = hton32 (-1);

		trav = trav->next;
		i++;
	}

	ret = 0;
out:
	return ret;
}


int32_t
fini (xlator_t *this)
{
	return 0;
}


struct xlator_fops fops = {
  .lookup      = afr_lookup,
  .open        = afr_open,

  /* inode read */
  .access      = afr_access,
  .stat        = afr_stat,
  .fstat       = afr_fstat,
  .readlink    = afr_readlink,
  .getxattr    = afr_getxattr,
  .readv       = afr_readv,

  /* inode write */
  .chmod       = afr_chmod,
  .chown       = afr_chown,
  .writev      = afr_writev,
  .truncate    = afr_truncate,
  .utimens     = afr_utimens,

  /* dir read */
  .opendir     = afr_opendir,
/*  .closedir    = afr_closedir,  no need to implement this if I don't store ctx */
  .readdir     = afr_readdir,
  .statfs      = afr_statfs,

  /* dir write */
  .create      = afr_create,
  .mknod       = afr_mknod,
  .mkdir       = afr_mkdir,
  .unlink      = afr_unlink,
  .rmdir       = afr_rmdir,
  .link        = afr_link,
  .symlink     = afr_symlink,
  .rename      = afr_rename,
};


struct xlator_mops mops = {
};


struct xlator_cbks cbks = {
};
