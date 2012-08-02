/*
 * fs/dcache.c
 *
 * Complete reimplementation
 * (C) 1997 Thomas Schoebel-Theuer,
 * with heavy changes by Linus Torvalds
 */

/*
 * Notes on the allocation strategy:
 *
 * The dcache is a master of the icache - whenever a dcache entry
 * exists, the inode will always exist. "iput()" is done either when
 * the dcache entry is deleted or garbage collected.
 */

#include <linux/syscalls.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/cache.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <asm/uaccess.h>
#include <linux/security.h>
#include <linux/seqlock.h>
#include <linux/swap.h>
#include <linux/bootmem.h>
#include <linux/fs_struct.h>
#include <linux/hardirq.h>
#include <linux/bit_spinlock.h>
#include <linux/rculist_bl.h>
#include <linux/prefetch.h>
#include "internal.h"

/*
 * Usage:
 * dcache->d_inode->i_lock protects:
 *   - i_dentry, d_alias, d_inode of aliases
 * dcache_hash_bucket lock protects:
 *   - the dcache hash table
 * s_anon bl list spinlock protects:
 *   - the s_anon list (see __d_drop)
 * dcache_lru_lock protects:
 *   - the dcache lru lists and counters
 * d_lock protects:
 *   - d_flags
 *   - d_name
 *   - d_lru
 *   - d_count
 *   - d_unhashed()
 *   - d_parent and d_subdirs
 *   - childrens' d_child and d_parent
 *   - d_alias, d_inode
 *
 * Ordering:
 * dentry->d_inode->i_lock
 *   dentry->d_lock
 *     dcache_lru_lock
 *     dcache_hash_bucket lock
 *     s_anon lock
 *
 * If there is an ancestor relationship:
 * dentry->d_parent->...->d_parent->d_lock
 *   ...
 *     dentry->d_parent->d_lock
 *       dentry->d_lock
 *
 * If no ancestor relationship:
 * if (dentry1 < dentry2)
 *   dentry1->d_lock
 *     dentry2->d_lock
 */
int sysctl_vfs_cache_pressure __read_mostly = 50;
EXPORT_SYMBOL_GPL(sysctl_vfs_cache_pressure);

static __cacheline_aligned_in_smp DEFINE_SPINLOCK(dcache_lru_lock);
__cacheline_aligned_in_smp DEFINE_SEQLOCK(rename_lock);

EXPORT_SYMBOL(rename_lock);

static struct kmem_cache *dentry_cache __read_mostly;

/*
 * This is the single most critical data structure when it comes
 * to the dcache: the hashtable for lookups. Somebody should try
 * to make this good - I've just made it work.
 *
 * This hash-function tries to avoid losing too many bits of hash
 * information, yet avoid using a prime hash-size or similar.
 */
#define D_HASHBITS     d_hash_shift
#define D_HASHMASK     d_hash_mask

static unsigned int d_hash_mask __read_mostly;
static unsigned int d_hash_shift __read_mostly;

static struct hlist_bl_head *dentry_hashtable __read_mostly;

static inline struct hlist_bl_head *d_hash(struct dentry *parent,
					unsigned long hash)
{
	hash += ((unsigned long) parent ^ GOLDEN_RATIO_PRIME) / L1_CACHE_BYTES;
	hash = hash ^ ((hash ^ GOLDEN_RATIO_PRIME) >> D_HASHBITS);
	return dentry_hashtable + (hash & D_HASHMASK);
}

/* Statistics gathering. */
struct dentry_stat_t dentry_stat = {
	.age_limit = 45,
};

static DEFINE_PER_CPU(unsigned int, nr_dentry);

#if defined(CONFIG_SYSCTL) && defined(CONFIG_PROC_FS)
static int get_nr_dentry(void)
{
	int i;
	int sum = 0;
	for_each_possible_cpu(i)
		sum += per_cpu(nr_dentry, i);
	return sum < 0 ? 0 : sum;
}

int proc_nr_dentry(ctl_table *table, int write, void __user *buffer,
		   size_t *lenp, loff_t *ppos)
{
	dentry_stat.nr_dentry = get_nr_dentry();
	return proc_dointvec(table, write, buffer, lenp, ppos);
}
#endif

static void __d_free(struct rcu_head *head)
{
	struct dentry *dentry = container_of(head, struct dentry, d_u.d_rcu);

	WARN_ON(!list_empty(&dentry->d_alias));
	if (dname_external(dentry))
		kfree(dentry->d_name.name);
	kmem_cache_free(dentry_cache, dentry); 
}

/*
 * no locks, please.
 */
static void d_free(struct dentry *dentry)
{
	BUG_ON(dentry->d_count);
	this_cpu_dec(nr_dentry);
	if (dentry->d_op && dentry->d_op->d_release)
		dentry->d_op->d_release(dentry);

	/* if dentry was never visible to RCU, immediate free is OK */
	if (!(dentry->d_flags & DCACHE_RCUACCESS))
		__d_free(&dentry->d_u.d_rcu);
	else
		call_rcu(&dentry->d_u.d_rcu, __d_free);
}

/**
 * dentry_rcuwalk_barrier - invalidate in-progress rcu-walk lookups
 * @dentry: the target dentry
 * After this call, in-progress rcu-walk path lookup will fail. This
 * should be called after unhashing, and after changing d_inode (if
 * the dentry has not already been unhashed).
 */
static inline void dentry_rcuwalk_barrier(struct dentry *dentry)
{
	assert_spin_locked(&dentry->d_lock);
	/* Go through a barrier */
	write_seqcount_barrier(&dentry->d_seq);
}

/*
 * Release the dentry's inode, using the filesystem
 * d_iput() operation if defined. Dentry has no refcount
 * and is unhashed.
 */
static void dentry_iput(struct dentry * dentry)
	__releases(dentry->d_lock)
	__releases(dentry->d_inode->i_lock)
{
	struct inode *inode = dentry->d_inode;
	if (inode) {
		dentry->d_inode = NULL;
		list_del_init(&dentry->d_alias);
		spin_unlock(&dentry->d_lock);
		spin_unlock(&inode->i_lock);
		if (!inode->i_nlink)
			fsnotify_inoderemove(inode);
		if (dentry->d_op && dentry->d_op->d_iput)
			dentry->d_op->d_iput(dentry, inode);
		else
			iput(inode);
	} else {
		spin_unlock(&dentry->d_lock);
	}
}

/*
 * Release the dentry's inode, using the filesystem
 * d_iput() operation if defined. dentry remains in-use.
 */
static void dentry_unlink_inode(struct dentry * dentry)
	__releases(dentry->d_lock)
	__releases(dentry->d_inode->i_lock)
{
	struct inode *inode = dentry->d_inode;
	dentry->d_inode = NULL;
	list_del_init(&dentry->d_alias);
	dentry_rcuwalk_barrier(dentry);
	spin_unlock(&dentry->d_lock);
	spin_unlock(&inode->i_lock);
	if (!inode->i_nlink)
		fsnotify_inoderemove(inode);
	if (dentry->d_op && dentry->d_op->d_iput)
		dentry->d_op->d_iput(dentry, inode);
	else
		iput(inode);
}

/*
 * dentry_lru_(add|del|move_tail) must be called with d_lock held.
 */
static void dentry_lru_add(struct dentry *dentry)
{
	if (list_empty(&dentry->d_lru)) {
		spin_lock(&dcache_lru_lock);
		list_add(&dentry->d_lru, &dentry->d_sb->s_dentry_lru);
		dentry->d_sb->s_nr_dentry_unused++;
		dentry_stat.nr_unused++;
		spin_unlock(&dcache_lru_lock);
	}
}

static void __dentry_lru_del(struct dentry *dentry)
{
	list_del_init(&dentry->d_lru);
	dentry->d_sb->s_nr_dentry_unused--;
	dentry_stat.nr_unused--;
}

static void dentry_lru_del(struct dentry *dentry)
{
	if (!list_empty(&dentry->d_lru)) {
		spin_lock(&dcache_lru_lock);
		__dentry_lru_del(dentry);
		spin_unlock(&dcache_lru_lock);
	}
}

static void dentry_lru_move_tail(struct dentry *dentry)
{
	spin_lock(&dcache_lru_lock);
	if (list_empty(&dentry->d_lru)) {
		list_add_tail(&dentry->d_lru, &dentry->d_sb->s_dentry_lru);
		dentry->d_sb->s_nr_dentry_unused++;
		dentry_stat.nr_unused++;
	} else {
		list_move_tail(&dentry->d_lru, &dentry->d_sb->s_dentry_lru);
	}
	spin_unlock(&dcache_lru_lock);
}

/**
 * d_kill - kill dentry and return parent
 * @dentry: dentry to kill
 * @parent: parent dentry
 *
 * The dentry must already be unhashed and removed from the LRU.
 *
 * If this is the root of the dentry tree, return NULL.
 *
 * dentry->d_lock and parent->d_lock must be held by caller, and are dropped by
 * d_kill.
 */
static struct dentry *d_kill(struct dentry *dentry, struct dentry *parent)
	__releases(dentry->d_lock)
	__releases(parent->d_lock)
	__releases(dentry->d_inode->i_lock)
{
	list_del(&dentry->d_u.d_child);
	/*
	 * Inform try_to_ascend() that we are no longer attached to the
	 * dentry tree
	 */
	dentry->d_flags |= DCACHE_DISCONNECTED;
	if (parent)
		spin_unlock(&parent->d_lock);
	dentry_iput(dentry);
	/*
	 * dentry_iput drops the locks, at which point nobody (except
	 * transient RCU lookups) can reach this dentry.
	 */
	d_free(dentry);
	return parent;
}

/**
 * d_drop - drop a dentry
 * @dentry: dentry to drop
 *
 * d_drop() unhashes the entry from the parent dentry hashes, so that it won't
 * be found through a VFS lookup any more. Note that this is different from
 * deleting the dentry - d_delete will try to mark the dentry negative if
 * possible, giving a successful _negative_ lookup, while d_drop will
 * just make the cache lookup fail.
 *
 * d_drop() is used mainly for stuff that wants to invalidate a dentry for some
 * reason (NFS timeouts or autofs deletes).
 *
 * __d_drop requires dentry->d_lock.
 */
void __d_drop(struct dentry *dentry)
{
	if (!d_unhashed(dentry)) {
		struct hlist_bl_head *b;
		if (unlikely(dentry->d_flags & DCACHE_DISCONNECTED))
			b = &dentry->d_sb->s_anon;
		else
			b = d_hash(dentry->d_parent, dentry->d_name.hash);

		hlist_bl_lock(b);
		__hlist_bl_del(&dentry->d_hash);
		dentry->d_hash.pprev = NULL;
		hlist_bl_unlock(b);

		dentry_rcuwalk_barrier(dentry);
	}
}
EXPORT_SYMBOL(__d_drop);

void d_drop(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	__d_drop(dentry);
	spin_unlock(&dentry->d_lock);
}
EXPORT_SYMBOL(d_drop);

/*
 * Finish off a dentry we've decided to kill.
 * dentry->d_lock must be held, returns with it unlocked.
 * If ref is non-zero, then decrement the refcount too.
 * Returns dentry requiring refcount drop, or NULL if we're done.
 */
static inline struct dentry *dentry_kill(struct dentry *dentry, int ref)
	__releases(dentry->d_lock)
{
	struct inode *inode;
	struct dentry *parent;

	inode = dentry->d_inode;
	if (inode && !spin_trylock(&inode->i_lock)) {
relock:
		spin_unlock(&dentry->d_lock);
		cpu_relax();
		return dentry; /* try again with same dentry */
	}
	if (IS_ROOT(dentry))
		parent = NULL;
	else
		parent = dentry->d_parent;
	if (parent && !spin_trylock(&parent->d_lock)) {
		if (inode)
			spin_unlock(&inode->i_lock);
		goto relock;
	}

	if (ref)
		dentry->d_count--;
	/* if dentry was on the d_lru list delete it from there */
	dentry_lru_del(dentry);
	/* if it was on the hash then remove it */
	__d_drop(dentry);
	return d_kill(dentry, parent);
}

