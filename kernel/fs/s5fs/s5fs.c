/*
 *   FILE: s5fs.c
 * AUTHOR: afenn
 *  DESCR: S5FS entry points
 */

#include "kernel.h"
#include "types.h"
#include "globals.h"
#include "errno.h"

#include "util/string.h"
#include "util/printf.h"
#include "util/debug.h"

#include "proc/kmutex.h"

#include "fs/s5fs/s5fs_subr.h"
#include "fs/s5fs/s5fs.h"
#include "fs/dirent.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/file.h"
#include "fs/stat.h"

#include "drivers/dev.h"
#include "drivers/blockdev.h"

#include "mm/kmalloc.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "mm/mm.h"
#include "mm/mman.h"

#include "vm/vmmap.h"
#include "vm/shadow.h"

/* Diagnostic/Utility: */
static int s5_check_super(s5_super_t *super);
static int s5fs_check_refcounts(fs_t *fs);

/* fs_t entry points: */
static void s5fs_read_vnode(vnode_t *vnode);
static void s5fs_delete_vnode(vnode_t *vnode);
static int  s5fs_query_vnode(vnode_t *vnode);
static int  s5fs_umount(fs_t *fs);
static int  s5fs_free_blocks(fs_t *fs);

/* vnode_t entry points: */
static int  s5fs_read(vnode_t *vnode, off_t offset, void *buf, size_t len);
static int  s5fs_write(vnode_t *vnode, off_t offset, const void *buf, size_t len);
static int  s5fs_mmap(vnode_t *file, vmarea_t *vma, mmobj_t **ret);
static int  s5fs_create(vnode_t *vdir, const char *name, size_t namelen, vnode_t **result);
static int  s5fs_mknod(struct vnode *dir, const char *name, size_t namelen, int mode, devid_t devid);
static int  s5fs_lookup(vnode_t *base, const char *name, size_t namelen, vnode_t **result);
static int  s5fs_link(vnode_t *src, vnode_t *dir, const char *name, size_t namelen);
static int  s5fs_unlink(vnode_t *vdir, const char *name, size_t namelen);
static int  s5fs_mkdir(vnode_t *vdir, const char *name, size_t namelen);
static int  s5fs_rmdir(vnode_t *parent, const char *name, size_t namelen);
static int  s5fs_readdir(vnode_t *vnode, int offset, struct dirent *d);
static int  s5fs_stat(vnode_t *vnode, struct stat *ss);
static int  s5fs_fillpage(vnode_t *vnode, off_t offset, void *pagebuf);
static int  s5fs_dirtypage(vnode_t *vnode, off_t offset);
static int  s5fs_cleanpage(vnode_t *vnode, off_t offset, void *pagebuf);

fs_ops_t s5fs_fsops = {
        s5fs_read_vnode,
        s5fs_delete_vnode,
        s5fs_query_vnode,
        s5fs_umount
};

/* vnode operations table for directory files: */
static vnode_ops_t s5fs_dir_vops = {
        .read = NULL,
        .write = NULL,
        .mmap = NULL,
        .create = s5fs_create,
        .mknod = s5fs_mknod,
        .lookup = s5fs_lookup,
        .link = s5fs_link,
        .unlink = s5fs_unlink,
        .mkdir = s5fs_mkdir,
        .rmdir = s5fs_rmdir,
        .readdir = s5fs_readdir,
        .stat = s5fs_stat,
        .fillpage = s5fs_fillpage,
        .dirtypage = s5fs_dirtypage,
        .cleanpage = s5fs_cleanpage
};

/* vnode operations table for regular files: */
static vnode_ops_t s5fs_file_vops = {
        .read = s5fs_read,
        .write = s5fs_write,
        .mmap = s5fs_mmap,
        .create = NULL,
        .mknod = NULL,
        .lookup = NULL,
        .link = NULL,
        .unlink = NULL,
        .mkdir = NULL,
        .rmdir = NULL,
        .readdir = NULL,
        .stat = s5fs_stat,
        .fillpage = s5fs_fillpage,
        .dirtypage = s5fs_dirtypage,
        .cleanpage = s5fs_cleanpage
};

/*
 * Read fs->fs_dev and set fs_op, fs_root, and fs_i.
 *
 * Point fs->fs_i to an s5fs_t*, and initialize it.  Be sure to
 * verify the superblock (using s5_check_super()).  Use vget() to get
 * the root vnode for fs_root.
 *
 * Return 0 on success, negative on failure.
 */
