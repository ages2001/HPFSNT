/*
 * HPFSNT - IRP_MJ_FLUSH_BUFFERS, IRP_MJ_SHUTDOWN and the dirty flag
 *
 * The spareblock carries a dirty bit (OS/2 and NT's CHKDSK run when it is
 * set). It goes to the disk before the first change to the volume and is
 * cleared once everything has been written: on a volume flush, lock,
 * dismount or shutdown, and on removable media when its last writer
 * closes. A volume that was dirty when mounted stays dirty for the check
 * tool.
 */

#include "hpfs.h"

/* ------------------------------------------------------------------ */
/* The dirty bit                                                       */
/* ------------------------------------------------------------------ */

/*
 * Before the first change. Called by the engine host (HpfsHostMap) with
 * Vcb->EngineResource held, before it hands out a block for writing.
 */
VOID
HpfsMarkVolumeDirty (
    PHPFS_VCB Vcb
    )
{
    NTSTATUS Status;

    if (Vcb->DirtyMarked || Vcb->InMarkDirty || (Vcb->VcbState & VCB_STATE_READ_ONLY)) {
        return;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->EngineResource, TRUE);

    Vcb->InMarkDirty = TRUE;
    Status = HpfsMarkDirty(&Vcb->Vol, TRUE);
    if (NT_SUCCESS(Status)) {
        /* Nothing else is pending here: the last engine call flushed */
        Status = HpfsMetaFlush(Vcb);
    }
    Vcb->InMarkDirty = FALSE;

    /* Even on failure: retrying before every block would not help */
    Vcb->DirtyMarked = TRUE;

    HpfsRelease(&Vcb->EngineResource);

    if (!NT_SUCCESS(Status)) {
        HPFS_DBG((HPFS_PFX "setting the dirty bit failed %lX\n", Status));
    }
}

/* Once everything is on the disk: Vcb->Resource exclusive */
NTSTATUS
HpfsMarkVolumeClean (
    PHPFS_VCB Vcb
    )
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Vcb->DirtyMarked || (Vcb->VcbState & (VCB_STATE_KEEP_DIRTY | VCB_STATE_READ_ONLY))) {
        return STATUS_SUCCESS;
    }

    HpfsEngineEnter(Vcb);
    Vcb->InMarkDirty = TRUE;
    Status = HpfsMarkDirty(&Vcb->Vol, FALSE);
    Vcb->InMarkDirty = FALSE;
    Status = HpfsEngineLeave(Vcb, Status);

    if (NT_SUCCESS(Status)) {
        Vcb->DirtyMarked = FALSE;
    }
    return Status;
}

/* ------------------------------------------------------------------ */
/* Flushing                                                            */
/* ------------------------------------------------------------------ */

/* Data and entry set of one file; Vcb->Resource exclusive */
NTSTATUS
HpfsFlushFile (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {

            CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);
            Status = Iosb.Status;

            /* Let a lazy write in progress finish */
            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            HpfsRelease(&Fcb->PagingIoResource);
        }

        /*
         * HPFS keeps no valid-data length on disk, so any sectors allocated
         * past the last byte written must be zeroed before the larger size
         * reaches the directory entry, or stale data would show through on
         * the next mount.
         */
        if (HpfsIsFcb(Fcb) && !(Fcb->FcbState & FCB_STATE_DELETED) &&
            Fcb->Header.ValidDataLength.QuadPart < Fcb->Header.FileSize.QuadPart) {

            NTSTATUS Zero = HpfsZeroDisk(Vcb, Fcb, Fcb->Header.ValidDataLength.QuadPart,
                                        Fcb->Header.FileSize.QuadPart);

            if (NT_SUCCESS(Zero)) {
                Fcb->Header.ValidDataLength = Fcb->Header.FileSize;
            } else if (NT_SUCCESS(Status)) {
                Status = Zero;
            }
        }

        if ((Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) && !(Fcb->FcbState & FCB_STATE_DELETED)) {

            if (NT_SUCCESS(Status)) {
                Status = HpfsUpdateDirent(Vcb, Fcb);
            } else {
                (VOID)HpfsUpdateDirent(Vcb, Fcb);
            }
        }

    } __finally {

        HpfsRelease(&Fcb->Resource);
    }

    return Status;
}