/* 
 * This is dput
 *
 * This is complicated by the fact that we do not want to put
 * dentries that are no longer on any hash chain on the unused
 * list: we'd much rather just get rid of them immediately.
 *
 * However, that implies that we have to traverse the dentry
 * tree upwards to the parents which might _also_ now be
 * scheduled for deletion (it may have been only waiting for
 * its last child to go away).
 *
 * This tail recursion is done by hand as we don't want to depend
 * on the compiler to always get this right (gcc generally doesn't).
 * Real recursion would eat up our stack space.
 */

/*
 * dput - release a dentry
 * @dentry: dentry to release 
 *
 * Release a dentry. This will drop the usage count and if appropriate
 * call the dentry unlink method as well as removing it from the queues and
 * releasing its resources. If the parent dentries were scheduled for release
 * they too may now get deleted.
 */
void dput(struct dentry *dentry)
{
	if (!dentry)
		return;

repeat:
	if (dentry->d_count == 1)
		might_sleep();
	spin_lock(&dentry->d_lock);
	BUG_ON(!dentry->d_count);
	if (dentry->d_count > 1) {
		dentry->d_count--;
		spin_unlock(&dentry->d_lock);
		return;
	}

	if (dentry->d_flags & DCACHE_OP_DELETE) {
		if (dentry->d_op->d_delete(dentry))
			goto kill_it;
	}

	/* Unreachable? Get rid of it */
 	if (d_unhashed(dentry))
		goto kill_it;

	/* Otherwise leave it cached and ensure it's on the LRU */
	dentry->d_flags |= DCACHE_REFERENCED;
	dentry_lru_add(dentry);

	dentry->d_count--;
	spin_unlock(&dentry->d_lock);
	return;

kill_it:
	dentry = dentry_kill(dentry, 1);
	if (dentry)
		goto repeat;
}
EXPORT_SYMBOL(dput);

/**
 * d_invalidate - invalidate a dentry
 * @dentry: dentry to invalidate
 *
 * Try to invalidate the dentry if it turns out to be
 * possible. If there are other dentries that can be
 * reached through this one we can't delete it and we
 * return -EBUSY. On success we return 0.
 *
 * no dcache lock.
 */
 
int d_invalidate(struct dentry * dentry)
{
	/*
	 * If it's already been dropped, return OK.
	 */
	spin_lock(&dentry->d_lock);
	if (d_unhashed(dentry)) {
		spin_unlock(&dentry->d_lock);
		return 0;
	}
	/*
	 * Check whether to do a partial shrink_dcache
	 * to get rid of unused child entries.
	 */
	if (!list_empty(&dentry->d_subdirs)) {
		spin_unlock(&dentry->d_lock);
		shrink_dcache_parent(dentry);
		spin_lock(&dentry->d_lock);
	}

	/*
	 * Somebody else still using it?
	 *
	 * If it's a directory, we can't drop it
	 * for fear of somebody re-populating it
	 * with children (even though dropping it
	 * would make it unreachable from the root,
	 * we might still populate it if it was a
	 * working directory or similar).
	 */
	if (dentry->d_count > 1) {
		if (dentry->d_inode && S_ISDIR(dentry->d_inode->i_mode)) {
			spin_unlock(&dentry->d_lock);
			return -EBUSY;
		}
	}

	__d_drop(dentry);
	spin_unlock(&dentry->d_lock);
	return 0;
}
EXPORT_SYMBOL(d_invalidate);

/* This must be called with d_lock held */
static inline void __dget_dlock(struct dentry *dentry)
{
	dentry->d_count++;
}

static inline void __dget(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	__dget_dlock(dentry);
	spin_unlock(&dentry->d_lock);
}

struct dentry *dget_parent(struct dentry *dentry)
{
	struct dentry *ret;

repeat:
	/*
	 * Don't need rcu_dereference because we re-check it was correct under
	 * the lock.
	 */
	rcu_read_lock();
	ret = dentry->d_parent;
	if (!ret) {
		rcu_read_unlock();
		goto out;
	}
	spin_lock(&ret->d_lock);
	if (unlikely(ret != dentry->d_parent)) {
		spin_unlock(&ret->d_lock);
		rcu_read_unlock();
		goto repeat;
	}
	rcu_read_unlock();
	BUG_ON(!ret->d_count);
	ret->d_count++;
	spin_unlock(&ret->d_lock);
out:
	return ret;
}
EXPORT_SYMBOL(dget_parent);

/**
 * d_find_alias - grab a hashed alias of inode
 * @inode: inode in question
 * @want_discon:  flag, used by d_splice_alias, to request
 *          that only a DISCONNECTED alias be returned.
 *
 * If inode has a hashed alias, or is a directory and has any alias,
 * acquire the reference to alias and return it. Otherwise return NULL.
 * Notice that if inode is a directory there can be only one alias and
 * it can be unhashed only if it has no children, or if it is the root
 * of a filesystem.
 *
 * If the inode has an IS_ROOT, DCACHE_DISCONNECTED alias, then prefer
 * any other hashed alias over that one unless @want_discon is set,
 * in which case only return an IS_ROOT, DCACHE_DISCONNECTED alias.
 */
static struct dentry *__d_find_alias(struct inode *inode, int want_discon)
{
	struct dentry *alias, *discon_alias;

again:
	discon_alias = NULL;
	list_for_each_entry(alias, &inode->i_dentry, d_alias) {
		spin_lock(&alias->d_lock);
 		if (S_ISDIR(inode->i_mode) || !d_unhashed(alias)) {
			if (IS_ROOT(alias) &&
			    (alias->d_flags & DCACHE_DISCONNECTED)) {
				discon_alias = alias;
			} else if (!want_discon) {
				__dget_dlock(alias);
				spin_unlock(&alias->d_lock);
				return alias;
			}
		}
		spin_unlock(&alias->d_lock);
	}
	if (discon_alias) {
		alias = discon_alias;
		spin_lock(&alias->d_lock);
		if (S_ISDIR(inode->i_mode) || !d_unhashed(alias)) {
			if (IS_ROOT(alias) &&
			    (alias->d_flags & DCACHE_DISCONNECTED)) {
				__dget_dlock(alias);
				spin_unlock(&alias->d_lock);
				return alias;
			}
		}
		spin_unlock(&alias->d_lock);
		goto again;
	}
	return NULL;
}

struct dentry *d_find_alias(struct inode *inode)
{
	struct dentry *de = NULL;

	if (!list_empty(&inode->i_dentry)) {
		spin_lock(&inode->i_lock);
		de = __d_find_alias(inode, 0);
		spin_unlock(&inode->i_lock);
	}
	return de;
}
EXPORT_SYMBOL(d_find_alias);

/*
 *	Try to kill dentries associated with this inode.
 * WARNING: you must own a reference to inode.
 */
void d_prune_aliases(struct inode *inode)
{
	struct dentry *dentry;
restart:
	spin_lock(&inode->i_lock);
	list_for_each_entry(dentry, &inode->i_dentry, d_alias) {
		spin_lock(&dentry->d_lock);
		if (!dentry->d_count) {
			__dget_dlock(dentry);
			__d_drop(dentry);
			spin_unlock(&dentry->d_lock);
			spin_unlock(&inode->i_lock);
			dput(dentry);
			goto restart;
		}
		spin_unlock(&dentry->d_lock);
	}
	spin_unlock(&inode->i_lock);
}
EXPORT_SYMBOL(d_prune_aliases);

/*
 * Try to throw away a dentry - free the inode, dput the parent.
 * Requires dentry->d_lock is held, and dentry->d_count == 0.
 * Releases dentry->d_lock.
 *
 * This may fail if locks cannot be acquired no problem, just try again.
 */
static void try_prune_one_dentry(struct dentry *dentry)
	__releases(dentry->d_lock)
{
	struct dentry *parent;

	parent = dentry_kill(dentry, 0);
	/*
	 * If dentry_kill returns NULL, we have nothing more to do.
	 * if it returns the same dentry, trylocks failed. In either
	 * case, just loop again.
	 *
	 * Otherwise, we need to prune ancestors too. This is necessary
	 * to prevent quadratic behavior of shrink_dcache_parent(), but
	 * is also expected to be beneficial in reducing dentry cache
	 * fragmentation.
	 */
	if (!parent)
		return;
	if (parent == dentry)
		return;

	/* Prune ancestors. */
	dentry = parent;
	while (dentry) {
		spin_lock(&dentry->d_lock);
		if (dentry->d_count > 1) {
			dentry->d_count--;
			spin_unlock(&dentry->d_lock);
			return;
		}
		dentry = dentry_kill(dentry, 1);
	}
}

static void shrink_dentry_list(struct list_head *list)
{
	struct dentry *dentry;

	rcu_read_lock();
	for (;;) {
		dentry = list_entry_rcu(list->prev, struct dentry, d_lru);
		if (&dentry->d_lru == list)
			break; /* empty */
		spin_lock(&dentry->d_lock);
		if (dentry != list_entry(list->prev, struct dentry, d_lru)) {
			spin_unlock(&dentry->d_lock);
			continue;
		}

		/*
		 * We found an inuse dentry which was not removed from
		 * the LRU because of laziness during lookup.  Do not free
		 * it - just keep it off the LRU list.
		 */
		if (dentry->d_count) {
			dentry_lru_del(dentry);
			spin_unlock(&dentry->d_lock);
			continue;
		}

		rcu_read_unlock();

		try_prune_one_dentry(dentry);

		rcu_read_lock();
	}
	rcu_read_unlock();
}

/**
 * __shrink_dcache_sb - shrink the dentry LRU on a given superblock
 * @sb:		superblock to shrink dentry LRU.
 * @count:	number of entries to prune
 * @flags:	flags to control the dentry processing
 *
 * If flags contains DCACHE_REFERENCED reference dentries will not be pruned.
 */
static void __shrink_dcache_sb(struct super_block *sb, int *count, int flags)
{
	/* called from prune_dcache() and shrink_dcache_parent() */
	struct dentry *dentry;
	LIST_HEAD(referenced);
	LIST_HEAD(tmp);
	int cnt = *count;

relock:
	spin_lock(&dcache_lru_lock);
	while (!list_empty(&sb->s_dentry_lru)) {
		dentry = list_entry(sb->s_dentry_lru.prev,
				struct dentry, d_lru);
		BUG_ON(dentry->d_sb != sb);

		if (!spin_trylock(&dentry->d_lock)) {
			spin_unlock(&dcache_lru_lock);
			cpu_relax();
			goto relock;
		}

		/*
		 * If we are honouring the DCACHE_REFERENCED flag and the
		 * dentry has this flag set, don't free it.  Clear the flag
		 * and put it back on the LRU.
		 */
		if (flags & DCACHE_REFERENCED &&
				dentry->d_flags & DCACHE_REFERENCED) {
			dentry->d_flags &= ~DCACHE_REFERENCED;
			list_move(&dentry->d_lru, &referenced);
			spin_unlock(&dentry->d_lock);
		} else {
			list_move_tail(&dentry->d_lru, &tmp);
			spin_unlock(&dentry->d_lock);
			if (!--cnt)
				break;
		}
		cond_resched_lock(&dcache_lru_lock);
	}
	if (!list_empty(&referenced))
		list_splice(&referenced, &sb->s_dentry_lru);
	spin_unlock(&dcache_lru_lock);

	shrink_dentry_list(&tmp);

	*count = cnt;
}

