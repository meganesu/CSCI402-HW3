/*
 *   FILE: s5fs_subr.c
 * AUTHOR: afenn
 *  DESCR:
 *  $Id: s5fs_subr.c,v 1.1.2.1 2006/06/04 01:02:15 afenn Exp $
 */

#include "kernel.h"
#include "util/debug.h"
#include "mm/kmalloc.h"
#include "globals.h"
#include "proc/sched.h"
#include "proc/kmutex.h"
#include "errno.h"
#include "util/string.h"
#include "util/printf.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "drivers/dev.h"
#include "drivers/blockdev.h"
#include "fs/stat.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/s5fs/s5fs_subr.h"
#include "fs/s5fs/s5fs.h"
#include "mm/mm.h"
#include "mm/page.h"

#define dprintf(...) dbg(DBG_S5FS, __VA_ARGS__)

#define s5_dirty_super(fs)                                           \
        do {                                                         \
                pframe_t *p;                                         \
                int err;                                             \
                pframe_get(S5FS_TO_VMOBJ(fs), S5_SUPER_BLOCK, &p);   \
                KASSERT(p);                                          \
                err = pframe_dirty(p);                               \
                KASSERT(!err                                         \
                        && "shouldn\'t fail for a page belonging "   \
                        "to a block device");                        \
        } while (0)


static void s5_free_block(s5fs_t *fs, int block);
static int s5_alloc_block(s5fs_t *);


/*
 * Return the disk-block number for the given seek pointer (aka file
 * position).
 *
 * If the seek pointer refers to a sparse block, and alloc is false,
 * then return 0. If the seek pointer refers to a sparse block, and
 * alloc is true, then allocate a new disk block (and make the inode
 * point to it) and return it.
 *
 * Be sure to handle indirect blocks!
 *
 * If there is an error, return -errno.
 *
 * You probably want to use pframe_get, pframe_pin, pframe_unpin, pframe_dirty.
 */
int
s5_seek_to_block(vnode_t *vnode, off_t seekptr, int alloc)
{
        /* seekptr defines which byte in a file you're looking at.
             it's an integer. not yet in terms of direct blocks or
             indirect block entries. use S5_DATA_BLOCK() for that.
         */
        /* This function should take that seekptr and translate it to the actual
             disk address where the contents of that block of the file lives.
         */

        s5_inode_t *inode = VNODE_TO_S5INODE(vnode);

        /* Make sure offset is within file length
             i.e. not trying to read direct block that isn't used by file */

        int disk_block_addr;

        /* Figure out if you're accessing a direct block or an indirect block */
        if (S5_DATA_BLOCK(seekptr) > S5_NDIRECT_BLOCKS-1) {
          /* Indirect block field in inode corresponds to a disk block number
           *   where the indirect block data lives
           */
          /* Check to see if indirect block has been allocated */
          if ((inode->s5_indirect_block == 0) && (alloc == 1)) {
              /* DO STUFF IN HERE TO ALLOCATE INDIRECT BLOCK */
              int indir_block = s5_alloc_block(VNODE_TO_S5FS(vnode));
              if (indir_block < 0) return indir_block;

              /* Set newly allocated indirect block into inode info */
              inode->s5_indirect_block = indir_block;
          }

          pframe_t *pf;
          pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(vnode)), inode->s5_indirect_block, &pf);
          pframe_pin(pf);

          off_t indirect_block_index = S5_DATA_BLOCK(seekptr) - S5_NDIRECT_BLOCKS;
          char *iblock_entry_addr = (char *) pf->pf_addr + (sizeof(int) * indirect_block_index);

          /* Copy whatever lives at address stored in indirect block entry */
          memcpy((void *) &disk_block_addr, (void *) iblock_entry_addr, sizeof(int));

          /* If sparse and alloc == 1, allocate new block */
          if ((disk_block_addr == 0) && (alloc == 1)) {
              disk_block_addr = s5_alloc_block(VNODE_TO_S5FS(vnode));
              if (disk_block_addr < 0) {
                  pframe_unpin(pf);
                  return disk_block_addr; /* Return error code if alloc_block() failed */
              }

              /* If you get here, disk_block_addr returned valid page number,
               *   set entry in indirect block appropriately.
               */
              memcpy((void *) iblock_entry_addr, (void *) &disk_block_addr, sizeof(int));

              /* Dirty indirect block pframe, since you just wrote to it */
              /* NOTE: In this case, inode contents don't change (same pointer to indirect block),
               *       so don't need to dirty inode.
               */
              pframe_dirty(pf);
          }

          pframe_unpin(pf);

          return disk_block_addr;
        }

        /* If accessing a direct block */
        disk_block_addr = inode->s5_direct_blocks[S5_DATA_BLOCK(seekptr)];

        /* If sparse block and alloc is true, use alloc_block() */
        if ((disk_block_addr == 0) && (alloc == 1)) {
            disk_block_addr = s5_alloc_block(VNODE_TO_S5FS(vnode));
            if (disk_block_addr == -ENOSPC) return disk_block_addr; /* Return error code if alloc_block() failed */

            /* If you get here, disk_block_addr returned valid page number,
             *   set direct block entry appropriately.
             */
            inode->s5_direct_blocks[S5_DATA_BLOCK(seekptr)] = disk_block_addr;

            /* Dirty inode, since you just wrote to it */
            s5_dirty_inode(VNODE_TO_S5FS(vnode), inode);
        }

        return disk_block_addr;


        NOT_YET_IMPLEMENTED("S5FS: s5_seek_to_block");
        return -1;
}


