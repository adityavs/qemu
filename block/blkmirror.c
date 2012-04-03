/*
 * Block driver for mirrored writes.
 *
 * Copyright (C) 2011-2012 Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdarg.h>
#include "block_int.h"

typedef struct DupAIOCB DupAIOCB;

typedef struct SingleAIOCB {
    BlockDriverAIOCB *aiocb;
    DupAIOCB *parent;
} SingleAIOCB;

struct DupAIOCB {
    BlockDriverAIOCB common;
    int count;
    int canceled;

    BlockDriverCompletionFunc *cb;
    SingleAIOCB aios[2];
    int ret;
};

/* Valid blkmirror filenames look like blkmirror:format:path/to/target.
 *
 * The driver is not intended for general usage.  It expects bdrv_append
 * to tack it onto an existing image, which is used as the primary
 * source and which shares the backing file with the target.
 *
 * Overall, this driver is full of hacks and limitations, which are a
 * consequence of the lack for arbitrary configuration of block devices
 * (aka -blockdev).  Still, at the same time it's quite elegant in how
 * it efficiently reuses other bits of infrastructure in the QEMU block
 * layer.  In particular:
 *
 *    * Storing the source's backing file in bs->backing_hd makes streaming
 *      commands operate transparently on the blkmirror device: data is read
 *      from the source and written to both source and target (which is a bit
 *      inefficient, but could be optimized).
 *
 *    * Sharing the backing file is needed so that the target can already
 *      operate before the destination backing file is in place, in case it
 *      is copied outside QEMU.  It is necessary to have a backing file;
 *      BDRV_O_NO_BACKING alone is not enough, because otherwise CoW on
 *      the target would not prefill newly-allocated clusters with the
 *      correct data.
 *
 *    * Finally, relying on bdrv_append makes it easy to install blkmirror
 *      atomically, with an easy rollback path in case creation of the
 *      blkmirror device fails (example: mode=existing and a non-existent
 *      target).  Alternatives would require rollbacking a reopen command,
 *      possibly with the added complication of a partially-shared backing
 *      file chain.
 */
static int blkmirror_open(BlockDriverState *bs, const char *filename, int flags)
{
    int ret;
    const char *filename2;
    char *format;
    BlockDriver *drv;

    /* Parse the blkmirror: prefix */
    if (!strstart(filename, "blkmirror:", &filename)) {
        return -EINVAL;
    }

    /* The source image filename is added by bdrv_append.  We only need
     * to parse and open the destination image and format.  */
    filename2 = strchr(filename, ':');
    if (filename2) {
        format = g_strdup(filename);
        format[filename2 - filename] = 0;
        filename2++;

        drv = bdrv_find_whitelisted_format(format);
        if (!drv) {
            ret = -EINVAL;
            qerror_report(QERR_INVALID_PARAMETER_VALUE, "format",
                          "a supported format");
            goto out;
        }
    } else {
        drv = NULL;
        format = NULL;
        filename2 = filename;
    }

    /* BDRV_O_NO_BACKING: Source and target share the backing file, but
     * the source (bs->backing_hd) is only set after initialization.
     * We will initialize bs->file->backing_hd later.
     *
     * BDRV_O_NO_FLUSH: If we crash, we cannot assume the target to
     * be a valid mirror and we have to start over.  So speed up things
     * by operating on the destination in cache=unsafe mode.
     */
    bs->file = bdrv_new("");
    ret = bdrv_open(bs->file, filename2,
                    flags | BDRV_O_NO_BACKING | BDRV_O_NO_FLUSH | BDRV_O_CACHE_WB,
                    drv);
    if (ret < 0) {
        goto out;
    }

out:
    g_free(format);
    return ret;
}

static void blkmirror_rebind(BlockDriverState *bs)
{
    BlockDriverState *source = bs->backing_hd;

    /* Do not store the source in backing_hd.  Store the source's backing
     * file instead.
     */
    bs->opaque = bs->backing_hd;
    bs->backing_hd = source->backing_hd;

    /* Forbid committing to the backing file.  */
    if (bs->backing_hd) {
        bs->backing_hd->keep_read_only = true;
    }

    /* That is also the target's backing file.  */
    bs->file->backing_hd = source->backing_hd;
}

static void blkmirror_close(BlockDriverState *bs)
{
    BlockDriverState *source = bs->opaque;

    source->backing_hd = NULL;
    bs->file->backing_hd = NULL;

    /* backing_hd and file closed by the caller.  */
    bdrv_delete(source);
}

static coroutine_fn int blkmirror_co_flush(BlockDriverState *bs)
{
    BlockDriverState *source = bs->opaque;

    return bdrv_co_flush(source);
}

static int64_t blkmirror_getlength(BlockDriverState *bs)
{
    return bdrv_getlength(bs->file);
}

static int coroutine_fn blkmirror_co_is_allocated(BlockDriverState *bs,
                                                  int64_t sector_num,
                                                  int nb_sectors, int *pnum)
{
    return bdrv_is_allocated(bs->file, sector_num, nb_sectors, pnum);
}

static BlockDriverAIOCB *blkmirror_aio_readv(BlockDriverState *bs,
                                             int64_t sector_num,
                                             QEMUIOVector *qiov, int nb_sectors,
                                             BlockDriverCompletionFunc *cb,
                                             void *opaque)
{
    BlockDriverState *source = bs->opaque;

    return bdrv_aio_readv(source, sector_num, qiov, nb_sectors,
                          cb, opaque);
}