/**
 * prune_dcache - shrink the dcache
 * @count: number of entries to try to free
 *
 * Shrink the dcache. This is done when we need more memory, or simply when we
 * need to unmount something (at which point we need to unuse all dentries).
 *
 * This function may fail to free any resources if all the dentries are in use.
 */
static void prune_dcache(int count)
{
	struct super_block *sb, *p = NULL;
	int w_count;
	int unused = dentry_stat.nr_unused;
	int prune_ratio;
	int pruned;

	if (unused == 0 || count == 0)
		return;
	if (count >= unused)
		prune_ratio = 1;
	else
		prune_ratio = unused / count;
	spin_lock(&sb_lock);
	list_for_each_entry(sb, &super_blocks, s_list) {
		if (list_empty(&sb->s_instances))
			continue;
		if (sb->s_nr_dentry_unused == 0)
			continue;
		sb->s_count++;
		/* Now, we reclaim unused dentrins with fairness.
		 * We reclaim them same percentage from each superblock.
		 * We calculate number of dentries to scan on this sb
		 * as follows, but the implementation is arranged to avoid
		 * overflows:
		 * number of dentries to scan on this sb =
		 * count * (number of dentries on this sb /
		 * number of dentries in the machine)
		 */
		spin_unlock(&sb_lock);
		if (prune_ratio != 1)
			w_count = (sb->s_nr_dentry_unused / prune_ratio) + 1;
		else
			w_count = sb->s_nr_dentry_unused;
		pruned = w_count;
		/*
		 * We need to be sure this filesystem isn't being unmounted,
		 * otherwise we could race with generic_shutdown_super(), and
		 * end up holding a reference to an inode while the filesystem
		 * is unmounted.  So we try to get s_umount, and make sure
		 * s_root isn't NULL.
		 */
		if (down_read_trylock(&sb->s_umount)) {
			if ((sb->s_root != NULL) &&
			    (!list_empty(&sb->s_dentry_lru))) {
				__shrink_dcache_sb(sb, &w_count,
						DCACHE_REFERENCED);
				pruned -= w_count;
			}
			up_read(&sb->s_umount);
		}
		spin_lock(&sb_lock);
		if (p)
			__put_super(p);
		count -= pruned;
		p = sb;
		/* more work left to do? */
		if (count <= 0)
			break;
	}
	if (p)
		__put_super(p);
	spin_unlock(&sb_lock);
}

/**
 * shrink_dcache_sb - shrink dcache for a superblock
 * @sb: superblock
 *
 * Shrink the dcache for the specified super block. This is used to free
 * the dcache before unmounting a file system.
 */
void shrink_dcache_sb(struct super_block *sb)
{
	LIST_HEAD(tmp);

	spin_lock(&dcache_lru_lock);
	while (!list_empty(&sb->s_dentry_lru)) {
		list_splice_init(&sb->s_dentry_lru, &tmp);
		spin_unlock(&dcache_lru_lock);
		shrink_dentry_list(&tmp);
		spin_lock(&dcache_lru_lock);
	}
	spin_unlock(&dcache_lru_lock);
}
EXPORT_SYMBOL(shrink_dcache_sb);

/*
 * destroy a single subtree of dentries for unmount
 * - see the comments on shrink_dcache_for_umount() for a description of the
 *   locking
 */
static void shrink_dcache_for_umount_subtree(struct dentry *dentry)
{
	struct dentry *parent;
	unsigned detached = 0;

	BUG_ON(!IS_ROOT(dentry));

	/* detach this root from the system */
	spin_lock(&dentry->d_lock);
	dentry_lru_del(dentry);
	__d_drop(dentry);
	spin_unlock(&dentry->d_lock);

	for (;;) {
		/* descend to the first leaf in the current subtree */
		while (!list_empty(&dentry->d_subdirs)) {
			struct dentry *loop;

			/* this is a branch with children - detach all of them
			 * from the system in one go */
			spin_lock(&dentry->d_lock);
			list_for_each_entry(loop, &dentry->d_subdirs,
					    d_u.d_child) {
				spin_lock_nested(&loop->d_lock,
						DENTRY_D_LOCK_NESTED);
				dentry_lru_del(loop);
				__d_drop(loop);
				spin_unlock(&loop->d_lock);
			}
			spin_unlock(&dentry->d_lock);

			/* move to the first child */
			dentry = list_entry(dentry->d_subdirs.next,
					    struct dentry, d_u.d_child);
		}

		/* consume the dentries from this leaf up through its parents
		 * until we find one with children or run out altogether */
		do {
			struct inode *inode;

			if (dentry->d_count != 0) {
				printk(KERN_ERR
				       "BUG: Dentry %p{i=%lx,n=%s}"
				       " still in use (%d)"
				       " [unmount of %s %s]\n",
				       dentry,
				       dentry->d_inode ?
				       dentry->d_inode->i_ino : 0UL,
				       dentry->d_name.name,
				       dentry->d_count,
				       dentry->d_sb->s_type->name,
				       dentry->d_sb->s_id);
				BUG();
			}

			if (IS_ROOT(dentry)) {
				parent = NULL;
				list_del(&dentry->d_u.d_child);
			} else {
				parent = dentry->d_parent;
				spin_lock(&parent->d_lock);
				parent->d_count--;
				list_del(&dentry->d_u.d_child);
				spin_unlock(&parent->d_lock);
			}

			detached++;

			inode = dentry->d_inode;
			if (inode) {
				dentry->d_inode = NULL;
				list_del_init(&dentry->d_alias);
				if (dentry->d_op && dentry->d_op->d_iput)
					dentry->d_op->d_iput(dentry, inode);
				else
					iput(inode);
			}

			d_free(dentry);

			/* finished when we fall off the top of the tree,
			 * otherwise we ascend to the parent and move to the
			 * next sibling if there is one */
			if (!parent)
				return;
			dentry = parent;
		} while (list_empty(&dentry->d_subdirs));

		dentry = list_entry(dentry->d_subdirs.next,
				    struct dentry, d_u.d_child);
	}
}

/*
 * destroy the dentries attached to a superblock on unmounting
 * - we don't need to use dentry->d_lock because:
 *   - the superblock is detached from all mountings and open files, so the
 *     dentry trees will not be rearranged by the VFS
 *   - s_umount is write-locked, so the memory pressure shrinker will ignore
 *     any dentries belonging to this superblock that it comes across
 *   - the filesystem itself is no longer permitted to rearrange the dentries
 *     in this superblock
 */
void shrink_dcache_for_umount(struct super_block *sb)
{
	struct dentry *dentry;

	if (down_read_trylock(&sb->s_umount))
		BUG();

	dentry = sb->s_root;
	sb->s_root = NULL;
	spin_lock(&dentry->d_lock);
	dentry->d_count--;
	spin_unlock(&dentry->d_lock);
	shrink_dcache_for_umount_subtree(dentry);

	while (!hlist_bl_empty(&sb->s_anon)) {
		dentry = hlist_bl_entry(hlist_bl_first(&sb->s_anon), struct dentry, d_hash);
		shrink_dcache_for_umount_subtree(dentry);
	}
}

/*
 * This tries to ascend one level of parenthood, but
 * we can race with renaming, so we need to re-check
 * the parenthood after dropping the lock and check
 * that the sequence number still matches.
 */
static struct dentry *try_to_ascend(struct dentry *old, int locked, unsigned seq)
{
	struct dentry *new = old->d_parent;

	rcu_read_lock();
	spin_unlock(&old->d_lock);
	spin_lock(&new->d_lock);

	/*
	 * might go back up the wrong parent if we have had a rename
	 * or deletion
	 */
	if (new != old->d_parent ||
		 (old->d_flags & DCACHE_DISCONNECTED) ||
		 (!locked && read_seqretry(&rename_lock, seq))) {
		spin_unlock(&new->d_lock);
		new = NULL;
	}
	rcu_read_unlock();
	return new;
}


/*
 * Search for at least 1 mount point in the dentry's subdirs.
 * We descend to the next level whenever the d_subdirs
 * list is non-empty and continue searching.
 */
 
/**
 * have_submounts - check for mounts over a dentry
 * @parent: dentry to check.
 *
 * Return true if the parent or its subdirectories contain
 * a mount point
 */
int have_submounts(struct dentry *parent)
{
	struct dentry *this_parent;
	struct list_head *next;
	unsigned seq;
	int locked = 0;

	seq = read_seqbegin(&rename_lock);
again:
	this_parent = parent;

	if (d_mountpoint(parent))
		goto positive;
	spin_lock(&this_parent->d_lock);
repeat:
	next = this_parent->d_subdirs.next;
resume:
	while (next != &this_parent->d_subdirs) {
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_u.d_child);
		next = tmp->next;

		spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);
		/* Have we found a mount point ? */
		if (d_mountpoint(dentry)) {
			spin_unlock(&dentry->d_lock);
			spin_unlock(&this_parent->d_lock);
			goto positive;
		}
		if (!list_empty(&dentry->d_subdirs)) {
			spin_unlock(&this_parent->d_lock);
			spin_release(&dentry->d_lock.dep_map, 1, _RET_IP_);
			this_parent = dentry;
			spin_acquire(&this_parent->d_lock.dep_map, 0, 1, _RET_IP_);
			goto repeat;
		}
		spin_unlock(&dentry->d_lock);
	}
	/*
	 * All done at this level ... ascend and resume the search.
	 */
	if (this_parent != parent) {
		struct dentry *child = this_parent;
		this_parent = try_to_ascend(this_parent, locked, seq);
		if (!this_parent)
			goto rename_retry;
		next = child->d_u.d_child.next;
		goto resume;
	}
	spin_unlock(&this_parent->d_lock);
	if (!locked && read_seqretry(&rename_lock, seq))
		goto rename_retry;
	if (locked)
		write_sequnlock(&rename_lock);
	return 0; /* No mount points found in tree */
positive:
	if (!locked && read_seqretry(&rename_lock, seq))
		goto rename_retry;
	if (locked)
		write_sequnlock(&rename_lock);
	return 1;

rename_retry:
	locked = 1;
	write_seqlock(&rename_lock);
	goto again;
}
EXPORT_SYMBOL(have_submounts);

/*
 * Search the dentry child list for the specified parent,
 * and move any unused dentries to the end of the unused
 * list for prune_dcache(). We descend to the next level
 * whenever the d_subdirs list is non-empty and continue
 * searching.
 *
 * It returns zero iff there are no unused children,
 * otherwise  it returns the number of children moved to
 * the end of the unused list. This may not be the total
 * number of unused children, because select_parent can
 * drop the lock and return early due to latency
 * constraints.
 */
static int select_parent(struct dentry * parent)
{
	struct dentry *this_parent;
	struct list_head *next;
	unsigned seq;
	int found = 0;
	int locked = 0;

	seq = read_seqbegin(&rename_lock);
again:
	this_parent = parent;
	spin_lock(&this_parent->d_lock);
repeat:
	next = this_parent->d_subdirs.next;
resume:
	while (next != &this_parent->d_subdirs) {
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_u.d_child);
		next = tmp->next;

		spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);

		/* 
		 * move only zero ref count dentries to the end 
		 * of the unused list for prune_dcache
		 */
		if (!dentry->d_count) {
			dentry_lru_move_tail(dentry);
			found++;
		} else {
			dentry_lru_del(dentry);
		}

		/*
		 * We can return to the caller if we have found some (this
		 * ensures forward progress). We'll be coming back to find
		 * the rest.
		 */
		if (found && need_resched()) {
			spin_unlock(&dentry->d_lock);
			goto out;
		}

		/*
		 * Descend a level if the d_subdirs list is non-empty.
		 */
		if (!list_empty(&dentry->d_subdirs)) {
			spin_unlock(&this_parent->d_lock);
			spin_release(&dentry->d_lock.dep_map, 1, _RET_IP_);
			this_parent = dentry;
			spin_acquire(&this_parent->d_lock.dep_map, 0, 1, _RET_IP_);
			goto repeat;
		}

		spin_unlock(&dentry->d_lock);
	}
	/*
	 * All done at this level ... ascend and resume the search.
	 */
	if (this_parent != parent) {
		struct dentry *child = this_parent;
		this_parent = try_to_ascend(this_parent, locked, seq);
		if (!this_parent)
			goto rename_retry;
		next = child->d_u.d_child.next;
		goto resume;
	}