int
s5fs_mount(struct fs *fs)
{
        int num;
        blockdev_t *dev;
        s5fs_t *s5;
        pframe_t *vp;

        KASSERT(fs);

        if (sscanf(fs->fs_dev, "disk%d", &num) != 1) {
                return -EINVAL;
        }

        if (!(dev = blockdev_lookup(MKDEVID(1, num)))) {
                return -EINVAL;
        }

        /* allocate and initialize an s5fs_t: */
        s5 = (s5fs_t *)kmalloc(sizeof(s5fs_t));

        if (!s5)
                return -ENOMEM;

        /*     init s5f_disk: */
        s5->s5f_bdev  = dev;

        /*     init s5f_super: */
        pframe_get(S5FS_TO_VMOBJ(s5), S5_SUPER_BLOCK, &vp);

        KASSERT(vp);

        s5->s5f_super = (s5_super_t *)(vp->pf_addr);

        if (s5_check_super(s5->s5f_super)) {
                /* corrupt */
                kfree(s5);
                return -EINVAL;
        }

        pframe_pin(vp);

        /*     init s5f_mutex: */
        kmutex_init(&s5->s5f_mutex);

        /*     init s5f_fs: */
        s5->s5f_fs = fs;


        /* Init the members of fs that we (the fs-implementation) are
         * responsible for initializing: */
        fs->fs_i = s5;
        fs->fs_op = &s5fs_fsops;
        fs->fs_root = vget(fs, s5->s5f_super->s5s_root_inode);

        return 0;
}

/* Implementation of fs_t entry points: */

/*
 * MACROS
 *
 * There are several macros which we have defined for you that
 * will make your life easier. Go find them, and use them.
 * Hint: Check out s5fs(_subr).h
 */


/*
 * See the comment in vfs.h for what is expected of this function.
 *
 * When this function returns, the inode link count should be incremented.
 * Note that most UNIX filesystems don't do this, they have a separate
 * flag to indicate that the VFS is using a file. However, this is
 * simpler to implement.
 *
 * To get the inode you need to use pframe_get then use the pf_addr
 * and the S5_INODE_OFFSET(vnode) to get the inode
 *
 * Don't forget to update linkcounts and pin the page.
 *
 * Note that the devid is stored in the indirect_block in the case of
 * a char or block device
 *
 * Finally, the main idea is to do special initialization based on the
 * type of inode (i.e. regular, directory, char/block device, etc').
 *
 */
static void
s5fs_read_vnode(vnode_t *vnode)
{
        /*
         * This is called by vget.
         *
         * read_vnode will be passed a vnode_t*, which will have its vn_fs
         * and vn_vno fields initialized.
         *
         * read_vnode must initialize the following members of the provided
         * vnode_t:
         *     vn_ops
         *     vn_mode
         *         and vn_devid if appropriate
         *     vn_len
         *     vn_i
         *
         * This entry point is ALLOWED TO BLOCK.
         */

        /* Call pframe_get() on memory object associated with file system as a whole */
        mmobj_t *o = S5FS_TO_VMOBJ(VNODE_TO_S5FS(vnode));
        pframe_t *pf;
        int res = pframe_get(o, S5_INODE_BLOCK(vnode->vn_vno), &pf);
        if (res != 0) panic("ERROR in pframe_get() from read_vnode()\n");

        /* Pin page frame. */
        pframe_pin(pf);

        /* pframe_get() returned a block of inodes. Isolate the one you're looking for. */
        s5_inode_t *inode = (s5_inode_t *) pf->pf_addr + S5_INODE_OFFSET(vnode->vn_vno);
        vnode->vn_i = inode;

        /*if (inode->s5_linkcount == 1)*/ inode->s5_linkcount++;

        /* Set vn_len */
        vnode->vn_len = inode->s5_size;

        /* Check inode->s5_type to set the rest of the fields */
        if (inode->s5_type == S5_TYPE_DATA) {
            vnode->vn_mode = S_IFREG;
            vnode->vn_ops = &s5fs_file_vops;
        }
        else if (inode->s5_type == S5_TYPE_DIR) {
            vnode->vn_mode = S_IFDIR;
            vnode->vn_ops = &s5fs_dir_vops;
        }
        else if (inode->s5_type == S5_TYPE_CHR) {
            vnode->vn_mode = S_IFCHR;
            vnode->vn_devid = inode->s5_indirect_block;
            vnode->vn_cdev = bytedev_lookup(vnode->vn_devid);
            vnode->vn_ops = (struct vnode_ops *) vnode->vn_cdev->cd_ops;
        }
        else if (inode->s5_type == S5_TYPE_BLK) {
            vnode->vn_mode = S_IFBLK;
            vnode->vn_devid = inode->s5_indirect_block;
            vnode->vn_bdev = blockdev_lookup(vnode->vn_devid);
            vnode->vn_ops = (struct vnode_ops *) vnode->vn_bdev->bd_ops;
        }
        else panic("ERROR in read_vnode(). inode type should not be free.\n");

        return;

        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_read_vnode"); */
}