/*
 * Locks the mutex for the whole file system
 */
static void
lock_s5(s5fs_t *fs)
{
        kmutex_lock(&fs->s5f_mutex);
}

/*
 * Unlocks the mutex for the whole file system
 */
static void
unlock_s5(s5fs_t *fs)
{
        kmutex_unlock(&fs->s5f_mutex);
}


/*
 * Write len bytes to the given inode, starting at seek bytes from the
 * beginning of the inode. On success, return the number of bytes
 * actually written (which should be 'len', unless there's only enough
 * room for a partial write); on failure, return -errno.
 *
 * This function should allow writing to files or directories, treating
 * them identically.
 *
 * Writing to a sparse block of the file should cause that block to be
 * allocated.  Writing past the end of the file should increase the size
 * of the file. Blocks between the end and where you start writing will
 * be sparse.
 *
 * Do not call s5_seek_to_block() directly from this function.  You will
 * use the vnode's pframe functions, which will eventually result in a
 * call to s5_seek_to_block().
 *
 * You will need pframe_dirty(), pframe_get(), memcpy().
 */
int
s5_write_file(vnode_t *vnode, off_t seek, const char *bytes, size_t len)
{
        /* Convert vnode to inode for easy access */
        s5_inode_t *inode = VNODE_TO_S5INODE(vnode);

        int block_num = S5_DATA_BLOCK(seek); /* Which of inode's direct blocks are you writing to? */
        int offset = S5_DATA_OFFSET(seek); /* What is the offset within that block? */

        /* This is the address where the data for the block you want to write starts */
        int block_addr = inode->s5_direct_blocks[block_num];

        /* Get first block into pframe */
        pframe_t *pf;
        int res = pframe_get(&vnode->vn_mmobj, block_num, &pf);

        /* Write new bytes into that frame */
        char *read_startaddr = (char *)pf->pf_addr + S5_DATA_OFFSET(seek);
        memcpy((void *) read_startaddr, (void *)bytes, len);

        /* Dirty pframe, which will call s5fs_dirtypage() */
        res = pframe_dirty(pf);
        if (res < 0 ) {
            pframe_free(pf);
            return res;
        }

        dbg_print("vnode dir length before: %d\n", vnode->vn_len);
        vnode->vn_len += len;
        dbg_print("vnode dir length after: %d\n", vnode->vn_len);
        VNODE_TO_S5INODE(vnode)->s5_size += len;

        return len;


        /* int pframe_dirty(pframe_t *pf) */
/*
 * Indicates that a page is about to be modified. This should be called on a
 * page before any attempt to modify its contents. This marks the page dirty
 * (so that pageoutd knows to clean it before reclaiming the page frame)
 * and calls the dirtypage mmobj entry point.
 * The given page must not be busy.
 *
 * This routine can block at the mmobj operation level.
 *
 * @param pf the page to dirty
 * @return 0 on success, -errno on failure
 */

        /* NOT_YET_IMPLEMENTED("S5FS: s5_write_file"); */
        return -1;
}