out:
	spin_unlock(&this_parent->d_lock);
	if (!locked && read_seqretry(&rename_lock, seq))
		goto rename_retry;
	if (locked)
		write_sequnlock(&rename_lock);
	return found;

rename_retry:
	if (found)
		return found;
	locked = 1;
	write_seqlock(&rename_lock);
	goto again;
}

/**
 * shrink_dcache_parent - prune dcache
 * @parent: parent of entries to prune
 *
 * Prune the dcache to remove unused children of the parent dentry.
 */
 
void shrink_dcache_parent(struct dentry * parent)
{
	struct super_block *sb = parent->d_sb;
	int found;

	while ((found = select_parent(parent)) != 0)
		__shrink_dcache_sb(sb, &found, 0);
}
EXPORT_SYMBOL(shrink_dcache_parent);

/*
 * Scan `sc->nr_slab_to_reclaim' dentries and return the number which remain.
 *
 * We need to avoid reentering the filesystem if the caller is performing a
 * GFP_NOFS allocation attempt.  One example deadlock is:
 *
 * ext2_new_block->getblk->GFP->shrink_dcache_memory->prune_dcache->
 * prune_one_dentry->dput->dentry_iput->iput->inode->i_sb->s_op->put_inode->
 * ext2_discard_prealloc->ext2_free_blocks->lock_super->DEADLOCK.
 *
 * In this case we return -1 to tell the caller that we baled.
 */
static int shrink_dcache_memory(struct shrinker *shrink,
				struct shrink_control *sc)
{
	int nr = sc->nr_to_scan;
	gfp_t gfp_mask = sc->gfp_mask;

	if (nr) {
		if (!(gfp_mask & __GFP_FS))
			return -1;
		prune_dcache(nr);
	}

	return (dentry_stat.nr_unused / 100) * sysctl_vfs_cache_pressure;
}

static struct shrinker dcache_shrinker = {
	.shrink = shrink_dcache_memory,
	.seeks = DEFAULT_SEEKS,
};

/**
 * d_alloc	-	allocate a dcache entry
 * @parent: parent of entry to allocate
 * @name: qstr of the name
 *
 * Allocates a dentry. It returns %NULL if there is insufficient memory
 * available. On a success the dentry is returned. The name passed in is
 * copied and the copy passed in may be reused after this call.
 */
 
struct dentry *d_alloc(struct dentry * parent, const struct qstr *name)
{
	struct dentry *dentry;
	char *dname;

	dentry = kmem_cache_alloc(dentry_cache, GFP_KERNEL);
	if (!dentry)
		return NULL;

	if (name->len > DNAME_INLINE_LEN-1) {
		dname = kmalloc(name->len + 1, GFP_KERNEL);
		if (!dname) {
			kmem_cache_free(dentry_cache, dentry); 
			return NULL;
		}
	} else  {
		dname = dentry->d_iname;
	}	
	dentry->d_name.name = dname;

	dentry->d_name.len = name->len;
	dentry->d_name.hash = name->hash;
	memcpy(dname, name->name, name->len);
	dname[name->len] = 0;

	dentry->d_count = 1;
	dentry->d_flags = 0;
	spin_lock_init(&dentry->d_lock);
	seqcount_init(&dentry->d_seq);
	dentry->d_inode = NULL;
	dentry->d_parent = NULL;
	dentry->d_sb = NULL;
	dentry->d_op = NULL;
	dentry->d_fsdata = NULL;
	INIT_HLIST_BL_NODE(&dentry->d_hash);
	INIT_LIST_HEAD(&dentry->d_lru);
	INIT_LIST_HEAD(&dentry->d_subdirs);
	INIT_LIST_HEAD(&dentry->d_alias);
	INIT_LIST_HEAD(&dentry->d_u.d_child);

	if (parent) {
		spin_lock(&parent->d_lock);
		/*
		 * don't need child lock because it is not subject
		 * to concurrency here
		 */
		__dget_dlock(parent);
		dentry->d_parent = parent;
		dentry->d_sb = parent->d_sb;
		d_set_d_op(dentry, dentry->d_sb->s_d_op);
		list_add(&dentry->d_u.d_child, &parent->d_subdirs);
		spin_unlock(&parent->d_lock);
	}

	this_cpu_inc(nr_dentry);

	return dentry;
}
EXPORT_SYMBOL(d_alloc);

struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name)
{
	struct dentry *dentry = d_alloc(NULL, name);
	if (dentry) {
		dentry->d_sb = sb;
		d_set_d_op(dentry, dentry->d_sb->s_d_op);
		dentry->d_parent = dentry;
		dentry->d_flags |= DCACHE_DISCONNECTED;
	}
	return dentry;
}
EXPORT_SYMBOL(d_alloc_pseudo);

struct dentry *d_alloc_name(struct dentry *parent, const char *name)
{
	struct qstr q;

	q.name = name;
	q.len = strlen(name);
	q.hash = full_name_hash(q.name, q.len);
	return d_alloc(parent, &q);
}
EXPORT_SYMBOL(d_alloc_name);

void d_set_d_op(struct dentry *dentry, const struct dentry_operations *op)
{
	WARN_ON_ONCE(dentry->d_op);
	WARN_ON_ONCE(dentry->d_flags & (DCACHE_OP_HASH	|
				DCACHE_OP_COMPARE	|
				DCACHE_OP_REVALIDATE	|
				DCACHE_OP_DELETE ));
	dentry->d_op = op;
	if (!op)
		return;
	if (op->d_hash)
		dentry->d_flags |= DCACHE_OP_HASH;
	if (op->d_compare)
		dentry->d_flags |= DCACHE_OP_COMPARE;
	if (op->d_revalidate)
		dentry->d_flags |= DCACHE_OP_REVALIDATE;
	if (op->d_delete)
		dentry->d_flags |= DCACHE_OP_DELETE;

}
EXPORT_SYMBOL(d_set_d_op);

static void __d_instantiate(struct dentry *dentry, struct inode *inode)
{
	spin_lock(&dentry->d_lock);
	if (inode) {
		if (unlikely(IS_AUTOMOUNT(inode)))
			dentry->d_flags |= DCACHE_NEED_AUTOMOUNT;
		list_add(&dentry->d_alias, &inode->i_dentry);
	}
	dentry->d_inode = inode;
	dentry_rcuwalk_barrier(dentry);
	spin_unlock(&dentry->d_lock);
	fsnotify_d_instantiate(dentry, inode);
}

/**
 * d_instantiate - fill in inode information for a dentry
 * @entry: dentry to complete
 * @inode: inode to attach to this dentry
 *
 * Fill in inode information in the entry.
 *
 * This turns negative dentries into productive full members
 * of society.
 *
 * NOTE! This assumes that the inode count has been incremented
 * (or otherwise set) by the caller to indicate that it is now
 * in use by the dcache.
 */
 
void d_instantiate(struct dentry *entry, struct inode * inode)
{
	BUG_ON(!list_empty(&entry->d_alias));
	if (inode)
		spin_lock(&inode->i_lock);
	__d_instantiate(entry, inode);
	if (inode)
		spin_unlock(&inode->i_lock);
	security_d_instantiate(entry, inode);
}
EXPORT_SYMBOL(d_instantiate);

/**
 * d_instantiate_unique - instantiate a non-aliased dentry
 * @entry: dentry to instantiate
 * @inode: inode to attach to this dentry
 *
 * Fill in inode information in the entry. On success, it returns NULL.
 * If an unhashed alias of "entry" already exists, then we return the
 * aliased dentry instead and drop one reference to inode.
 *
 * Note that in order to avoid conflicts with rename() etc, the caller
 * had better be holding the parent directory semaphore.
 *
 * This also assumes that the inode count has been incremented
 * (or otherwise set) by the caller to indicate that it is now
 * in use by the dcache.
 */
static struct dentry *__d_instantiate_unique(struct dentry *entry,
					     struct inode *inode)
{
	struct dentry *alias;
	int len = entry->d_name.len;
	const char *name = entry->d_name.name;
	unsigned int hash = entry->d_name.hash;

	if (!inode) {
		__d_instantiate(entry, NULL);
		return NULL;
	}

	list_for_each_entry(alias, &inode->i_dentry, d_alias) {
		struct qstr *qstr = &alias->d_name;

		/*
		 * Don't need alias->d_lock here, because aliases with
		 * d_parent == entry->d_parent are not subject to name or
		 * parent changes, because the parent inode i_mutex is held.
		 */
		if (qstr->hash != hash)
			continue;
		if (alias->d_parent != entry->d_parent)
			continue;
		if (dentry_cmp(qstr->name, qstr->len, name, len))
			continue;
		__dget(alias);
		return alias;
	}

	__d_instantiate(entry, inode);
	return NULL;
}

struct dentry *d_instantiate_unique(struct dentry *entry, struct inode *inode)
{
	struct dentry *result;

	BUG_ON(!list_empty(&entry->d_alias));

	if (inode)
		spin_lock(&inode->i_lock);
	result = __d_instantiate_unique(entry, inode);
	if (inode)
		spin_unlock(&inode->i_lock);

	if (!result) {
		security_d_instantiate(entry, inode);
		return NULL;
	}

	BUG_ON(!d_unhashed(result));
	iput(inode);
	return result;
}

EXPORT_SYMBOL(d_instantiate_unique);

/**
 * d_alloc_root - allocate root dentry
 * @root_inode: inode to allocate the root for
 *
 * Allocate a root ("/") dentry for the inode given. The inode is
 * instantiated and returned. %NULL is returned if there is insufficient
 * memory or the inode passed is %NULL.
 */
 
struct dentry * d_alloc_root(struct inode * root_inode)
{
	struct dentry *res = NULL;

	if (root_inode) {
		static const struct qstr name = { .name = "/", .len = 1 };

		res = d_alloc(NULL, &name);
		if (res) {
			res->d_sb = root_inode->i_sb;
			d_set_d_op(res, res->d_sb->s_d_op);
			res->d_parent = res;
			d_instantiate(res, root_inode);
		}
	}
	return res;
}
EXPORT_SYMBOL(d_alloc_root);

static struct dentry * __d_find_any_alias(struct inode *inode)
{
	struct dentry *alias;

	if (list_empty(&inode->i_dentry))
		return NULL;
	alias = list_first_entry(&inode->i_dentry, struct dentry, d_alias);
	__dget(alias);
	return alias;
}

static struct dentry * d_find_any_alias(struct inode *inode)
{
	struct dentry *de;

	spin_lock(&inode->i_lock);
	de = __d_find_any_alias(inode);
	spin_unlock(&inode->i_lock);
	return de;
}


/**
 * d_obtain_alias - find or allocate a dentry for a given inode
 * @inode: inode to allocate the dentry for
 *
 * Obtain a dentry for an inode resulting from NFS filehandle conversion or
 * similar open by handle operations.  The returned dentry may be anonymous,
 * or may have a full name (if the inode was already in the cache).
 *
 * When called on a directory inode, we must ensure that the inode only ever
 * has one dentry.  If a dentry is found, that is returned instead of
 * allocating a new one.
 *
 * On successful return, the reference to the inode has been transferred
 * to the dentry.  In case of an error the reference on the inode is released.
 * To make it easier to use in export operations a %NULL or IS_ERR inode may
 * be passed in and will be the error will be propagate to the return value,
 * with a %NULL @inode replaced by ERR_PTR(-ESTALE).
 */