/*
 * See the comment in vfs.h for what is expected of this function.
 *
 * When this function returns, the inode refcount should be decremented.
 *
 * You probably want to use s5_free_inode() if there are no more links to
 * the inode, and dont forget to unpin the page
 */
static void
s5fs_delete_vnode(vnode_t *vnode)
{
        /*
         * The inverse of read_vnode; delete_vnode is called by vput when the
         * specified vnode_t no longer needs to exist (it is neither actively
         * nor passively referenced).
         *
         * This entry point is ALLOWED TO BLOCK.
         */

        pframe_t *pf;
        int res = pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(vnode)), S5_INODE_BLOCK(vnode->vn_vno), &pf);

        VNODE_TO_S5INODE(vnode)->s5_linkcount--;
        if (VNODE_TO_S5INODE(vnode)->s5_linkcount == 0) {s5_free_inode(vnode);}

        pframe_unpin(pf); 

        NOT_YET_IMPLEMENTED("S5FS: s5fs_delete_vnode");
}

/*
 * See the comment in vfs.h for what is expected of this function.
 *
 * The vnode still exists on disk if it has a linkcount greater than 1.
 * (Remember, VFS takes a reference on the inode as long as it uses it.)
 *
 */
static int
s5fs_query_vnode(vnode_t *vnode)
{
        /*
         * Returns 1 if the vnode still exists in the filesystem, 0 of it can
         * be deleted. Called by vput when there are no active references to
         * the vnode. If query_vnode returns 0, vput evicts all pages of the vnode
         * from memory so that it can be deleted.
         */

        s5_inode_t *inode = (s5_inode_t *) vnode->vn_i;
        if ( inode->s5_linkcount > 1 ) return 1;
        return 0;

        NOT_YET_IMPLEMENTED("S5FS: s5fs_query_vnode");
        return 0;
}

/*
 * s5fs_check_refcounts()
 * vput root vnode
 */
static int
s5fs_umount(fs_t *fs)
{
        s5fs_t *s5 = (s5fs_t *)fs->fs_i;
        blockdev_t *bd = s5->s5f_bdev;
        pframe_t *sbp;
        int ret;

        if (s5fs_check_refcounts(fs)) {
                dbg(DBG_PRINT, "s5fs_umount: WARNING: linkcount corruption "
                    "discovered in fs on block device with major %d "
                    "and minor %d!!\n", MAJOR(bd->bd_id), MINOR(bd->bd_id));
        }
        if (s5_check_super(s5->s5f_super)) {
                dbg(DBG_PRINT, "s5fs_umount: WARNING: corrupted superblock "
                    "discovered on fs on block device with major %d "
                    "and minor %d!!\n", MAJOR(bd->bd_id), MINOR(bd->bd_id));
        }

        vnode_flush_all(fs);

        vput(fs->fs_root);
	dbg(DBG_PRINT, "s5fs_umount: Free data blocks %d\n", 
		s5fs_free_blocks(fs));

        if (0 > (ret = pframe_get(S5FS_TO_VMOBJ(s5), S5_SUPER_BLOCK, &sbp))) {
                panic("s5fs_umount: failed to pframe_get super block. "
                      "This should never happen (the page should already "
                      "be resident and pinned, and even if it wasn't, "
                      "block device readpage entry point does not "
                      "fail.\n");
        }

        KASSERT(sbp);

        pframe_unpin(sbp);

        kfree(s5);

        blockdev_flush_all(bd);

        return 0;
}




/* Implementation of vnode_t entry points: */

/*
 * Unless otherwise mentioned, these functions should leave all refcounts net
 * unchanged.
 */

/*
 * You will need to lock the vnode's mutex before doing anything that can block.
 * pframe functions can block, so probably what you want to do
 * is just lock the mutex in the s5fs_* functions listed below, and then not
 * worry about the mutexes in s5fs_subr.c.
 *
 * Note that you will not be calling pframe functions directly, but
 * s5fs_subr.c functions will be, so you need to lock around them.
 *
 * DO NOT TRY to do fine grained locking your first time through,
 * as it will break, and you will cry.
 *
 * Finally, you should read and understand the basic overview of
 * the s5fs_subr functions. All of the following functions might delegate,
 * and it will make your life easier if you know what is going on.
 */


/* Simply call s5_read_file. */
static int
s5fs_read(vnode_t *vnode, off_t offset, void *buf, size_t len)
{
        kmutex_lock(&vnode->vn_mutex);
        int res = s5_read_file(vnode, offset, buf, len);
        kmutex_unlock(&vnode->vn_mutex);
        return res;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_read"); */
        return -1;
}

/* Simply call s5_write_file. */
static int
s5fs_write(vnode_t *vnode, off_t offset, const void *buf, size_t len)
{
        kmutex_lock(&vnode->vn_mutex);
        int res = s5_write_file(vnode, offset, buf, len);
        kmutex_unlock(&vnode->vn_mutex);
        return res;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_write"); */
        return -1;
}