/*
 * Read up to len bytes from the given inode, starting at seek bytes
 * from the beginning of the inode. On success, return the number of
 * bytes actually read, or 0 if the end of the file has been reached; on
 * failure, return -errno.
 *
 * This function should allow reading from files or directories,
 * treating them identically.
 *
 * Reading from a sparse block of the file should act like reading
 * zeros; it should not cause the sparse blocks to be allocated.
 *
 * Similarly as in s5_write_file(), do not call s5_seek_to_block()
 * directly from this function.
 *
 * If the region to be read would extend past the end of the file, less
 * data will be read than was requested.
 *
 * You probably want to use pframe_get(), memcpy().
 */
int
s5_read_file(struct vnode *vnode, off_t seek, char *dest, size_t len)
{
        /* int pframe_get(struct mmobj *o, uint32_t pagenum, pframe_t **result) */
/*
 * Find and return the pframe representing the page identified by the object
 * and page number. If the page is already resident in memory, then we return
 * the existing page. Otherwise, we allocate a new page and fill it (in which
 * case this routine may block).
 *
 * As long as this routine returns successfully, the returned page will be a
 * non-busy page that will be guaranteed to remain resident until the calling
 * context blocks without first pinning the page.
 *
 * This routine may block at the mmobj operation level.
 *
 * @param o the parent object of the page
 * @param pagenum the page number of this page in the object
 * @param result used to return the pframe (NULL if there's an error)
 * @return 0 on success, < 0 on failure.
 */

/* Start of data in file is in addr of s5_directblocks */

        /* If data block and offset are within correct range, then get addr of direct block from inode in vnode
         * Create page frame. Read from memory using pframeget.
 memobj from vnode
offset from inode (direct block)
read into pframe you created

If pageget was successful (it should be)
handle case where you fall off block by reading too far
then memcpy from pframe you got to destination
pf_addr is start of data
use data_offset macro to get offset
address to read from is start of data plus offset
return num bytes you read */

        /* Here, seek is an offset in BYTES (not blocks), where you want to
            start reading file from.
         */

        /* Convert vnode to inode for easy access */
        s5_inode_t *inode = VNODE_TO_S5INODE(vnode);
        /* If seek > inode->s5_size, attempting to start reading past end of file data. Return -errno. */
        if ((uint32_t) seek > inode->s5_size) {
            dbg_print("ERROR! Attempting to start read past end of file.\n");
            return -EFAULT; 
        }
        /* If seek + len are larger than one block, you'll need to bring in multiple pages */

        /* If you get here, start of read_file is within file data. */

        /* Get address of direct block from inode in vnode.
           Create page frame.
           Read into page frame you create using pframe_get().
           memobj from vnode, offset from inode */
        pframe_t *pf;
        /*int res = pframe_get(&vnode->vn_mmobj, inode->s5_direct_blocks[S5_DATA_BLOCK(seek)], &pf);*/
        int res = pframe_get(&vnode->vn_mmobj, S5_DATA_BLOCK(seek), &pf);
        if (res != 0) {
            dbg_print("ERROR! pframe_get() did not return successfully.\n");
            return res;
        }

        /* If pframe_get is successful (which it should be)
           handle case where you read too far off the end of the file
           i.e. reset the length to read as the length until the end of the file */
        if ((seek + len) > inode->s5_size) {
            len = inode->s5_size - seek;
        }

        /* Use memcopy from pframe you got to destination.
           pf_addr is start of data for that page frame
           use S5_DATA_OFFSET() macro to get offset
           address to read from is start of data plus offset */
        char *read_startaddr = (char *)pf->pf_addr + S5_DATA_OFFSET(seek);
        /* INSTEAD OF pf->pf_addr, USE inode->s5_directblocks[S5_DATA_BLOCK(seek)] ??? */
        /* char *block_addr = inode->s5_direct_blocks[S5_DATA_BLOCK(seek)] + S5_DATA_OFFSET(seek); */
        memcpy((void *) dest, (void *)read_startaddr, len);

        /* Return the number of bytes you actually read from the file */
        return len;

        /* NOT_YET_IMPLEMENTED("S5FS: s5_read_file"); */
        return -1;
}