struct dentry *d_obtain_alias(struct inode *inode)
{
	static const struct qstr anonstring = { .name = "" };
	struct dentry *tmp;
	struct dentry *res;

	if (!inode)
		return ERR_PTR(-ESTALE);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	res = d_find_any_alias(inode);
	if (res)
		goto out_iput;

	tmp = d_alloc(NULL, &anonstring);
	if (!tmp) {
		res = ERR_PTR(-ENOMEM);
		goto out_iput;
	}
	tmp->d_parent = tmp; /* make sure dput doesn't croak */


	spin_lock(&inode->i_lock);
	res = __d_find_any_alias(inode);
	if (res) {
		spin_unlock(&inode->i_lock);
		dput(tmp);
		goto out_iput;
	}

	/* attach a disconnected dentry */
	spin_lock(&tmp->d_lock);
	tmp->d_sb = inode->i_sb;
	d_set_d_op(tmp, tmp->d_sb->s_d_op);
	tmp->d_inode = inode;
	tmp->d_flags |= DCACHE_DISCONNECTED;
	list_add(&tmp->d_alias, &inode->i_dentry);
	hlist_bl_lock(&tmp->d_sb->s_anon);
	hlist_bl_add_head(&tmp->d_hash, &tmp->d_sb->s_anon);
	hlist_bl_unlock(&tmp->d_sb->s_anon);
	spin_unlock(&tmp->d_lock);
	spin_unlock(&inode->i_lock);
	security_d_instantiate(tmp, inode);

	return tmp;

 out_iput:
	if (res && !IS_ERR(res))
		security_d_instantiate(res, inode);
	iput(inode);
	return res;
}
EXPORT_SYMBOL(d_obtain_alias);

/**
 * d_splice_alias - splice a disconnected dentry into the tree if one exists
 * @inode:  the inode which may have a disconnected dentry
 * @dentry: a negative dentry which we want to point to the inode.
 *
 * If inode is a directory and has a 'disconnected' dentry (i.e. IS_ROOT and
 * DCACHE_DISCONNECTED), then d_move that in place of the given dentry
 * and return it, else simply d_add the inode to the dentry and return NULL.
 *
 * This is needed in the lookup routine of any filesystem that is exportable
 * (via knfsd) so that we can build dcache paths to directories effectively.
 *
 * If a dentry was found and moved, then it is returned.  Otherwise NULL
 * is returned.  This matches the expected return value of ->lookup.
 *
 */
struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry)
{
	struct dentry *new = NULL;

	if (inode && S_ISDIR(inode->i_mode)) {
		spin_lock(&inode->i_lock);
		new = __d_find_alias(inode, 1);
		if (new) {
			BUG_ON(!(new->d_flags & DCACHE_DISCONNECTED));
			spin_unlock(&inode->i_lock);
			security_d_instantiate(new, inode);
			d_move(new, dentry);
			iput(inode);
		} else {
			/* already taking inode->i_lock, so d_add() by hand */
			__d_instantiate(dentry, inode);
			spin_unlock(&inode->i_lock);
			security_d_instantiate(dentry, inode);
			d_rehash(dentry);
		}
	} else
		d_add(dentry, inode);
	return new;
}
EXPORT_SYMBOL(d_splice_alias);

/**
 * d_add_ci - lookup or allocate new dentry with case-exact name
 * @inode:  the inode case-insensitive lookup has found
 * @dentry: the negative dentry that was passed to the parent's lookup func
 * @name:   the case-exact name to be associated with the returned dentry
 *
 * This is to avoid filling the dcache with case-insensitive names to the
 * same inode, only the actual correct case is stored in the dcache for
 * case-insensitive filesystems.
 *
 * For a case-insensitive lookup match and if the the case-exact dentry
 * already exists in in the dcache, use it and return it.
 *
 * If no entry exists with the exact case name, allocate new dentry with
 * the exact case, and return the spliced entry.
 */
struct dentry *d_add_ci(struct dentry *dentry, struct inode *inode,
			struct qstr *name)
{
	int error;
	struct dentry *found;
	struct dentry *new;

	/*
	 * First check if a dentry matching the name already exists,
	 * if not go ahead and create it now.
	 */
	found = d_hash_and_lookup(dentry->d_parent, name);
	if (!found) {
		new = d_alloc(dentry->d_parent, name);
		if (!new) {
			error = -ENOMEM;
			goto err_out;
		}

		found = d_splice_alias(inode, new);
		if (found) {
			dput(new);
			return found;
		}
		return new;
	}

	/*
	 * If a matching dentry exists, and it's not negative use it.
	 *
	 * Decrement the reference count to balance the iget() done
	 * earlier on.
	 */
	if (found->d_inode) {
		if (unlikely(found->d_inode != inode)) {
			/* This can't happen because bad inodes are unhashed. */
			BUG_ON(!is_bad_inode(inode));
			BUG_ON(!is_bad_inode(found->d_inode));
		}
		iput(inode);
		return found;
	}

	/*
	 * Negative dentry: instantiate it unless the inode is a directory and
	 * already has a dentry.
	 */
	spin_lock(&inode->i_lock);
	if (!S_ISDIR(inode->i_mode) || list_empty(&inode->i_dentry)) {
		__d_instantiate(found, inode);
		spin_unlock(&inode->i_lock);
		security_d_instantiate(found, inode);
		return found;
	}

	/*
	 * In case a directory already has a (disconnected) entry grab a
	 * reference to it, move it in place and use it.
	 */
	new = list_entry(inode->i_dentry.next, struct dentry, d_alias);
	__dget(new);
	spin_unlock(&inode->i_lock);
	security_d_instantiate(found, inode);
	d_move(new, found);
	iput(inode);
	dput(found);
	return new;

err_out:
	iput(inode);
	return ERR_PTR(error);
}
EXPORT_SYMBOL(d_add_ci);

/**
 * __d_lookup_rcu - search for a dentry (racy, store-free)
 * @parent: parent dentry
 * @name: qstr of name we wish to find
 * @seq: returns d_seq value at the point where the dentry was found
 * @inode: returns dentry->d_inode when the inode was found valid.
 * Returns: dentry, or NULL
 *
 * __d_lookup_rcu is the dcache lookup function for rcu-walk name
 * resolution (store-free path walking) design described in
 * Documentation/filesystems/path-lookup.txt.
 *
 * This is not to be used outside core vfs.
 *
 * __d_lookup_rcu must only be used in rcu-walk mode, ie. with vfsmount lock
 * held, and rcu_read_lock held. The returned dentry must not be stored into
 * without taking d_lock and checking d_seq sequence count against @seq
 * returned here.
 *
 * A refcount may be taken on the found dentry with the __d_rcu_to_refcount
 * function.
 *
 * Alternatively, __d_lookup_rcu may be called again to look up the child of
 * the returned dentry, so long as its parent's seqlock is checked after the
 * child is looked up. Thus, an interlocking stepping of sequence lock checks
 * is formed, giving integrity down the path walk.
 */
struct dentry *__d_lookup_rcu(struct dentry *parent, struct qstr *name,
				unsigned *seq, struct inode **inode)
{
	unsigned int len = name->len;
	unsigned int hash = name->hash;
	const unsigned char *str = name->name;
	struct hlist_bl_head *b = d_hash(parent, hash);
	struct hlist_bl_node *node;
	struct dentry *dentry;

	/*
	 * Note: There is significant duplication with __d_lookup_rcu which is
	 * required to prevent single threaded performance regressions
	 * especially on architectures where smp_rmb (in seqcounts) are costly.
	 * Keep the two functions in sync.
	 */

	/*
	 * The hash list is protected using RCU.
	 *
	 * Carefully use d_seq when comparing a candidate dentry, to avoid
	 * races with d_move().
	 *
	 * It is possible that concurrent renames can mess up our list
	 * walk here and result in missing our dentry, resulting in the
	 * false-negative result. d_lookup() protects against concurrent
	 * renames using rename_lock seqlock.
	 *
	 * See Documentation/filesystems/path-lookup.txt for more details.
	 */
	hlist_bl_for_each_entry_rcu(dentry, node, b, d_hash) {
		struct inode *i;
		const char *tname;
		int tlen;

		if (dentry->d_name.hash != hash)
			continue;

seqretry:
		*seq = read_seqcount_begin(&dentry->d_seq);
		if (dentry->d_parent != parent)
			continue;
		if (d_unhashed(dentry))
			continue;
		tlen = dentry->d_name.len;
		tname = dentry->d_name.name;
		i = dentry->d_inode;
		prefetch(tname);
		/*
		 * This seqcount check is required to ensure name and
		 * len are loaded atomically, so as not to walk off the
		 * edge of memory when walking. If we could load this
		 * atomically some other way, we could drop this check.
		 */
		if (read_seqcount_retry(&dentry->d_seq, *seq))
			goto seqretry;
		if (parent->d_flags & DCACHE_OP_COMPARE) {
			if (parent->d_op->d_compare(parent, *inode,
						dentry, i,
						tlen, tname, name))
				continue;
		} else {
			if (dentry_cmp(tname, tlen, str, len))
				continue;
		}
		/*
		 * No extra seqcount check is required after the name
		 * compare. The caller must perform a seqcount check in
		 * order to do anything useful with the returned dentry
		 * anyway.
		 */
		*inode = i;
		return dentry;
	}
	return NULL;
}

/**
 * d_lookup - search for a dentry
 * @parent: parent dentry
 * @name: qstr of name we wish to find
 * Returns: dentry, or NULL
 *
 * d_lookup searches the children of the parent dentry for the name in
 * question. If the dentry is found its reference count is incremented and the
 * dentry is returned. The caller must use dput to free the entry when it has
 * finished using it. %NULL is returned if the dentry does not exist.
 */
struct dentry *d_lookup(struct dentry *parent, struct qstr *name)
{
	struct dentry *dentry;
	unsigned seq;

        do {
                seq = read_seqbegin(&rename_lock);
                dentry = __d_lookup(parent, name);
                if (dentry)
			break;
	} while (read_seqretry(&rename_lock, seq));
	return dentry;
}
EXPORT_SYMBOL(d_lookup);

/**
 * __d_lookup - search for a dentry (racy)
 * @parent: parent dentry
 * @name: qstr of name we wish to find
 * Returns: dentry, or NULL
 *
 * __d_lookup is like d_lookup, however it may (rarely) return a
 * false-negative result due to unrelated rename activity.
 *
 * __d_lookup is slightly faster by avoiding rename_lock read seqlock,
 * however it must be used carefully, eg. with a following d_lookup in
 * the case of failure.
 *
 * __d_lookup callers must be commented.
 */
struct dentry *__d_lookup(struct dentry *parent, struct qstr *name)
{
	unsigned int len = name->len;
	unsigned int hash = name->hash;
	const unsigned char *str = name->name;
	struct hlist_bl_head *b = d_hash(parent, hash);
	struct hlist_bl_node *node;
	struct dentry *found = NULL;
	struct dentry *dentry;

	/*
	 * Note: There is significant duplication with __d_lookup_rcu which is
	 * required to prevent single threaded performance regressions
	 * especially on architectures where smp_rmb (in seqcounts) are costly.
	 * Keep the two functions in sync.
	 */

	/*
	 * The hash list is protected using RCU.
	 *
	 * Take d_lock when comparing a candidate dentry, to avoid races
	 * with d_move().
	 *
	 * It is possible that concurrent renames can mess up our list
	 * walk here and result in missing our dentry, resulting in the
	 * false-negative result. d_lookup() protects against concurrent
	 * renames using rename_lock seqlock.
	 *
	 * See Documentation/filesystems/path-lookup.txt for more details.
	 */
	rcu_read_lock();
	