/* This function is deceptivly simple, just return the vnode's
 * mmobj_t through the ret variable. Remember to watch the
 * refcount.
 *
 * Don't worry about this until VM.
 */
static int
s5fs_mmap(vnode_t *file, vmarea_t *vma, mmobj_t **ret)
{
        /* NOT_YET_IMPLEMENTED("VM: s5fs_mmap"); */

        return 0;
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * When this function returns, the inode refcount of the file should be 2
 * and the vnode refcount should be 1.
 *
 * You probably want to use s5_alloc_inode(), s5_link(), and vget().
 */
static int
s5fs_create(vnode_t *dir, const char *name, size_t namelen, vnode_t **result)
{
/* Allocate an inode (pull one from disk and pin it)
 * Add a directory entry
 * Dirty any modified pages
 */
/* Called by open_namev()
   vget() a new vnode, create an entry for it in dir of specified name
*/

/* Like mkdir and mknod, but creating a regular data file. 
   Just change the type you pass in for alloc_inode()(?)
 	*/

        int inode_num = s5_alloc_inode(dir->vn_fs, S5_TYPE_DATA, 0); /* Pull new inode from disk */

        pframe_t *pf;
        int res = pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(dir)), S5_INODE_BLOCK(inode_num), &pf);
        vnode_t *new_file = vget(dir->vn_fs, inode_num); /* vget will pin the page this inode is in */

        /* link new directory to current directory and parent directory */
        int link1 = s5_link(dir, new_file, name, namelen); /* Create dirent for new_dir in parent dir */

        if (link1 != 0) {
            dbg_print("ERROR. File %s already exists.\n", name);
            return -EEXIST;
        }

        *result = new_file;

        /* vput(dir); */


        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_create"); */
        return -1;
}


/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * This function is similar to s5fs_create, but it creates a special
 * file specified by 'devid'.
 *
 * You probably want to use s5_alloc_inode, s5_link(), vget(), and vput().
 */
static int
s5fs_mknod(vnode_t *dir, const char *name, size_t namelen, int mode, devid_t devid)
{
        dbg_print("Made it into mknod(). Making %s\n", name);
        int type;
        if (S_ISCHR(mode)) type = S5_TYPE_CHR;
        else if (S_ISBLK(mode)) type = S5_TYPE_BLK;
        else return -1; /* Should not happen. This means mode passed in was not CHR or BLK. */
        dbg_print("type %d\n", type);
        int inode_num = s5_alloc_inode(dir->vn_fs, type, devid);

        pframe_t *pf;
        int res = pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(dir)), S5_INODE_BLOCK(inode_num), &pf);
        vnode_t *new_dev = vget(dir->vn_fs, inode_num);

        /* link new directory to current directory and parent directory */
        int link1 = s5fs_link(dir, new_dev, name, namelen); /* Create dirent for new_dir in parent dir */

        if (link1 != 0) {
            dbg_print("ERROR. File %s already exists.\n", name);
            return -EEXIST;
        }

        vput(new_dev);

        dbg_print("new dev linkcount: %d\n", VNODE_TO_S5INODE(new_dev)->s5_linkcount);
        return 0;

        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_mknod"); */
        return -1;

/*
        int inode_num = s5_alloc_inode(dir->vn_fs, S5_TYPE_DIR, 0);

        pframe_t *pf;
        int res = pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(dir)), S5_INODE_BLOCK(inode_num), &pf);
        vnode_t *new_dir = vget(dir->vn_fs, inode_num);

         link new directory to current directory and parent directory 
        int link1 = s5fs_link(dir, new_dir, name, namelen);  Create dirent for new_dir in parent dir 
        int link2 = s5fs_link(new_dir, new_dir, ".", 1); 
        int link3 = s5fs_link(new_dir, dir, "..", 2); 

        if (link1 != 0) {
            dbg_print("ERROR. File %s already exists.\n", name);
            return -EEXIST;
        }

        vput(new_dir);

        VNODE_TO_S5INODE(new_dir)->s5_linkcount = 1;
*/
}

/*
 * lookup sets *result to the vnode in dir with the specified name.

 * You probably want to use s5_find_dirent() and vget().
 */