/*
 * Allocate a new disk-block off the block free list and return it. If
 * there are no free blocks, return -ENOSPC.
 *
 * This will not initialize the contents of an allocated block; these
 * contents are undefined.
 *
 * If the super block's s5s_nfree is 0, you need to refill 
 * s5s_free_blocks and reset s5s_nfree.  You need to read the contents 
 * of this page using the pframe system in order to obtain the next set of
 * free block numbers.
 *
 * Don't forget to dirty the appropriate blocks!
 *
 * You'll probably want to use lock_s5(), unlock_s5(), pframe_get(),
 * and s5_dirty_super()
 */
static int
s5_alloc_block(s5fs_t *fs)
{
        /* Returns disk address where newly allocated block lives.
         *   i.e. valid pagenum for disk's mmobj.
         */

        lock_s5(fs);

        int disk_block; /* Will store disk block number of newly allocated block */
        s5_super_t *super = fs->s5f_super;

        /* pframe_get() with S5FS_TO_VMOBJ(fs). Page 0. Bring superblock into pframe. */
        /* Lock fs mutex */
        pframe_t *pf;
        pframe_get(S5FS_TO_VMOBJ(fs), S5_SUPER_BLOCK, &pf);

        /* If no free blocks in superblock, refill s5s_free_blocks and reset s5s_nfree. */
        /*   remember, last free block in list contains pointer to next set of free blocks */
        /* Check if last free block == -1, i.e. no more free blocks anywhere. */
        if (super->s5s_nfree == 0) {
            int next_link = super->s5s_free_blocks[S5_NBLKS_PER_FNODE-1]; /* Block number of block to copy entries from */

            if (next_link == -1) {
                unlock_s5(fs);
                return -ENOSPC; /* Reached end of free list. No more space on disk. */
            }

            /* Bring the page for that block into memory */
            pframe_t *pf_next_block;
            pframe_get(S5FS_TO_VMOBJ(fs), next_link, &pf_next_block);

            /* Copy contents of next block into free_blocks array */
            memcpy(super->s5s_free_blocks, pf_next_block->pf_addr, sizeof(super->s5s_free_blocks));
            /* Reset nfree */
            super->s5s_nfree = S5_NBLKS_PER_FNODE-1;

            s5_dirty_super(fs);

            unlock_s5(fs);

            return next_link;
        }

        /* Get a free block off the free list in the superblock */
        disk_block = super->s5s_free_blocks[super->s5s_nfree - 1];
        /* Reset nfree */
        super->s5s_nfree--;

        /* Dirty the superblock (since you changed the values) */
        s5_dirty_super(fs);

        unlock_s5(fs);

        return disk_block;

        /* NOT_YET_IMPLEMENTED("S5FS: s5_alloc_block"); */
        return -1;
}


/*
 * Given a filesystem and a block number, frees the given block in the
 * filesystem.
 *
 * This function may potentially block.
 *
 * The caller is responsible for ensuring that the block being placed on
 * the free list is actually free and is not resident.
 */
static void
s5_free_block(s5fs_t *fs, int blockno)
{
        s5_super_t *s = fs->s5f_super;


        lock_s5(fs);

        KASSERT(S5_NBLKS_PER_FNODE > s->s5s_nfree);

        if ((S5_NBLKS_PER_FNODE - 1) == s->s5s_nfree) {
                /* get the pframe where we will store the free block nums */
                pframe_t *prev_free_blocks = NULL;
                KASSERT(fs->s5f_bdev);
                pframe_get(&fs->s5f_bdev->bd_mmobj, blockno, &prev_free_blocks);
                KASSERT(prev_free_blocks->pf_addr);

                /* copy from the superblock to the new block on disk */
                memcpy(prev_free_blocks->pf_addr, (void *)(s->s5s_free_blocks),
                       S5_NBLKS_PER_FNODE * sizeof(int));
                pframe_dirty(prev_free_blocks);

                /* reset s->s5s_nfree and s->s5s_free_blocks */
                s->s5s_nfree = 0;
                s->s5s_free_blocks[S5_NBLKS_PER_FNODE - 1] = blockno;
        } else {
                s->s5s_free_blocks[s->s5s_nfree++] = blockno;
        }

        s5_dirty_super(fs);

        unlock_s5(fs);
}

