/*
 * HPFSNT - structured exception handling
 *
 * HpfsTryCall(Body, Context) runs Body and catches what it raises, as an
 * __try/__except around it would. Visual C++ builds use __try/__except;
 * GCC has neither, so:
 *
 *   x86    A frame is registered on the thread's exception chain (FS:[0]).
 *          Its handler records the code, unwinds every frame registered
 *          after it (RtlUnwind, so the Cache Manager's termination handlers
 *          run), takes itself off the chain and jumps back
 *          (__builtin_longjmp). This is what the Visual C++ runtime's
 *          _except_handler does before it runs an __except block.
 *
 *   amd64  Exceptions are dispatched by the unwind tables. A small
 *          assembly routine calls Body; its unwind data names a language
 *          handler which records the code and unwinds to that routine
 *          (RtlUnwindEx), resuming after the call with the code in RAX -
 *          what __C_specific_handler does for an __except block.
 */

#include "hpfs.h"

NTSTATUS
HpfsExceptionStatus (
    NTSTATUS Code
    )
{
    if (Code == STATUS_IN_PAGE_ERROR) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }
    if (Code == STATUS_ACCESS_VIOLATION || Code == STATUS_DATATYPE_MISALIGNMENT) {
        return STATUS_INVALID_USER_BUFFER;
    }
    if (!FsRtlIsNtstatusExpected(Code)) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }
    return Code;
}

/* The code an exception is reported with: an in-page error carries the I/O status */
static NTSTATUS
HpfsCatchCode (
    PEXCEPTION_RECORD Record
    )
{
    if (Record->ExceptionCode == STATUS_IN_PAGE_ERROR && Record->NumberParameters >= 3) {
        return (NTSTATUS)Record->ExceptionInformation[2];
    }
    return Record->ExceptionCode;
}

static BOOLEAN
HpfsWanted (
    PEXCEPTION_RECORD Record,
    BOOLEAN ExpectedOnly
    )
{
    return (BOOLEAN)(!ExpectedOnly || Record->ExceptionCode == STATUS_IN_PAGE_ERROR ||
                     FsRtlIsNtstatusExpected(Record->ExceptionCode));
}

#ifndef EXCEPTION_UNWINDING
#define EXCEPTION_UNWINDING     0x2
#endif
#ifndef EXCEPTION_EXIT_UNWIND
#define EXCEPTION_EXIT_UNWIND   0x4
#endif

#if defined(__GNUC__) && defined(_X86_)

/* ------------------------------------------------------------------ */
/* GCC, x86: a frame on the FS:[0] chain                               */
/* ------------------------------------------------------------------ */

__declspec(dllimport) VOID NTAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
                                           PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);

typedef struct _HPFS_SEH_REG {
    struct _HPFS_SEH_REG *Next;
    PVOID       Handler;
} HPFS_SEH_REG;

typedef struct _HPFS_SEH {
    HPFS_SEH_REG Reg;               /* must be first */
    PVOID       Jmp[5];
    NTSTATUS    Code;
    BOOLEAN     ExpectedOnly;
} HPFS_SEH, *PHPFS_SEH;

static __inline PVOID
ReadFs0 (
    VOID
    )
{
    PVOID P;
    __asm__ __volatile__ ("movl %%fs:0, %0" : "=r" (P));
    return P;
}

static __inline VOID
WriteFs0 (
    PVOID P
    )
{
    __asm__ __volatile__ ("movl %0, %%fs:0" : : "r" (P) : "memory");
}

/*
 * RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue) resumes at
 * TargetIp once every frame above TargetFrame has been unwound; pass the
 * address right after the call, as the C runtime's global unwind does.
 */
static VOID __attribute__((noinline))
HpfsGlobalUnwind (
    PVOID Frame,
    PEXCEPTION_RECORD Record
    )
{
    PVOID Target = (PVOID)&RtlUnwind;

    __asm__ __volatile__ (
        "pushl %%ebx\n\t"
        "pushl %%esi\n\t"
        "pushl %%edi\n\t"
        "pushl %%ebp\n\t"
        "pushl $0\n\t"
        "pushl %1\n\t"
        "pushl $1f\n\t"
        "pushl %0\n\t"
        "call *%2\n"
        "1:\n\t"
        "popl %%ebp\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "popl %%ebx\n\t"
        : "+a" (Frame), "+d" (Record), "+c" (Target)
        :
        : "memory", "cc");
}

static EXCEPTION_DISPOSITION __cdecl
HpfsSehHandler (
    PEXCEPTION_RECORD Record,
    PVOID Frame,
    PCONTEXT Context,
    PVOID DispatcherContext
    )
{
    PHPFS_SEH Seh = (PHPFS_SEH)Frame;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(DispatcherContext);

    if (Record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        return ExceptionContinueSearch;
    }
    if (!HpfsWanted(Record, Seh->ExpectedOnly)) {
        return ExceptionContinueSearch;     /* a bug: let the system stop here */
    }

    Seh->Code = HpfsCatchCode(Record);

    HpfsGlobalUnwind(Frame, Record);

    /* RtlUnwind left this frame at the head of the chain: take it off */
    WriteFs0(Seh->Reg.Next);

    __builtin_longjmp(Seh->Jmp, 1);
    return ExceptionContinueSearch;     /* not reached */
}