int
s5fs_lookup(vnode_t *base, const char *name, size_t namelen, vnode_t **result)
{
/* If name is in the directory, return the vnode associated with it */

         /* int s5_find_dirent(vnode_t *vnode, const char *name, size_t namelen) */
/*
 * Locate the directory entry in the given inode with the given name,
 * and return its inode number. If there is no entry with the given
 * name, return -ENOENT.
 *
 * You'll probably want to use s5_read_file and name_match
 *
 * You can either read one dirent at a time or optimize and read more.
 * Either is fine.
 */
         dbg_print("Looking up %s\n", name);
         kmutex_lock(&base->vn_mutex);
         int inum = s5_find_dirent(base, name, namelen);
         kmutex_unlock(&base->vn_mutex);
         if (inum == -ENOENT) {
             dbg_print("ERROR. No dir entry with given name.\n");
             return inum;
         }
         /* If you get here, dir entry has matching name. inum is its inode number. */


         /* struct vnode *vget(struct fs *fs, ino_t vnum); */
/*
 *     Obtain a vnode representing the file that filesystem 'fs' identifies
 *     by inode number 'vnum'; returns the vnode_t corresponding to the
 *     given filesystem and vnode number.  If a vnode for the given file
 *     already exists (it already has an entry in the system inode table) then
 *     the reference count of that vnode is incremented and it is returned.
 *     Otherwise a new vnode is created in the system inode table with a
 *     reference count of 1.
 *     This function has no unsuccessful return.
 *
 *     MAY BLOCK.
 */
        kmutex_lock(&base->vn_mutex);
        *result = vget(base->vn_fs, inum);
        kmutex_unlock(&base->vn_mutex);
        return 0;


        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_lookup"); */
        return -1;
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * When this function returns, the inode refcount of the linked file
 * should be incremented.
 *
 * You probably want to use s5_link().
 */
static int
s5fs_link(vnode_t *src, vnode_t *dir, const char *name, size_t namelen)
{
        /* Add a directory entry linking to the vnode
         * Dirty file blocks (and maybe the inode)
         */
        kmutex_lock(&src->vn_mutex);
        int res = s5_link(src, dir, name, namelen);
        kmutex_unlock(&src->vn_mutex);
        return res;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_link"); */
        return -1;
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * When this function returns, the inode refcount of the unlinked file
 * should be decremented.
 *
 * You probably want to use s5_remove_dirent().
 */
static int
s5fs_unlink(vnode_t *dir, const char *name, size_t namelen)
{
        /* Remove the directory entry linking to the vnode
         * Reduce link count
         * Dirty file blocks (and maybe the inode)
         */
        kmutex_lock(&dir->vn_mutex);
        int res = s5_remove_dirent(dir, name, namelen);
        kmutex_unlock(&dir->vn_mutex);
        return res;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_unlink"); */
        return -1;
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * You need to create the "." and ".." directory entries in the new
 * directory. These are simply links to the new directory and its
 * parent.
 *
 * When this function returns, the linkcount on the parent should
 * be incremented, and the linkcount on the new directory should be
 * 1. It might make more sense for the linkcount on the new
 * directory to be 2 (since "." refers to it as well as its entry in the
 * parent dir), but convention is that empty directories have only 1
 * link.
 *
 * You probably want to use s5_alloc_inode, and s5_link().
 *
 * Assert, a lot.
 */
static int
s5fs_mkdir(vnode_t *dir, const char *name, size_t namelen)
{
/* Create a directory called name in dir */

/* Allocate an inode (pull one from disk and pin it)
 * Add directory entries (new dir, ., ..)
 * Get link counts right
 * Make modified pages dirty
 */


        /* int s5_alloc_inode(fs_t *fs, uint16_t type, devid_t devid) */
/*
 * Creates a new inode from the free list and initializes its fields.
 * Uses S5_INODE_BLOCK to get the page from which to create the inode
 *
 * This function may block.
 */

        /* int s5_link(vnode_t *parent, vnode_t *child, const char *name, size_t namelen) */
/*
 * Create a new directory entry in directory 'parent' with the given name, which
 * refers to the same file as 'child'.
 *
 * When this function returns, the inode refcount on the file that was linked to
 * should be incremented.
 *
 * Remember to incrament the ref counts appropriately
 *
 * You probably want to use s5_find_dirent(), s5_write_file(), and s5_dirty_inode().
 */

        int inode_num = s5_alloc_inode(dir->vn_fs, S5_TYPE_DIR, 0);

        pframe_t *pf;
        int res = pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(dir)), S5_INODE_BLOCK(inode_num), &pf);
        vnode_t *new_dir = vget(dir->vn_fs, inode_num);
        /*dbg_print("1 Child %s link count is %d\n", name, VNODE_TO_S5INODE(new_dir)->s5_linkcount);
        dbg_print("  Child %s ref count is %d\n", name, new_dir->vn_refcount);*/

        if (VNODE_TO_S5INODE(new_dir)->s5_linkcount != 1) { /* Only one linking to you is from the vget */
            dbg_print("ERROR. File %s already exists.\n", name);
            return -EEXIST;
        }

        /* link new directory to current directory and parent directory */
        int link1 = s5fs_link(dir, new_dir, name, namelen); /* Create dirent for new_dir in parent dir */
        /*dbg_print("2 Child %s link count is %d\n", name, VNODE_TO_S5INODE(new_dir)->s5_linkcount);
        dbg_print("  Child %s ref count is %d\n", name, new_dir->vn_refcount);*/
        int link2 = s5fs_link(new_dir, new_dir, ".", 1); /* Create . dirent in new_dir */
        VNODE_TO_S5INODE(new_dir)->s5_linkcount--; /* Don't want the link to itself to count */
        /*dbg_print("3 Child %s link count is %d\n", name, VNODE_TO_S5INODE(new_dir)->s5_linkcount);
        dbg_print("  Child %s ref count is %d\n", name, new_dir->vn_refcount);*/
        int link3 = s5fs_link(new_dir, dir, "..", 2); /* Create .. dirent in new_dir */
        /*dbg_print("4 Child %s link count is %d\n", name, VNODE_TO_S5INODE(new_dir)->s5_linkcount);
        dbg_print("  Child %s ref count is %d\n", name, new_dir->vn_refcount);*/

        vput(new_dir);
        /*dbg_print("Child %s link count is %d\n", name, VNODE_TO_S5INODE(new_dir)->s5_linkcount);
        dbg_print("  Child %s ref count is %d\n", name, new_dir->vn_refcount);*/

        return 0;

/*
pframe_get for that inode. mmobj of dir (maybe on fs?). pagenum is S5_INODE_BLOCK(inode).
this pins pageframe for you
need to get vnode for child. use vget. use parent directory's file system and new inode number.
link will increment the link count for the child.
link to current directory and parent directory.
after linking, need to set link count to one (not two)
*/


        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_mkdir"); */
        return -1;
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * When this function returns, the linkcount on the parent should be
 * decremented (since ".." in the removed directory no longer references
 * it). Remember that the directory must be empty (except for "." and
 * "..").
 *
 * You probably want to use s5_find_dirent() and s5_remove_dirent().
 */
static int
s5fs_rmdir(vnode_t *parent, const char *name, size_t namelen)
{
/* Removes directory name from parent
 * Directory to be removed must be empty (except for . and ..)
 */

         /* int s5_find_dirent(vnode_t *vnode, const char *name, size_t namelen) */
/*
 * Locate the directory entry in the given inode with the given name,
 * and return its inode number. If there is no entry with the given
 * name, return -ENOENT.
 *
 * You'll probably want to use s5_read_file and name_match
 *
 * You can either read one dirent at a time or optimize and read more.
 * Either is fine.
 */

         /* int s5_remove_dirent(vnode_t *vnode, const char *name, size_t namelen) */
/*
 * Locate the directory entry in the given inode with the given name,
 * and delete it. If there is no entry with the given name, return
 * -ENOENT.
 *
 * In order to ensure that the directory entries are contiguous in the
 * directory file, you will need to move the last directory entry into
 * the remove dirent's place.
 *
 * When this function returns, the inode refcount on the removed file
 * should be decremented.
 *
 * It would be a nice extension to free blocks from the end of the
 * directory file which are no longer needed.
 *
 * Don't forget to dirty appropriate blocks!
 *
 * You probably want to use vget(), vput(), s5_read_file(),
 * s5_write_file(), and s5_dirty_inode().
 */

        NOT_YET_IMPLEMENTED("S5FS: s5fs_rmdir");
        return -1;
}


/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * Here you need to use s5_read_file() to read a s5_dirent_t from a directory
 * and copy that data into the given dirent. The value of d_off is dependent on
 * your implementation and may or may not b e necessary.  Finally, return the
 * number of bytes read.
 */
static int
s5fs_readdir(vnode_t *vnode, off_t offset, struct dirent *d)
{
        /*
         * readdir reads one directory entry from the dir into the struct
         * dirent. On success, it returns the amount that offset should be
         * increased by to obtain the next directory entry with a
         * subsequent call to readdir. If the end of the file as been
         * reached (offset == file->vn_len), no directory entry will be
         * read and 0 will be returned.
         */

        /* int s5_read_file(struct vnode *vnode, off_t seek, char *dest, size_t len) */
/*
 * Read up to len bytes from the given inode, starting at seek bytes
 * from the beginning of the inode. On success, return the number of
 * bytes actually read, or 0 if the end of the file has been reached; on
 * failure, return -errno.
 */
        if (offset == vnode->vn_len) return 0; /* Don't read dirent if EOF reached */

        s5_dirent_t dirent_read;
        int blocks_read = s5_read_file(vnode, offset, (char *)&dirent_read, sizeof(s5_dirent_t));


/* typedef struct dirent {
        ino_t   d_ino;                  
        off_t   d_off;                  seek pointer of next entry
        char    d_name[NAME_LEN + 1];   
} dirent_t; 
typedef struct s5_dirent {
        uint32_t   s5d_inode;
        char       s5d_name[S5_NAME_LEN];
} s5_dirent_t; */

        /* Set fields in d based on what you read into dirent_read */
        d->d_ino = dirent_read.s5d_inode;
        d->d_off = offset+blocks_read;
        strncpy(d->d_name, dirent_read.s5d_name, S5_NAME_LEN-1);
        return blocks_read;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_readdir");*/
        return -1;
}


/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * Don't worry if you don't know what some of the fields in struct stat
 * mean. The ones you should be sure to set are st_mode, st_ino,
 * st_nlink, st_size, st_blksize, and st_blocks.
 *
 * You probably want to use s5_inode_blocks().
 */
static int
s5fs_stat(vnode_t *vnode, struct stat *ss)
{
        s5_inode_t *i = VNODE_TO_S5INODE(vnode);

        ss->st_mode = vnode->vn_mode;
        ss->st_ino = vnode->vn_vno;
        ss->st_nlink = VNODE_TO_S5INODE(vnode)->s5_linkcount;
        ss->st_size = vnode->vn_len;
        ss->st_blksize = S5_BLOCK_SIZE;
        kmutex_lock(&vnode->vn_mutex);
        ss->st_blocks = s5_inode_blocks(vnode);
        kmutex_unlock(&vnode->vn_mutex);

        return 0;

        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_stat"); */
        return -1;
}


/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * You'll probably want to use s5_seek_to_block and the device's
 * read_block function.
 */
static int
s5fs_fillpage(vnode_t *vnode, off_t offset, void *pagebuf)
{
        /* vnode = node to read
         * offset = where to start reading
         * pagebuf = page-sized destination
         */

        /*
         * Used by vnode vm_object entry points (and by no one else):
         */
        /*
         * Read the page of 'vnode' containing 'offset' into the
         * page-aligned and page-sized buffer pointed to by
         * 'pagebuf'.
         */
        /* return 0 on success, and -errno on failure.
         */

        /*
         *  Find disk block with data (s5_seek_to_block())
         *  If there is a disk block w/ data, copy it out from disk (read_block)
         *  Else, copy zeroes out
         */
        /* kmutex_lock(&vnode->vn_mutex);*/
        int loc = s5_seek_to_block(vnode, offset, 0);
        /* kmutex_unlock(&vnode->vn_mutex);*/

        if (loc == 0) { /* If no disk block with data, copy out zeroes */
            memset(pagebuf, 0, S5_BLOCK_SIZE);
            return 0;
        }

        /* If you get here, there is a disk block with data */
        /* return vnode->vn_bdev->bd_ops->read_block(vnode->vn_bdev, (char *)pagebuf, loc, 1); */
        blockdev_t *bdev = FS_TO_S5FS(vnode->vn_fs)->s5f_bdev;
        /* kmutex_lock(&vnode->vn_mutex);*/
        int block = bdev->bd_ops->read_block(bdev, pagebuf, loc, S5_BLOCK_SIZE); /* read_block() will block */
        /* kmutex_unlock(&vnode->vn_mutex);*/
        return block;

        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_fillpage"); */
        return -1;
}


/*
 * if this offset is NOT within a sparse region of the file
 *     return 0;
 *
 * attempt to make the region containing this offset no longer
 * sparse
 *     - attempt to allocate a free block
 *     - if no free blocks available, return -ENOSPC
 *     - associate this block with the inode; alter the inode as
 *       appropriate
 *         - dirty the page containing this inode
 *
 * Much of this can be done with s5_seek_to_block()
 */
static int
s5fs_dirtypage(vnode_t *vnode, off_t offset)
{
        /* If no space allocated in inode, get some (b/c that page will eventually be written)
         *   s5_alloc_block()
         *   when you manipulate inode, use s5_dirty_inode() to tell paging system
         */
        NOT_YET_IMPLEMENTED("S5FS: s5fs_dirtypage");
        return -1;
}

/*
 * Like fillpage, but for writing.
 */
static int
s5fs_cleanpage(vnode_t *vnode, off_t offset, void *pagebuf)
{
        /* vnode = node to write
         * offset = where to start writing
         * pagebuf = page-sized source
         */

        /* Find destination disk block (s5_seek_to_block())
         * Copy data out to disk (write_block)
         */

        NOT_YET_IMPLEMENTED("S5FS: s5fs_cleanpage");
        return -1;
}

/* Diagnostic/Utility: */

/*
 * verify the superblock.
 * returns -1 if the superblock is corrupt, 0 if it is OK.
 */
static int
s5_check_super(s5_super_t *super)
{
        if (!(super->s5s_magic == S5_MAGIC
              && (super->s5s_free_inode < super->s5s_num_inodes
                  || super->s5s_free_inode == (uint32_t) - 1)
              && super->s5s_root_inode < super->s5s_num_inodes))
                return -1;
        if (super->s5s_version != S5_CURRENT_VERSION) {
                dbg(DBG_PRINT, "Filesystem is version %d; "
                    "only version %d is supported.\n",
                    super->s5s_version, S5_CURRENT_VERSION);
                return -1;
        }
        return 0;
}

static void
calculate_refcounts(int *counts, vnode_t *vnode)
{
        int ret;

        counts[vnode->vn_vno]++;
        dbg(DBG_S5FS, "calculate_refcounts: Incrementing count of inode %d to"
            " %d\n", vnode->vn_vno, counts[vnode->vn_vno]);
        /*
         * We only consider the children of this directory if this is the
         * first time we have seen it.  Otherwise, we would recurse forever.
         */
        if (counts[vnode->vn_vno] == 1 && S_ISDIR(vnode->vn_mode)) {
                int offset = 0;
                struct dirent d;
                vnode_t *child;

                while (0 < (ret = s5fs_readdir(vnode, offset, &d))) {
                        /*
                         * We don't count '.', because we don't increment the
                         * refcount for this (an empty directory only has a
                         * link count of 1).
                         */
                        if (0 != strcmp(d.d_name, ".")) {
                                child = vget(vnode->vn_fs, d.d_ino);
                                calculate_refcounts(counts, child);
                                vput(child);
                        }
                        offset += ret;
                }

                KASSERT(ret == 0);
        }
}

/*
 * This will check the refcounts for the filesystem.  It will ensure that that
 * the expected number of refcounts will equal the actual number.  To do this,
 * we have to create a data structure to hold the counts of all the expected
 * refcounts, and then walk the fs to calculate them.
 */
int
s5fs_check_refcounts(fs_t *fs)
{
        s5fs_t *s5fs = (s5fs_t *)fs->fs_i;
        int *refcounts;
        int ret = 0;
        uint32_t i;

        refcounts = kmalloc(s5fs->s5f_super->s5s_num_inodes * sizeof(int));
        KASSERT(refcounts);
        memset(refcounts, 0, s5fs->s5f_super->s5s_num_inodes * sizeof(int));

        calculate_refcounts(refcounts, fs->fs_root);
        --refcounts[fs->fs_root->vn_vno]; /* the call on the preceding line
                                           * caused this to be incremented
                                           * not because another fs link to
                                           * it was discovered */

        dbg(DBG_PRINT, "Checking refcounts of s5fs filesystem on block "
            "device with major %d, minor %d\n",
            MAJOR(s5fs->s5f_bdev->bd_id), MINOR(s5fs->s5f_bdev->bd_id));

        for (i = 0; i < s5fs->s5f_super->s5s_num_inodes; i++) {
                vnode_t *vn;

                if (!refcounts[i]) continue;

                vn = vget(fs, i);
                KASSERT(vn);

                if (refcounts[i] != VNODE_TO_S5INODE(vn)->s5_linkcount - 1) {
                        dbg(DBG_PRINT, "   Inode %d, expecting %d, found %d\n", i,
                            refcounts[i], VNODE_TO_S5INODE(vn)->s5_linkcount - 1);
                        ret = -1;
                }
                vput(vn);
        }

        dbg(DBG_PRINT, "Refcount check of s5fs filesystem on block "
            "device with major %d, minor %d completed %s.\n",
            MAJOR(s5fs->s5f_bdev->bd_id), MINOR(s5fs->s5f_bdev->bd_id),
            (ret ? "UNSUCCESSFULLY" : "successfully"));

        kfree(refcounts);
        return ret;
}
/* Count free blocks in the system */
int
s5fs_free_blocks(fs_t *fs) {
    s5fs_t *s5fs = FS_TO_S5FS(fs);
    mmobj_t *disk = S5FS_TO_VMOBJ(s5fs);
    s5_super_t *su = s5fs->s5f_super;
    pframe_t *pf = NULL;
    uint32_t *freel = NULL;
    int tot = 0;

    kmutex_lock(&s5fs->s5f_mutex);
    tot = su->s5s_nfree;
    freel = su->s5s_free_blocks;
    if ( freel[S5_NBLKS_PER_FNODE-1] !=  ~0u) tot += 1;
    while ( freel[S5_NBLKS_PER_FNODE-1] !=  ~0u) {
	tot += S5_NBLKS_PER_FNODE;
	pframe_get(disk, freel[S5_NBLKS_PER_FNODE-1], &pf);
	KASSERT(pf);
	freel = pf->pf_addr;
	dbg(DBG_S5FS, "next pointer %x\n", freel[S5_NBLKS_PER_FNODE-1]);
	/* Subtract out the last null */
	if ( !freel[S5_NBLKS_PER_FNODE-1]!= ~0) tot--;
    }
    kmutex_unlock(&s5fs->s5f_mutex);
    return tot;
}