/*
 * Creates a new inode from the free list and initializes its fields.
 * Uses S5_INODE_BLOCK to get the page from which to create the inode
 *
 * This function may block.
 */
int
s5_alloc_inode(fs_t *fs, uint16_t type, devid_t devid)
{
        s5fs_t *s5fs = FS_TO_S5FS(fs);
        pframe_t *inodep;
        s5_inode_t *inode;
        int ret = -1;

        KASSERT((S5_TYPE_DATA == type)
                || (S5_TYPE_DIR == type)
                || (S5_TYPE_CHR == type)
                || (S5_TYPE_BLK == type));


        lock_s5(s5fs);

        if (s5fs->s5f_super->s5s_free_inode == (uint32_t) -1) {
                unlock_s5(s5fs);
                return -ENOSPC;
        }

        pframe_get(&s5fs->s5f_bdev->bd_mmobj,
                   S5_INODE_BLOCK(s5fs->s5f_super->s5s_free_inode),
                   &inodep);
        KASSERT(inodep);

        inode = (s5_inode_t *)(inodep->pf_addr)
                + S5_INODE_OFFSET(s5fs->s5f_super->s5s_free_inode);

        KASSERT(inode->s5_number == s5fs->s5f_super->s5s_free_inode);

        ret = inode->s5_number;

        /* reset s5s_free_inode; remove the inode from the inode free list: */
        s5fs->s5f_super->s5s_free_inode = inode->s5_next_free;
        pframe_pin(inodep);
        s5_dirty_super(s5fs);
        pframe_unpin(inodep);


        /* init the newly-allocated inode: */
        inode->s5_size = 0;
        inode->s5_type = type;
        inode->s5_linkcount = 0;
        memset(inode->s5_direct_blocks, 0, S5_NDIRECT_BLOCKS * sizeof(int));
        if ((S5_TYPE_CHR == type) || (S5_TYPE_BLK == type))
                inode->s5_indirect_block = devid;
        else
                inode->s5_indirect_block = 0;

        s5_dirty_inode(s5fs, inode);

        unlock_s5(s5fs);

        return ret;
}


/*
 * Free an inode by freeing its disk blocks and putting it back on the
 * inode free list.
 *
 * You should also reset the inode to an unused state (eg. zero-ing its
 * list of blocks and setting its type to S5_FREE_TYPE).
 *
 * Don't forget to free the indirect block if it exists.
 *
 * You probably want to use s5_free_block().
 */
