/*
 * HPFSNT - IRP_MJ_DEVICE_CONTROL and IRP_MJ_LOCK_CONTROL
 */

#include "hpfs.h"

/* Device requests on any of our handles go to the disk */
NTSTATUS
HpfsCommonDeviceControl (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_CCB Ccb = (PHPFS_CCB)FileObject->FsContext2;
    PHPFS_VCB Vcb = Ctx->Vcb;

    if (FileObject->FsContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * NT 3.1's DeviceIoControl sends file system control codes as device
     * controls: they are ours (the parameter layouts are the same).
     */
    if ((Ctx->IrpSp->Parameters.DeviceIoControl.IoControlCode >> 16) == FILE_DEVICE_FILE_SYSTEM) {
        return HpfsUserFsRequest(Ctx);
    }

    /* After a removal the target device may be gone */
    if ((Vcb->VcbState & VCB_STATE_DISMOUNTED) &&
        (Ccb == NULL || !(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME))) {

        return HPFS_STATUS_DISMOUNTED;
    }

    HpfsSkipStack(Ctx->Irp);
    Ctx->Flags |= HPFS_CTX_NO_COMPLETE;

    return IoCallDriver(Vcb->TargetDeviceObject, Ctx->Irp);
}

NTSTATUS
HpfsCommonLockControl (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PHPFS_FCB Fcb = (PHPFS_FCB)Ctx->IrpSp->FileObject->FsContext;
    NTSTATUS Status;

    if (Fcb == NULL || !HpfsIsFcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    Status = HpfsVerifyVcb(Ctx, Fcb->Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);

    __try {

        /* Completes the IRP itself */
        Status = FsRtlProcessFileLock(&Fcb->FileLock, Ctx->Irp, NULL);
        Ctx->Flags |= HPFS_CTX_NO_COMPLETE;

        Fcb->Header.IsFastIoPossible = HpfsIsFastIoPossible(Fcb);

    } __finally {

        HpfsRelease(&Fcb->Resource);
    }

    return Status;
}