	hlist_bl_for_each_entry_rcu(dentry, node, b, d_hash) {
		const char *tname;
		int tlen;

		if (dentry->d_name.hash != hash)
			continue;

		spin_lock(&dentry->d_lock);
		if (dentry->d_parent != parent)
			goto next;
		if (d_unhashed(dentry))
			goto next;

		/*
		 * It is safe to compare names since d_move() cannot
		 * change the qstr (protected by d_lock).
		 */
		tlen = dentry->d_name.len;
		tname = dentry->d_name.name;
		if (parent->d_flags & DCACHE_OP_COMPARE) {
			if (parent->d_op->d_compare(parent, parent->d_inode,
						dentry, dentry->d_inode,
						tlen, tname, name))
				goto next;
		} else {
			if (dentry_cmp(tname, tlen, str, len))
				goto next;
		}

		dentry->d_count++;
		found = dentry;
		spin_unlock(&dentry->d_lock);
		break;
next:
		spin_unlock(&dentry->d_lock);
 	}
 	rcu_read_unlock();

 	return found;
}

/**
 * d_hash_and_lookup - hash the qstr then search for a dentry
 * @dir: Directory to search in
 * @name: qstr of name we wish to find
 *
 * On hash failure or on lookup failure NULL is returned.
 */
struct dentry *d_hash_and_lookup(struct dentry *dir, struct qstr *name)
{
	struct dentry *dentry = NULL;

	/*
	 * Check for a fs-specific hash function. Note that we must
	 * calculate the standard hash first, as the d_op->d_hash()
	 * routine may choose to leave the hash value unchanged.
	 */
	name->hash = full_name_hash(name->name, name->len);
	if (dir->d_flags & DCACHE_OP_HASH) {
		if (dir->d_op->d_hash(dir, dir->d_inode, name) < 0)
			goto out;
	}
	dentry = d_lookup(dir, name);
out:
	return dentry;
}

/**
 * d_validate - verify dentry provided from insecure source (deprecated)
 * @dentry: The dentry alleged to be valid child of @dparent
 * @dparent: The parent dentry (known to be valid)
 *
 * An insecure source has sent us a dentry, here we verify it and dget() it.
 * This is used by ncpfs in its readdir implementation.
 * Zero is returned in the dentry is invalid.
 *
 * This function is slow for big directories, and deprecated, do not use it.
 */
int d_validate(struct dentry *dentry, struct dentry *dparent)
{
	struct dentry *child;

	spin_lock(&dparent->d_lock);
	list_for_each_entry(child, &dparent->d_subdirs, d_u.d_child) {
		if (dentry == child) {
			spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);
			__dget_dlock(dentry);
			spin_unlock(&dentry->d_lock);
			spin_unlock(&dparent->d_lock);
			return 1;
		}
	}
	spin_unlock(&dparent->d_lock);

	return 0;
}
EXPORT_SYMBOL(d_validate);

/*
 * When a file is deleted, we have two options:
 * - turn this dentry into a negative dentry
 * - unhash this dentry and free it.
 *
 * Usually, we want to just turn this into
 * a negative dentry, but if anybody else is
 * currently using the dentry or the inode
 * we can't do that and we fall back on removing
 * it from the hash queues and waiting for
 * it to be deleted later when it has no users
 */
 
/**
 * d_delete - delete a dentry
 * @dentry: The dentry to delete
 *
 * Turn the dentry into a negative dentry if possible, otherwise
 * remove it from the hash queues so it can be deleted later
 */
 
void d_delete(struct dentry * dentry)
{
	struct inode *inode;
	int isdir = 0;
	/*
	 * Are we the only user?
	 */
again:
	spin_lock(&dentry->d_lock);
	inode = dentry->d_inode;
	isdir = S_ISDIR(inode->i_mode);
	if (dentry->d_count == 1) {
		if (inode && !spin_trylock(&inode->i_lock)) {
			spin_unlock(&dentry->d_lock);
			cpu_relax();
			goto again;
		}
		dentry->d_flags &= ~DCACHE_CANT_MOUNT;
		dentry_unlink_inode(dentry);
		fsnotify_nameremove(dentry, isdir);
		return;
	}

	if (!d_unhashed(dentry))
		__d_drop(dentry);

	spin_unlock(&dentry->d_lock);

	fsnotify_nameremove(dentry, isdir);
}
EXPORT_SYMBOL(d_delete);

static void __d_rehash(struct dentry * entry, struct hlist_bl_head *b)
{
	BUG_ON(!d_unhashed(entry));
	hlist_bl_lock(b);
	entry->d_flags |= DCACHE_RCUACCESS;
	hlist_bl_add_head_rcu(&entry->d_hash, b);
	hlist_bl_unlock(b);
}

static void _d_rehash(struct dentry * entry)
{
	__d_rehash(entry, d_hash(entry->d_parent, entry->d_name.hash));
}

/**
 * d_rehash	- add an entry back to the hash
 * @entry: dentry to add to the hash
 *
 * Adds a dentry to the hash according to its name.
 */
 
void d_rehash(struct dentry * entry)
{
	spin_lock(&entry->d_lock);
	_d_rehash(entry);
	spin_unlock(&entry->d_lock);
}
EXPORT_SYMBOL(d_rehash);

/**
 * dentry_update_name_case - update case insensitive dentry with a new name
 * @dentry: dentry to be updated
 * @name: new name
 *
 * Update a case insensitive dentry with new case of name.
 *
 * dentry must have been returned by d_lookup with name @name. Old and new
 * name lengths must match (ie. no d_compare which allows mismatched name
 * lengths).
 *
 * Parent inode i_mutex must be held over d_lookup and into this call (to
 * keep renames and concurrent inserts, and readdir(2) away).
 */
void dentry_update_name_case(struct dentry *dentry, struct qstr *name)
{
	BUG_ON(!mutex_is_locked(&dentry->d_parent->d_inode->i_mutex));
	BUG_ON(dentry->d_name.len != name->len); /* d_lookup gives this */

	spin_lock(&dentry->d_lock);
	write_seqcount_begin(&dentry->d_seq);
	memcpy((unsigned char *)dentry->d_name.name, name->name, name->len);
	write_seqcount_end(&dentry->d_seq);
	spin_unlock(&dentry->d_lock);
}
EXPORT_SYMBOL(dentry_update_name_case);

static void switch_names(struct dentry *dentry, struct dentry *target)
{
	if (dname_external(target)) {
		if (dname_external(dentry)) {
			/*
			 * Both external: swap the pointers
			 */
			swap(target->d_name.name, dentry->d_name.name);
		} else {
			/*
			 * dentry:internal, target:external.  Steal target's
			 * storage and make target internal.
			 */
			memcpy(target->d_iname, dentry->d_name.name,
					dentry->d_name.len + 1);
			dentry->d_name.name = target->d_name.name;
			target->d_name.name = target->d_iname;
		}
	} else {
		if (dname_external(dentry)) {
			/*
			 * dentry:external, target:internal.  Give dentry's
			 * storage to target and make dentry internal
			 */
			memcpy(dentry->d_iname, target->d_name.name,
					target->d_name.len + 1);
			target->d_name.name = dentry->d_name.name;
			dentry->d_name.name = dentry->d_iname;
		} else {
			/*
			 * Both are internal.  Just copy target to dentry
			 */
			memcpy(dentry->d_iname, target->d_name.name,
					target->d_name.len + 1);
			dentry->d_name.len = target->d_name.len;
			return;
		}
	}
	swap(dentry->d_name.len, target->d_name.len);
}

static void dentry_lock_for_move(struct dentry *dentry, struct dentry *target)
{
	/*
	 * XXXX: do we really need to take target->d_lock?
	 */
	if (IS_ROOT(dentry) || dentry->d_parent == target->d_parent)
		spin_lock(&target->d_parent->d_lock);
	else {
		if (d_ancestor(dentry->d_parent, target->d_parent)) {
			spin_lock(&dentry->d_parent->d_lock);
			spin_lock_nested(&target->d_parent->d_lock,
						DENTRY_D_LOCK_NESTED);
		} else {
			spin_lock(&target->d_parent->d_lock);
			spin_lock_nested(&dentry->d_parent->d_lock,
						DENTRY_D_LOCK_NESTED);
		}
	}
	if (target < dentry) {
		spin_lock_nested(&target->d_lock, 2);
		spin_lock_nested(&dentry->d_lock, 3);
	} else {
		spin_lock_nested(&dentry->d_lock, 2);
		spin_lock_nested(&target->d_lock, 3);
	}
}

static void dentry_unlock_parents_for_move(struct dentry *dentry,
					struct dentry *target)
{
	if (target->d_parent != dentry->d_parent)
		spin_unlock(&dentry->d_parent->d_lock);
	if (target->d_parent != target)
		spin_unlock(&target->d_parent->d_lock);
}

/*
 * When switching names, the actual string doesn't strictly have to
 * be preserved in the target - because we're dropping the target
 * anyway. As such, we can just do a simple memcpy() to copy over
 * the new name before we switch.
 *
 * Note that we have to be a lot more careful about getting the hash
 * switched - we have to switch the hash value properly even if it
 * then no longer matches the actual (corrupted) string of the target.
 * The hash value has to match the hash queue that the dentry is on..
 */
/*
 * __d_move - move a dentry
 * @dentry: entry to move
 * @target: new dentry
 *
 * Update the dcache to reflect the move of a file name. Negative
 * dcache entries should not be moved in this way.  Caller hold
 * rename_lock.
 */
static void __d_move(struct dentry * dentry, struct dentry * target)
{
	if (!dentry->d_inode)
		printk(KERN_WARNING "VFS: moving negative dcache entry\n");

	BUG_ON(d_ancestor(dentry, target));
	BUG_ON(d_ancestor(target, dentry));

	dentry_lock_for_move(dentry, target);

	write_seqcount_begin(&dentry->d_seq);
	write_seqcount_begin(&target->d_seq);

	/* __d_drop does write_seqcount_barrier, but they're OK to nest. */

	/*
	 * Move the dentry to the target hash queue. Don't bother checking
	 * for the same hash queue because of how unlikely it is.
	 */
	__d_drop(dentry);
	__d_rehash(dentry, d_hash(target->d_parent, target->d_name.hash));

	/* Unhash the target: dput() will then get rid of it */
	__d_drop(target);

	list_del(&dentry->d_u.d_child);
	list_del(&target->d_u.d_child);

	/* Switch the names.. */
	switch_names(dentry, target);
	swap(dentry->d_name.hash, target->d_name.hash);

	/* ... and switch the parents */
	if (IS_ROOT(dentry)) {
		dentry->d_parent = target->d_parent;
		target->d_parent = target;
		INIT_LIST_HEAD(&target->d_u.d_child);
	} else {
		swap(dentry->d_parent, target->d_parent);

		/* And add them back to the (new) parent lists */
		list_add(&target->d_u.d_child, &target->d_parent->d_subdirs);
	}

	list_add(&dentry->d_u.d_child, &dentry->d_parent->d_subdirs);

	write_seqcount_end(&target->d_seq);
	write_seqcount_end(&dentry->d_seq);

	dentry_unlock_parents_for_move(dentry, target);
	spin_unlock(&target->d_lock);
	fsnotify_d_move(dentry);
	spin_unlock(&dentry->d_lock);
}

/*
 * d_move - move a dentry
 * @dentry: entry to move
 * @target: new dentry
 *
 * Update the dcache to reflect the move of a file name. Negative
 * dcache entries should not be moved in this way.
 */