void
s5_free_inode(vnode_t *vnode)
{
        uint32_t i;
        s5_inode_t *inode = VNODE_TO_S5INODE(vnode);
        s5fs_t *fs = VNODE_TO_S5FS(vnode);

        KASSERT((S5_TYPE_DATA == inode->s5_type)
                || (S5_TYPE_DIR == inode->s5_type)
                || (S5_TYPE_CHR == inode->s5_type)
                || (S5_TYPE_BLK == inode->s5_type));

        /* free any direct blocks */
        for (i = 0; i < S5_NDIRECT_BLOCKS; ++i) {
                if (inode->s5_direct_blocks[i]) {
                        dprintf("freeing block %d\n", inode->s5_direct_blocks[i]);
                        s5_free_block(fs, inode->s5_direct_blocks[i]);

                        s5_dirty_inode(fs, inode);
                        inode->s5_direct_blocks[i] = 0;
                }
        }

        if (((S5_TYPE_DATA == inode->s5_type)
             || (S5_TYPE_DIR == inode->s5_type))
            && inode->s5_indirect_block) {
                pframe_t *ibp;
                uint32_t *b;

                pframe_get(S5FS_TO_VMOBJ(fs),
                           (unsigned)inode->s5_indirect_block,
                           &ibp);
                KASSERT(ibp
                        && "because never fails for block_device "
                        "vm_objects");
                pframe_pin(ibp);

                b = (uint32_t *)(ibp->pf_addr);
                for (i = 0; i < S5_NIDIRECT_BLOCKS; ++i) {
                        KASSERT(b[i] != inode->s5_indirect_block);
                        if (b[i])
                                s5_free_block(fs, b[i]);
                }

                pframe_unpin(ibp);

                s5_free_block(fs, inode->s5_indirect_block);
        }

        inode->s5_indirect_block = 0;
        inode->s5_type = S5_TYPE_FREE;
        s5_dirty_inode(fs, inode);

        lock_s5(fs);
        inode->s5_next_free = fs->s5f_super->s5s_free_inode;
        fs->s5f_super->s5s_free_inode = inode->s5_number;
        unlock_s5(fs);

        s5_dirty_inode(fs, inode);
        s5_dirty_super(fs);
}

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
int
s5_find_dirent(vnode_t *vnode, const char *name, size_t namelen)
{
        /* int s5_read_file(struct vnode *vnode, off_t seek, char *dest, size_t len) */
/*
 * Read up to len bytes from the given inode, starting at seek bytes
 * from the beginning of the inode. On success, return the number of
 * bytes actually read, or 0 if the end of the file has been reached; on
 * failure, return -errno.
 *
 * This function should allow reading from files or directories,
 * treating them identically.
 *
 * Reading from a sparse block of the file should act like reading
 * zeros; it should not cause the sparse blocks to be allocated.
 *
 * Similarly as in s5_write_file(), do not call s5_seek_to_block()
 * directly from this function.
 *
 * If the region to be read would extend past the end of the file, less
 * data will be read than was requested.
 *
 * You probably want to use pframe_get(), memcpy().
 */
        s5_dirent_t dirent;
        off_t offset = 0;

        while ( s5_read_file(vnode, offset, (char*)&dirent, sizeof(s5_dirent_t)) > 0 ) {
            dbg_print("Found directory %s on vnode %d\n", dirent.s5d_name, dirent.s5d_inode);
            if (name_match(dirent.s5d_name, name, namelen)) {
                return dirent.s5d_inode;
            }
            offset += sizeof(s5_dirent_t);
        }

        /* If you get here, you reached the end of the file and didn't find a
         *   directory entry with a matching name. Return error.
         */
        return -ENOENT;


/* #define name_match(fname, name, namelen) \
        ( strlen(fname) == namelen && !strncmp((fname), (name), (namelen)) ) */

        /* NOT_YET_IMPLEMENTED("S5FS: s5_find_dirent");
        return -1; */
}

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
int
s5_remove_dirent(vnode_t *vnode, const char *name, size_t namelen)
{

/*        s5_dirent_t dirent;
        off_t offset = 0;

        while ( s5_read_file(vnode, offset, (char*)&dirent, sizeof(s5_dirent_t)) != 0 ) {
            dbg_print("Found directory %s on vnode %d\n", dirent.s5d_name, dirent.s5d_inode);
            if (name_match(dirent.s5d_name, name, namelen)) {
                return dirent.s5d_inode;
            }
            offset += sizeof(s5_dirent_t);
        }

        return -ENOENT;
*/

        int dirent_ino = s5_find_dirent(vnode, name, namelen);
        if (dirent_ino < 0) return dirent_ino;

        /* If you get here, directory entry was found, so remove file */
        s5_dirent_t file_to_rm;
        s5_dirent_t last_dirent;
        off_t rmfile_offset = -1;
        off_t last_offset = 0;

        while (s5_read_file(vnode, last_offset, (char *)&last_dirent, sizeof(s5_dirent_t)) != 0) {
            dbg_print("Found directory %s on vnode %d\n", last_dirent.s5d_name, last_dirent.s5d_inode);
            if (name_match(last_dirent.s5d_name, name, namelen)) {
                file_to_rm = last_dirent;
                rmfile_offset = last_offset;
            }
            last_offset += sizeof(s5_dirent_t);
        }
        last_offset -= sizeof(s5_dirent_t); /* When while exits, last_offset will be at end of directory dirents */
                                            /*   we want it to point at the last dirent */

        if (rmfile_offset == -1) return -ENOENT; /* If no dirent with matching name found, return error */

        /* Check to make sure you aren't removing a directory - CAN'T DO HERE, SINCE RMDIR() CALLS THIS */
        vnode_t *rm_file = vget(vnode->vn_fs, file_to_rm.s5d_inode); 
        /*if (VNODE_TO_S5INODE(rm_file)->s5_type == S5_TYPE_DIR) return -EPERM;*/

        /* Match found. Write last dirent into location of found dirent. */
        if (rmfile_offset != last_offset) {
            int res = s5_write_file(vnode, rmfile_offset, (char *)&last_dirent, sizeof(s5_dirent_t));
            if (res < 0) return res;
        }

        /* Update length of directory vnode and inode */
        int old_len = vnode->vn_len;
        vnode->vn_len -= sizeof(s5_dirent_t);
        VNODE_TO_S5INODE(vnode)->s5_size -= sizeof(s5_dirent_t);
        

        /* If block at end of directory file no longer needed, free it */
        if (S5_DATA_BLOCK(old_len) > S5_DATA_BLOCK(vnode->vn_len)) {
            dbg_print("Extra block at end of vnode. STILL NEED TO FREE IT.\n");
        }

        /* Dirty directory inode */
        s5_dirty_inode(VNODE_TO_S5FS(vnode), VNODE_TO_S5INODE(vnode));

        /* Decrement link count in child */
        VNODE_TO_S5INODE(rm_file)->s5_linkcount--;
        s5_dirty_inode(VNODE_TO_S5FS(rm_file), VNODE_TO_S5INODE(rm_file));
        vput(rm_file);

        return 0;
        
        /* vget on vnode
         * find offset into file where dirent lives
         * if dirent is not a directory type, and it exists,
         *   find the last directory entry in vnode->inode and
         *   write it to offset where dirent to remove lived
         *   CHANGE LENGTH IN INODE AND VNODE OF DIR
         * decrement link count on file that was removed
         */
 
        /* NOT_YET_IMPLEMENTED("S5FS: s5_remove_dirent"); */
        return -1;
}

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
int
s5_link(vnode_t *parent, vnode_t *child, const char *name, size_t namelen)
{

        /* int s5_find_dirent(vnode_t *vnode, const char *name, size_t namelen) */
/*
 * Locate the directory entry in the given inode with the given name,
 * and return its inode number. If there is no entry with the given
 * name, return -ENOENT.
 */

        /* int s5_write_file(vnode_t *vnode, off_t seek, const char *bytes, size_t len) */
/*
 * Write len bytes to the given inode, starting at seek bytes from the
 * beginning of the inode. On success, return the number of bytes
 * actually written (which should be 'len', unless there's only enough
 * room for a partial write); on failure, return -errno.
 *
 * This function should allow writing to files or directories, treating
 * them identically.
 *
 * Writing to a sparse block of the file should cause that block to be
 * allocated.  Writing past the end of the file should increase the size
 * of the file. Blocks between the end and where you start writing will
 * be sparse.
 *
 * Do not call s5_seek_to_block() directly from this function.  You will
 * use the vnode's pframe functions, which will eventually result in a
 * call to s5_seek_to_block().
 *
 * You will need pframe_dirty(), pframe_get(), memcpy().
 */

        /* #define s5_dirty_inode(fs, inode)  */

        /* Make sure child doesn't already exist. Use find_dirent for that.
else create a dirent s5_dirent_t, initizlie
inode value is s5 number. convert vnode to s5 inode macro.
write to file, increment link count, dirty parent's inode.
seek should be length of parent. len is sizeof(dirent_t)
return how many bytes you wrote
*/


        /* If you get here, child file doesn't exist yet, so you can create it. */
            s5_dirent_t new_dirent;
            strncpy(&new_dirent.s5d_name, name, S5_NAME_LEN-1);
            new_dirent.s5d_name[namelen] = '\0';

        if (VNODE_TO_S5INODE(parent)->s5_type == S5_TYPE_DIR) { /* Adding a new file to a directory */
            /* Make sure child doesn't already exist */
            int exists = s5_find_dirent(parent, name, namelen);
            if (exists != -ENOENT) {
                dbg_print("Can't create link. File %s already exists.\n", name);
                return -EEXIST;
            }

            new_dirent.s5d_inode = VNODE_TO_S5INODE(child)->s5_number;

            int res = s5_write_file(parent, parent->vn_len, (char*)&new_dirent, sizeof(s5_dirent_t));
            if (res < 0) return res;

            /* Increment the link count for the child inode */
            VNODE_TO_S5INODE(child)->s5_linkcount++;
            dbg_print("Incremented child %s link count to %d\n", name, VNODE_TO_S5INODE(child)->s5_linkcount);

            /* Dirty parent's inode */
            s5_dirty_inode(VNODE_TO_S5FS(parent), VNODE_TO_S5INODE(parent));
        }
        else if (VNODE_TO_S5INODE(parent)->s5_type == S5_TYPE_DATA) { /* Linking a data file to another data file */
            /* Make sure file name to link to doesn't already exist */
            int exists = s5_find_dirent(child, name, namelen);
            if (exists != -ENOENT) {
                dbg_print("Can't create link. File %s already exists.\n", name);
                return -EEXIST;
            }

            new_dirent.s5d_inode = VNODE_TO_S5INODE(parent)->s5_number;
            int res = s5_write_file(child, child->vn_len, (char*)&new_dirent, sizeof(s5_dirent_t));
            if (res < 0) return res;

            VNODE_TO_S5INODE(parent)->s5_linkcount++;
            dbg_print("Incremented parent %s link count to %d\n", name, VNODE_TO_S5INODE(parent)->s5_linkcount);
            s5_dirty_inode(VNODE_TO_S5FS(child), VNODE_TO_S5INODE(child));
        }

        return 0;

        /* NOT_YET_IMPLEMENTED("S5FS: s5_link"); */
        return -1;
}