static void dup_aio_cancel(BlockDriverAIOCB *acb)
{
    DupAIOCB *dcb = container_of(acb, DupAIOCB, common);
    int i;

    dcb->canceled = true;
    for (i = 0 ; i < 2; i++) {
        if (dcb->aios[i].aiocb) {
            bdrv_aio_cancel(dcb->aios[i].aiocb);
        }
    }
    qemu_aio_release(dcb);
}

static AIOPool dup_aio_pool = {
    .aiocb_size         = sizeof(DupAIOCB),
    .cancel             = dup_aio_cancel,
};

static void blkmirror_aio_cb(void *opaque, int ret)
{
    SingleAIOCB *scb = opaque;
    DupAIOCB *dcb = scb->parent;

    scb->aiocb = NULL;

    assert(dcb->count > 0);
    if (ret < 0 && dcb->ret == 0) {
        dcb->ret = ret;
    }
    if (--dcb->count == 0) {
        dcb->common.cb(dcb->common.opaque, dcb->ret);
        if (!dcb->canceled) {
            qemu_aio_release(dcb);
        }
    }
}

static DupAIOCB *dup_aio_get(BlockDriverState *bs,
                             BlockDriverCompletionFunc *cb,
                             void *opaque)
{
    DupAIOCB *dcb = qemu_aio_get(&dup_aio_pool, bs, cb, opaque);

    dcb->canceled = false;
    dcb->aios[0].parent = dcb;
    dcb->aios[1].parent = dcb;
    dcb->count = 2;
    dcb->ret = 0;
    return dcb;
}

static BlockDriverAIOCB *blkmirror_aio_writev(BlockDriverState *bs,
                                              int64_t sector_num,
                                              QEMUIOVector *qiov,
                                              int nb_sectors,
                                              BlockDriverCompletionFunc *cb,
                                              void *opaque)
{
    BlockDriverState *source = bs->opaque;
    DupAIOCB *dcb = dup_aio_get(bs, cb, opaque);

    dcb->aios[0].aiocb = bdrv_aio_writev(source, sector_num, qiov,
                                         nb_sectors, blkmirror_aio_cb,
                                         &dcb->aios[0]);
    dcb->aios[1].aiocb = bdrv_aio_writev(bs->file, sector_num, qiov,
                                         nb_sectors, blkmirror_aio_cb,
                                         &dcb->aios[1]);

    return &dcb->common;
}

static BlockDriverAIOCB *blkmirror_aio_discard(BlockDriverState *bs,
                                               int64_t sector_num,
                                               int nb_sectors,
                                               BlockDriverCompletionFunc *cb,
                                               void *opaque)
{
    BlockDriverState *source = bs->opaque;
    DupAIOCB *dcb = dup_aio_get(bs, cb, opaque);

    dcb->aios[0].aiocb = bdrv_aio_discard(source, sector_num,
                                          nb_sectors, blkmirror_aio_cb,
                                          &dcb->aios[0]);
    dcb->aios[1].aiocb = bdrv_aio_discard(bs->file, sector_num,
                                          nb_sectors, blkmirror_aio_cb,
                                          &dcb->aios[1]);

    return &dcb->common;
}

static int blkmirror_change_backing_file(BlockDriverState *bs,
    const char *backing_file, const char *backing_fmt)
{
    BlockDriverState *source = bs->opaque;
    int ret;

    /* Our backing file has changed, change the source and the target's too.  */
    source->backing_hd = bs->backing_hd;
    bs->file->backing_hd = source->backing_hd;

    /* First change the backing file on the target.  If the change will
     * fail on the source, the target will anyway be discarded.  If we
     * did it in the other order, we could end up with a changed source
     * even after reporting an error.
     */
    ret = bdrv_change_backing_file(bs->file, backing_file, backing_fmt);
    if (ret < 0) {
        return ret;
    }

    ret = bdrv_change_backing_file(source, backing_file, backing_fmt);
    if (ret < 0) {
        return ret;
    }

    pstrcpy(bs->backing_file, sizeof(bs->backing_file), backing_file ?: "");
    pstrcpy(bs->backing_format, sizeof(bs->backing_format), backing_fmt ?: "");
    return 0;
}


static BlockDriver bdrv_blkmirror = {
    .format_name        = "blkmirror",
    .protocol_name      = "blkmirror",
    .instance_size      = 0,

    .bdrv_getlength     = blkmirror_getlength,

    .bdrv_rebind        = blkmirror_rebind,
    .bdrv_change_backing_file   = blkmirror_change_backing_file,

    .bdrv_file_open     = blkmirror_open,
    .bdrv_close         = blkmirror_close,
    .bdrv_co_flush_to_disk = blkmirror_co_flush,
    .bdrv_co_is_allocated = blkmirror_co_is_allocated,

    .bdrv_aio_readv      = blkmirror_aio_readv,
    .bdrv_aio_writev     = blkmirror_aio_writev,
    .bdrv_aio_discard    = blkmirror_aio_discard,
};

static void bdrv_blkmirror_init(void)
{
    bdrv_register(&bdrv_blkmirror);
}

block_init(bdrv_blkmirror_init);