void d_move(struct dentry *dentry, struct dentry *target)
{
	write_seqlock(&rename_lock);
	__d_move(dentry, target);
	write_sequnlock(&rename_lock);
}
EXPORT_SYMBOL(d_move);

/**
 * d_ancestor - search for an ancestor
 * @p1: ancestor dentry
 * @p2: child dentry
 *
 * Returns the ancestor dentry of p2 which is a child of p1, if p1 is
 * an ancestor of p2, else NULL.
 */
struct dentry *d_ancestor(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	for (p = p2; !IS_ROOT(p); p = p->d_parent) {
		if (p->d_parent == p1)
			return p;
	}
	return NULL;
}

/*
 * This helper attempts to cope with remotely renamed directories
 *
 * It assumes that the caller is already holding
 * dentry->d_parent->d_inode->i_mutex, inode->i_lock and rename_lock
 *
 * Note: If ever the locking in lock_rename() changes, then please
 * remember to update this too...
 */
static struct dentry *__d_unalias(struct inode *inode,
		struct dentry *dentry, struct dentry *alias)
{
	struct mutex *m1 = NULL, *m2 = NULL;
	struct dentry *ret;

	/* If alias and dentry share a parent, then no extra locks required */
	if (alias->d_parent == dentry->d_parent)
		goto out_unalias;

	/* See lock_rename() */
	ret = ERR_PTR(-EBUSY);
	if (!mutex_trylock(&dentry->d_sb->s_vfs_rename_mutex))
		goto out_err;
	m1 = &dentry->d_sb->s_vfs_rename_mutex;
	if (!mutex_trylock(&alias->d_parent->d_inode->i_mutex))
		goto out_err;
	m2 = &alias->d_parent->d_inode->i_mutex;
out_unalias:
	__d_move(alias, dentry);
	ret = alias;
out_err:
	spin_unlock(&inode->i_lock);
	if (m2)
		mutex_unlock(m2);
	if (m1)
		mutex_unlock(m1);
	return ret;
}

/*
 * Prepare an anonymous dentry for life in the superblock's dentry tree as a
 * named dentry in place of the dentry to be replaced.
 * returns with anon->d_lock held!
 */
static void __d_materialise_dentry(struct dentry *dentry, struct dentry *anon)
{
	struct dentry *dparent, *aparent;

	dentry_lock_for_move(anon, dentry);

	write_seqcount_begin(&dentry->d_seq);
	write_seqcount_begin(&anon->d_seq);

	dparent = dentry->d_parent;
	aparent = anon->d_parent;

	switch_names(dentry, anon);
	swap(dentry->d_name.hash, anon->d_name.hash);

	dentry->d_parent = (aparent == anon) ? dentry : aparent;
	list_del(&dentry->d_u.d_child);
	if (!IS_ROOT(dentry))
		list_add(&dentry->d_u.d_child, &dentry->d_parent->d_subdirs);
	else
		INIT_LIST_HEAD(&dentry->d_u.d_child);

	anon->d_parent = (dparent == dentry) ? anon : dparent;
	list_del(&anon->d_u.d_child);
	if (!IS_ROOT(anon))
		list_add(&anon->d_u.d_child, &anon->d_parent->d_subdirs);
	else
		INIT_LIST_HEAD(&anon->d_u.d_child);

	write_seqcount_end(&dentry->d_seq);
	write_seqcount_end(&anon->d_seq);

	dentry_unlock_parents_for_move(anon, dentry);
	spin_unlock(&dentry->d_lock);

	/* anon->d_lock still locked, returns locked */
	anon->d_flags &= ~DCACHE_DISCONNECTED;
}

/**
 * d_materialise_unique - introduce an inode into the tree
 * @dentry: candidate dentry
 * @inode: inode to bind to the dentry, to which aliases may be attached
 *
 * Introduces an dentry into the tree, substituting an extant disconnected
 * root directory alias in its place if there is one
 */
struct dentry *d_materialise_unique(struct dentry *dentry, struct inode *inode)
{
	struct dentry *actual;

	BUG_ON(!d_unhashed(dentry));

	if (!inode) {
		actual = dentry;
		__d_instantiate(dentry, NULL);
		d_rehash(actual);
		goto out_nolock;
	}

	spin_lock(&inode->i_lock);

	if (S_ISDIR(inode->i_mode)) {
		struct dentry *alias;

		/* Does an aliased dentry already exist? */
		alias = __d_find_alias(inode, 0);
		if (alias) {
			actual = alias;
			write_seqlock(&rename_lock);

			if (d_ancestor(alias, dentry)) {
				/* Check for loops */
				actual = ERR_PTR(-ELOOP);
			} else if (IS_ROOT(alias)) {
				/* Is this an anonymous mountpoint that we
				 * could splice into our tree? */
				__d_materialise_dentry(dentry, alias);
				write_sequnlock(&rename_lock);
				__d_drop(alias);
				goto found;
			} else {
				/* Nope, but we must(!) avoid directory
				 * aliasing */
				actual = __d_unalias(inode, dentry, alias);
			}
			write_sequnlock(&rename_lock);
			if (IS_ERR(actual))
				dput(alias);
			goto out_nolock;
		}
	}

	/* Add a unique reference */
	actual = __d_instantiate_unique(dentry, inode);
	if (!actual)
		actual = dentry;
	else
		BUG_ON(!d_unhashed(actual));

	spin_lock(&actual->d_lock);
found:
	_d_rehash(actual);
	spin_unlock(&actual->d_lock);
	spin_unlock(&inode->i_lock);
out_nolock:
	if (actual == dentry) {
		security_d_instantiate(dentry, inode);
		return NULL;
	}

	iput(inode);
	return actual;
}
EXPORT_SYMBOL_GPL(d_materialise_unique);

static int prepend(char **buffer, int *buflen, const char *str, int namelen)
{
	*buflen -= namelen;
	if (*buflen < 0)
		return -ENAMETOOLONG;
	*buffer -= namelen;
	memcpy(*buffer, str, namelen);
	return 0;
}

static int prepend_name(char **buffer, int *buflen, struct qstr *name)
{
	return prepend(buffer, buflen, name->name, name->len);
}

/**
 * prepend_path - Prepend path string to a buffer
 * @path: the dentry/vfsmount to report
 * @root: root vfsmnt/dentry
 * @buffer: pointer to the end of the buffer
 * @buflen: pointer to buffer length
 *
 * Caller holds the rename_lock.
 */
static int prepend_path(const struct path *path,
			const struct path *root,
			char **buffer, int *buflen)
{
	struct dentry *dentry = path->dentry;
	struct vfsmount *vfsmnt = path->mnt;
	bool slash = false;
	int error = 0;

	br_read_lock(vfsmount_lock);
	while (dentry != root->dentry || vfsmnt != root->mnt) {
		struct dentry * parent;

		if (dentry == vfsmnt->mnt_root || IS_ROOT(dentry)) {
			/* Global root? */
			if (vfsmnt->mnt_parent == vfsmnt) {
				goto global_root;
			}
			dentry = vfsmnt->mnt_mountpoint;
			vfsmnt = vfsmnt->mnt_parent;
			continue;
		}
		parent = dentry->d_parent;
		prefetch(parent);
		spin_lock(&dentry->d_lock);
		error = prepend_name(buffer, buflen, &dentry->d_name);
		spin_unlock(&dentry->d_lock);
		if (!error)
			error = prepend(buffer, buflen, "/", 1);
		if (error)
			break;

		slash = true;
		dentry = parent;
	}

	if (!error && !slash)
		error = prepend(buffer, buflen, "/", 1);

out:
	br_read_unlock(vfsmount_lock);
	return error;

global_root:
	/*
	 * Filesystems needing to implement special "root names"
	 * should do so with ->d_dname()
	 */
	if (IS_ROOT(dentry) &&
	    (dentry->d_name.len != 1 || dentry->d_name.name[0] != '/')) {
		WARN(1, "Root dentry has weird name <%.*s>\n",
		     (int) dentry->d_name.len, dentry->d_name.name);
	}
	if (!slash)
		error = prepend(buffer, buflen, "/", 1);
	if (!error)
		error = vfsmnt->mnt_ns ? 1 : 2;
	goto out;
}

/**
 * __d_path - return the path of a dentry
 * @path: the dentry/vfsmount to report
 * @root: root vfsmnt/dentry
 * @buf: buffer to return value in
 * @buflen: buffer length
 *
 * Convert a dentry into an ASCII path name.
 *
 * Returns a pointer into the buffer or an error code if the
 * path was too long.
 *
 * "buflen" should be positive.
 *
 * If the path is not reachable from the supplied root, return %NULL.
 */
char *__d_path(const struct path *path,
	       const struct path *root,
	       char *buf, int buflen)
{
	char *res = buf + buflen;
	int error;

	prepend(&res, &buflen, "\0", 1);
	write_seqlock(&rename_lock);
	error = prepend_path(path, root, &res, &buflen);
	write_sequnlock(&rename_lock);

	if (error < 0)
		return ERR_PTR(error);
	if (error > 0)
		return NULL;
	return res;
}

char *d_absolute_path(const struct path *path,
	       char *buf, int buflen)
{
	struct path root = {};
	char *res = buf + buflen;
	int error;

	prepend(&res, &buflen, "\0", 1);
	write_seqlock(&rename_lock);
	error = prepend_path(path, &root, &res, &buflen);
	write_sequnlock(&rename_lock);

	if (error > 1)
		error = -EINVAL;
	if (error < 0)
		return ERR_PTR(error);
	return res;
}

/*
 * same as __d_path but appends "(deleted)" for unlinked files.
 */
static int path_with_deleted(const struct path *path,
			     const struct path *root,
			     char **buf, int *buflen)
{
	prepend(buf, buflen, "\0", 1);
	if (d_unlinked(path->dentry)) {
		int error = prepend(buf, buflen, " (deleted)", 10);
		if (error)
			return error;
	}

	return prepend_path(path, root, buf, buflen);
}

static int prepend_unreachable(char **buffer, int *buflen)
{
	return prepend(buffer, buflen, "(unreachable)", 13);
}

/**
 * d_path - return the path of a dentry
 * @path: path to report
 * @buf: buffer to return value in
 * @buflen: buffer length
 *
 * Convert a dentry into an ASCII path name. If the entry has been deleted
 * the string " (deleted)" is appended. Note that this is ambiguous.
 *
 * Returns a pointer into the buffer or an error code if the path was
 * too long. Note: Callers should use the returned pointer, not the passed
 * in buffer, to use the name! The implementation often starts at an offset
 * into the buffer, and may leave 0 bytes at the start.
 *
 * "buflen" should be positive.
 */
char *d_path(const struct path *path, char *buf, int buflen)
{
	char *res = buf + buflen;
	struct path root;
	int error;

	/*
	 * We have various synthetic filesystems that never get mounted.  On
	 * these filesystems dentries are never used for lookup purposes, and
	 * thus don't need to be hashed.  They also don't need a name until a
	 * user wants to identify the object in /proc/pid/fd/.  The little hack
	 * below allows us to generate a name for these objects on demand:
	 */
	if (path->dentry->d_op && path->dentry->d_op->d_dname)
		return path->dentry->d_op->d_dname(path->dentry, buf, buflen);

	get_fs_root(current->fs, &root);
	write_seqlock(&rename_lock);
	error = path_with_deleted(path, &root, &res, &buflen);
	if (error < 0)
		res = ERR_PTR(error);
	write_sequnlock(&rename_lock);
	path_put(&root);
	return res;
}
EXPORT_SYMBOL(d_path);

/**
 * d_path_with_unreachable - return the path of a dentry
 * @path: path to report
 * @buf: buffer to return value in
 * @buflen: buffer length
 *
 * The difference from d_path() is that this prepends "(unreachable)"
 * to paths which are unreachable from the current process' root.
 */