NTSTATUS
HpfsTryCall (
    HPFS_TRY_BODY Body,
    PVOID Context,
    BOOLEAN ExpectedOnly
    )
{
    HPFS_SEH Seh;

    Seh.ExpectedOnly = ExpectedOnly;
    Seh.Code = STATUS_SUCCESS;
    Seh.Reg.Next = (HPFS_SEH_REG *)ReadFs0();
    Seh.Reg.Handler = (PVOID)HpfsSehHandler;
    WriteFs0(&Seh.Reg);

    if (__builtin_setjmp(Seh.Jmp) == 0) {
        Body(Context);
        WriteFs0(Seh.Reg.Next);
        return STATUS_SUCCESS;
    }

    /* The handler took the frame off the chain */
    return Seh.Code;
}

#elif defined(__GNUC__) && defined(_AMD64_)

/* ------------------------------------------------------------------ */
/* GCC, amd64: a routine with a language handler in its unwind data    */
/* ------------------------------------------------------------------ */

__declspec(dllimport) VOID NTAPI RtlUnwindEx(PVOID TargetFrame, PVOID TargetIp,
                                             PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue,
                                             PCONTEXT ContextRecord, PVOID HistoryTable);

/* The leading fields of the dispatcher's DISPATCHER_CONTEXT */
typedef struct _HPFS_DISPATCHER_CONTEXT {
    ULONG64     ControlPc;
    ULONG64     ImageBase;
    PVOID       FunctionEntry;
    ULONG64     EstablisherFrame;
    ULONG64     TargetIp;
    PCONTEXT    ContextRecord;
    PVOID       LanguageHandler;
    PVOID       HandlerData;
    PVOID       HistoryTable;
} HPFS_DISPATCHER_CONTEXT, *PHPFS_DISPATCHER_CONTEXT;

/*
 * NTSTATUS HpfsTryCall(Body, Context, ExpectedOnly)
 *
 *   rsp+00..1F  home space for Body
 *   rsp+20      ExpectedOnly, read by the handler through the establisher
 *               frame (rsp after the prologue)
 */
__asm__ (
    "    .text\n"
    "    .globl  HpfsTryCall\n"
    "    .def    HpfsTryCall; .scl 2; .type 32; .endef\n"
    "    .seh_proc HpfsTryCall\n"
    "HpfsTryCall:\n"
    "    subq    $0x28, %rsp\n"
    "    .seh_stackalloc 0x28\n"
    "    .seh_endprologue\n"
    "    .seh_handler HpfsTryHandler, @except\n"
    "    movb    %r8b, 0x20(%rsp)\n"
    "    movq    %rcx, %rax\n"
    "    movq    %rdx, %rcx\n"
    "    callq   *%rax\n"
    "    xorl    %eax, %eax\n"
    "    .globl  HpfsTryResume\n"
    "HpfsTryResume:\n"
    "    nop\n"
    "    addq    $0x28, %rsp\n"
    "    ret\n"
    "    .seh_endproc\n"
);

extern UCHAR HpfsTryResume[];

EXCEPTION_DISPOSITION
HpfsTryHandler (
    PEXCEPTION_RECORD Record,
    PVOID EstablisherFrame,
    PCONTEXT Context,
    PVOID DispatcherContext
    )
{
    PHPFS_DISPATCHER_CONTEXT Dc = (PHPFS_DISPATCHER_CONTEXT)DispatcherContext;
    BOOLEAN ExpectedOnly = *((PUCHAR)EstablisherFrame + 0x20);
    NTSTATUS Code;

    UNREFERENCED_PARAMETER(Context);

    if (Record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        return ExceptionContinueSearch;
    }
    if (!HpfsWanted(Record, ExpectedOnly)) {
        return ExceptionContinueSearch;
    }

    Code = HpfsCatchCode(Record);

    /* Unwinds the frames above HpfsTryCall's and continues it after the
       call with Code in RAX; does not return */
    RtlUnwindEx(EstablisherFrame, HpfsTryResume, Record, (PVOID)(LONG_PTR)Code,
                Dc->ContextRecord, Dc->HistoryTable);

    return ExceptionContinueSearch;
}

#else

/* ------------------------------------------------------------------ */
/* Visual C++                                                          */
/* ------------------------------------------------------------------ */

static LONG
HpfsTryFilter (
    PEXCEPTION_POINTERS Pointers,
    BOOLEAN ExpectedOnly,
    PNTSTATUS Code
    )
{
    if (!HpfsWanted(Pointers->ExceptionRecord, ExpectedOnly)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    *Code = HpfsCatchCode(Pointers->ExceptionRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}

NTSTATUS
HpfsTryCall (
    HPFS_TRY_BODY Body,
    PVOID Context,
    BOOLEAN ExpectedOnly
    )
{
    NTSTATUS Code = STATUS_SUCCESS;

    __try {
        Body(Context);
    } __except (HpfsTryFilter(GetExceptionInformation(), ExpectedOnly, &Code)) {
        ;
    }
    return Code;
}

#endif

/* ------------------------------------------------------------------ */
/* Copies from and to callers' buffers                                 */
/* ------------------------------------------------------------------ */

typedef struct _HPFS_COPY {
    PVOID       Destination;
    const VOID *Source;
    SIZE_T      Length;
} HPFS_COPY;

static VOID
HpfsCopyBody (
    PVOID Context
    )
{
    HPFS_COPY *C = (HPFS_COPY *)Context;
    RtlCopyMemory(C->Destination, C->Source, C->Length);
}

NTSTATUS
HpfsSafeCopy (
    PVOID Destination,
    const VOID *Source,
    SIZE_T Length
    )
{
    HPFS_COPY C;
    NTSTATUS Code;

    C.Destination = Destination;
    C.Source = Source;
    C.Length = Length;
    Code = HpfsTryCall(HpfsCopyBody, &C, FALSE);
    return NT_SUCCESS(Code) ? STATUS_SUCCESS : HpfsExceptionStatus(Code);
}