/*
 * Return the number of blocks that this inode has allocated on disk.
 * This should include the indirect block, but not include sparse
 * blocks.
 *
 * This is only used by s5fs_stat().
 *
 * You'll probably want to use pframe_get().
 */
int
s5_inode_blocks(vnode_t *vnode)
{
        int block_count = 0;
        s5_inode_t *inode = VNODE_TO_S5INODE(vnode);

        /* For each block within length of file */
        int blocks_to_check = S5_DATA_BLOCK(vnode->vn_len);
        if (S5_DATA_OFFSET(vnode->vn_len) > 0) {
            blocks_to_check++; /* Additional block needed to hold partial block at end of file */
        }
        dbg_print("Blocks to check: %d\n", blocks_to_check);

        if (blocks_to_check == 0) return block_count;

        int i;
        for (i = 0; i < S5_NDIRECT_BLOCKS; i++) {

            /* Read value in ith direct block */
            if (inode->s5_direct_blocks[i] != 0) {
                block_count++;
            }

            if (i == blocks_to_check-1) { /* If you've checked all blocks in file */
                return block_count;
            }
        }

        /* (blocks_to_check > S5_NDIRECT_BLOCKS-1) */
          /* Need to go through indirect block entries */

        block_count++; /* Increment once, to include indirect block pointer in inode */

        uint32_t disk_block_addr; /* Will store contents of indirect block entry */

        /* Bring indirect block into memory */
        pframe_t *pf;
        pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(vnode)), inode->s5_indirect_block, &pf);
        pframe_pin(pf);

        for (i = 0; i < (blocks_to_check - S5_NDIRECT_BLOCKS); i++) {
            /* Indirect block field in inode corresponds to a disk block number
             *   where the indirect block data lives
             */
            off_t indirect_block_index = i;
            char *iblock_entry_addr = (char *) pf->pf_addr + (sizeof(int) * indirect_block_index);

            memcpy((void *) &disk_block_addr, (void *) iblock_entry_addr, sizeof(int));

            if (disk_block_addr != 0) block_count++;
        }

        pframe_unpin(pf);

        return block_count;


        /* NOT_YET_IMPLEMENTED("S5FS: s5_inode_blocks"); */
        return -1;
}