char *d_path_with_unreachable(const struct path *path, char *buf, int buflen)
{
	char *res = buf + buflen;
	struct path root;
	int error;

	if (path->dentry->d_op && path->dentry->d_op->d_dname)
		return path->dentry->d_op->d_dname(path->dentry, buf, buflen);

	get_fs_root(current->fs, &root);
	write_seqlock(&rename_lock);
	error = path_with_deleted(path, &root, &res, &buflen);
	if (error > 0)
		error = prepend_unreachable(&res, &buflen);
	write_sequnlock(&rename_lock);
	path_put(&root);
	if (error)
		res =  ERR_PTR(error);

	return res;
}

/*
 * Helper function for dentry_operations.d_dname() members
 */
char *dynamic_dname(struct dentry *dentry, char *buffer, int buflen,
			const char *fmt, ...)
{
	va_list args;
	char temp[64];
	int sz;

	va_start(args, fmt);
	sz = vsnprintf(temp, sizeof(temp), fmt, args) + 1;
	va_end(args);

	if (sz > sizeof(temp) || sz > buflen)
		return ERR_PTR(-ENAMETOOLONG);

	buffer += buflen - sz;
	return memcpy(buffer, temp, sz);
}

/*
 * Write full pathname from the root of the filesystem into the buffer.
 */
static char *__dentry_path(struct dentry *dentry, char *buf, int buflen)
{
	char *end = buf + buflen;
	char *retval;

	prepend(&end, &buflen, "\0", 1);
	if (buflen < 1)
		goto Elong;
	/* Get '/' right */
	retval = end-1;
	*retval = '/';

	while (!IS_ROOT(dentry)) {
		struct dentry *parent = dentry->d_parent;
		int error;

		prefetch(parent);
		spin_lock(&dentry->d_lock);
		error = prepend_name(&end, &buflen, &dentry->d_name);
		spin_unlock(&dentry->d_lock);
		if (error != 0 || prepend(&end, &buflen, "/", 1) != 0)
			goto Elong;

		retval = end;
		dentry = parent;
	}
	return retval;
Elong:
	return ERR_PTR(-ENAMETOOLONG);
}

char *dentry_path_raw(struct dentry *dentry, char *buf, int buflen)
{
	char *retval;

	write_seqlock(&rename_lock);
	retval = __dentry_path(dentry, buf, buflen);
	write_sequnlock(&rename_lock);

	return retval;
}
EXPORT_SYMBOL(dentry_path_raw);

char *dentry_path(struct dentry *dentry, char *buf, int buflen)
{
	char *p = NULL;
	char *retval;

	write_seqlock(&rename_lock);
	if (d_unlinked(dentry)) {
		p = buf + buflen;
		if (prepend(&p, &buflen, "//deleted", 10) != 0)
			goto Elong;
		buflen++;
	}
	retval = __dentry_path(dentry, buf, buflen);
	write_sequnlock(&rename_lock);
	if (!IS_ERR(retval) && p)
		*p = '/';	/* restore '/' overriden with '\0' */
	return retval;
Elong:
	return ERR_PTR(-ENAMETOOLONG);
}

/*
 * NOTE! The user-level library version returns a
 * character pointer. The kernel system call just
 * returns the length of the buffer filled (which
 * includes the ending '\0' character), or a negative
 * error value. So libc would do something like
 *
 *	char *getcwd(char * buf, size_t size)
 *	{
 *		int retval;
 *
 *		retval = sys_getcwd(buf, size);
 *		if (retval >= 0)
 *			return buf;
 *		errno = -retval;
 *		return NULL;
 *	}
 */
SYSCALL_DEFINE2(getcwd, char __user *, buf, unsigned long, size)
{
	int error;
	struct path pwd, root;
	char *page = (char *) __get_free_page(GFP_USER);

	if (!page)
		return -ENOMEM;

	get_fs_root_and_pwd(current->fs, &root, &pwd);

	error = -ENOENT;
	write_seqlock(&rename_lock);
	if (!d_unlinked(pwd.dentry)) {
		unsigned long len;
		char *cwd = page + PAGE_SIZE;
		int buflen = PAGE_SIZE;

		prepend(&cwd, &buflen, "\0", 1);
		error = prepend_path(&pwd, &root, &cwd, &buflen);
		write_sequnlock(&rename_lock);

		if (error < 0)
			goto out;

		/* Unreachable from current root */
		if (error > 0) {
			error = prepend_unreachable(&cwd, &buflen);
			if (error)
				goto out;
		}

		error = -ERANGE;
		len = PAGE_SIZE + page - cwd;
		if (len <= size) {
			error = len;
			if (copy_to_user(buf, cwd, len))
				error = -EFAULT;
		}
	} else {
		write_sequnlock(&rename_lock);
	}

out:
	path_put(&pwd);
	path_put(&root);
	free_page((unsigned long) page);
	return error;
}

/*
 * Test whether new_dentry is a subdirectory of old_dentry.
 *
 * Trivially implemented using the dcache structure
 */

/**
 * is_subdir - is new dentry a subdirectory of old_dentry
 * @new_dentry: new dentry
 * @old_dentry: old dentry
 *
 * Returns 1 if new_dentry is a subdirectory of the parent (at any depth).
 * Returns 0 otherwise.
 * Caller must ensure that "new_dentry" is pinned before calling is_subdir()
 */
  
int is_subdir(struct dentry *new_dentry, struct dentry *old_dentry)
{
	int result;
	unsigned seq;

	if (new_dentry == old_dentry)
		return 1;

	do {
		/* for restarting inner loop in case of seq retry */
		seq = read_seqbegin(&rename_lock);
		/*
		 * Need rcu_readlock to protect against the d_parent trashing
		 * due to d_move
		 */
		rcu_read_lock();
		if (d_ancestor(old_dentry, new_dentry))
			result = 1;
		else
			result = 0;
		rcu_read_unlock();
	} while (read_seqretry(&rename_lock, seq));

	return result;
}

int path_is_under(struct path *path1, struct path *path2)
{
	struct vfsmount *mnt = path1->mnt;
	struct dentry *dentry = path1->dentry;
	int res;

	br_read_lock(vfsmount_lock);
	if (mnt != path2->mnt) {
		for (;;) {
			if (mnt->mnt_parent == mnt) {
				br_read_unlock(vfsmount_lock);
				return 0;
			}
			if (mnt->mnt_parent == path2->mnt)
				break;
			mnt = mnt->mnt_parent;
		}
		dentry = mnt->mnt_mountpoint;
	}
	res = is_subdir(dentry, path2->dentry);
	br_read_unlock(vfsmount_lock);
	return res;
}
EXPORT_SYMBOL(path_is_under);

void d_genocide(struct dentry *root)
{
	struct dentry *this_parent;
	struct list_head *next;
	unsigned seq;
	int locked = 0;

	seq = read_seqbegin(&rename_lock);
again:
	this_parent = root;
	spin_lock(&this_parent->d_lock);
repeat:
	next = this_parent->d_subdirs.next;
resume:
	while (next != &this_parent->d_subdirs) {
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_u.d_child);
		next = tmp->next;

		spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);
		if (d_unhashed(dentry) || !dentry->d_inode) {
			spin_unlock(&dentry->d_lock);
			continue;
		}
		if (!list_empty(&dentry->d_subdirs)) {
			spin_unlock(&this_parent->d_lock);
			spin_release(&dentry->d_lock.dep_map, 1, _RET_IP_);
			this_parent = dentry;
			spin_acquire(&this_parent->d_lock.dep_map, 0, 1, _RET_IP_);
			goto repeat;
		}
		if (!(dentry->d_flags & DCACHE_GENOCIDE)) {
			dentry->d_flags |= DCACHE_GENOCIDE;
			dentry->d_count--;
		}
		spin_unlock(&dentry->d_lock);
	}
	if (this_parent != root) {
		struct dentry *child = this_parent;
		if (!(this_parent->d_flags & DCACHE_GENOCIDE)) {
			this_parent->d_flags |= DCACHE_GENOCIDE;
			this_parent->d_count--;
		}
		this_parent = try_to_ascend(this_parent, locked, seq);
		if (!this_parent)
			goto rename_retry;
		next = child->d_u.d_child.next;
		goto resume;
	}
	spin_unlock(&this_parent->d_lock);
	if (!locked && read_seqretry(&rename_lock, seq))
		goto rename_retry;
	if (locked)
		write_sequnlock(&rename_lock);
	return;

rename_retry:
	locked = 1;
	write_seqlock(&rename_lock);
	goto again;
}

/**
 * find_inode_number - check for dentry with name
 * @dir: directory to check
 * @name: Name to find.
 *
 * Check whether a dentry already exists for the given name,
 * and return the inode number if it has an inode. Otherwise
 * 0 is returned.
 *
 * This routine is used to post-process directory listings for
 * filesystems using synthetic inode numbers, and is necessary
 * to keep getcwd() working.
 */
 
ino_t find_inode_number(struct dentry *dir, struct qstr *name)
{
	struct dentry * dentry;
	ino_t ino = 0;

	dentry = d_hash_and_lookup(dir, name);
	if (dentry) {
		if (dentry->d_inode)
			ino = dentry->d_inode->i_ino;
		dput(dentry);
	}
	return ino;
}
EXPORT_SYMBOL(find_inode_number);

static __initdata unsigned long dhash_entries;
static int __init set_dhash_entries(char *str)
{
	if (!str)
		return 0;
	dhash_entries = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("dhash_entries=", set_dhash_entries);

static void __init dcache_init_early(void)
{
	int loop;

	/* If hashes are distributed across NUMA nodes, defer
	 * hash allocation until vmalloc space is available.
	 */
	if (hashdist)
		return;

	dentry_hashtable =
		alloc_large_system_hash("Dentry cache",
					sizeof(struct hlist_bl_head),
					dhash_entries,
					13,
					HASH_EARLY,
					&d_hash_shift,
					&d_hash_mask,
					0);

	for (loop = 0; loop < (1 << d_hash_shift); loop++)
		INIT_HLIST_BL_HEAD(dentry_hashtable + loop);
}

static void __init dcache_init(void)
{
	int loop;

	/* 
	 * A constructor could be added for stable state like the lists,
	 * but it is probably not worth it because of the cache nature
	 * of the dcache. 
	 */
	dentry_cache = KMEM_CACHE(dentry,
		SLAB_RECLAIM_ACCOUNT|SLAB_PANIC|SLAB_MEM_SPREAD);
	
	register_shrinker(&dcache_shrinker);

	/* Hash may have been set up in dcache_init_early */
	if (!hashdist)
		return;

	dentry_hashtable =
		alloc_large_system_hash("Dentry cache",
					sizeof(struct hlist_bl_head),
					dhash_entries,
					13,
					0,
					&d_hash_shift,
					&d_hash_mask,
					0);

	for (loop = 0; loop < (1 << d_hash_shift); loop++)
		INIT_HLIST_BL_HEAD(dentry_hashtable + loop);
}

/* SLAB cache for __getname() consumers */
struct kmem_cache *names_cachep __read_mostly;
EXPORT_SYMBOL(names_cachep);

EXPORT_SYMBOL(d_genocide);

void __init vfs_caches_init_early(void)
{
	dcache_init_early();
	inode_init_early();
}

void __init vfs_caches_init(unsigned long mempages)
{
	unsigned long reserve;

	/* Base hash sizes on available memory, with a reserve equal to
           150% of current kernel size */

	reserve = min((mempages - nr_free_pages()) * 3/2, mempages - 1);
	mempages -= reserve;

	names_cachep = kmem_cache_create("names_cache", PATH_MAX, 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);

	dcache_init();
	inode_init();
	files_init(mempages);
	mnt_init();
	bdev_cache_init();
	chrdev_init();
}