/* Metadata (written at the end of each engine call; this catches errors) */
static NTSTATUS
HpfsFlushMetadata (
    PHPFS_VCB Vcb
    )
{
    NTSTATUS Status;

    HpfsEngineEnter(Vcb);
    Status = HpfsEngineLeave(Vcb, STATUS_SUCCESS);
    return Status;
}

/*
 * Writes everything cached for the volume, and with MarkClean clears
 * VolumeDirty afterwards. Vcb->Resource exclusive.
 */
NTSTATUS
HpfsFlushVolume (
    PHPFS_VCB Vcb,
    BOOLEAN MarkClean
    )
{
    PLIST_ENTRY Entry;
    PHPFS_FCB Fcb;
    BOOLEAN Restart;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    if (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED)) {
        return STATUS_SUCCESS;
    }

    /* A flush can close file objects, so the list is walked with markers */
    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, HPFS_FCB, FcbLinks);

            if ((Fcb->FcbState & FCB_STATE_VISITED) || !HpfsIsFcb(Fcb)) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;
            Fcb->RefCount++;

            Result = HpfsFlushFile(Vcb, Fcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }

            HpfsDereferenceFcb(Vcb, Fcb);

            Restart = TRUE;
            break;
        }

    } while (Restart);

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, HPFS_FCB, FcbLinks);
        Fcb->FcbState &= ~FCB_STATE_VISITED;
    }

    Result = HpfsFlushMetadata(Vcb);
    DBGP("[HPFS flushvol data="); DBGX(Status); DBGP(" meta="); DBGX(Result);
    DBGP(" dirty="); DBGX(Vcb->DirtyMarked); DBGP(" state="); DBGX(Vcb->VcbState); DBGP("]\n");
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    /* The dirty bit is about the structures: they are on the disk once the
       metadata is, whatever happened to some file's data */
    if (MarkClean && NT_SUCCESS(Result)) {
        Result = HpfsMarkVolumeClean(Vcb);
        if (NT_SUCCESS(Status)) {
            Status = Result;
        }
    }

    (VOID)HpfsFlushDevice(Vcb);

    return Status;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

NTSTATUS
HpfsCommonFlushBuffers (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status) || (Vcb->VcbState & VCB_STATE_READ_ONLY)) {
            __leave;
        }

        if (HpfsIsVfcb(Fcb)) {

            Status = HpfsFlushVolume(Vcb, TRUE);

        } else {

            if (HpfsIsFcb(Fcb)) {
                Status = HpfsFlushFile(Vcb, Fcb);
            }

            if (NT_SUCCESS(Status)) {
                Status = HpfsFlushMetadata(Vcb);
            } else {
                (VOID)HpfsFlushMetadata(Vcb);
            }

            (VOID)HpfsFlushDevice(Vcb);
        }

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}

/* Sent to the file system device: leave every volume clean */
NTSTATUS
HpfsCommonShutdown (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PLIST_ENTRY Entry;
    PHPFS_VCB Vcb;

    UNREFERENCED_PARAMETER(Ctx);

    (VOID)ExAcquireResourceSharedLite(&HpfsData.Resource, TRUE);

    __try {

        for (Entry = HpfsData.VcbList.Flink; Entry != &HpfsData.VcbList; Entry = Entry->Flink) {

            Vcb = CONTAINING_RECORD(Entry, HPFS_VCB, VcbLinks);

            (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

            if (Vcb->VcbState & VCB_STATE_MOUNTED) {
                /* From now on every change is followed by a clean bit (HpfsEngineLeave) */
                Vcb->VcbState |= VCB_STATE_SHUTDOWN;
                (VOID)HpfsFlushVolume(Vcb, TRUE);
            }

            HpfsRelease(&Vcb->Resource);
        }

    } __finally {

        HpfsRelease(&HpfsData.Resource);
    }

    return STATUS_SUCCESS;
}
