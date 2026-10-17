/*
 * HPFSNT - HPFS boot loader (sectors 0-15 of an HPFS volume, 8 KB)
 *
 * Sector 0 is the boot sector: the BIOS (or the master boot record) loads it
 * at 0000:7C00 with DL = the boot drive. It reads sectors 1-15 behind itself
 * and continues there. That part finds NTLDR in the root directory (the
 * dnode B-tree), follows its allocation tree (fnode and anodes), loads it at
 * 2000:0000 and starts it the way the Windows NT boot sectors do:
 *
 *      DL = boot drive, DS:SI -> the BPB (07C0:000B), ES = 03E8h
 *
 * Disk access uses the INT 13h extensions when the BIOS has them (any
 * sector of a large disk), else CHS through the BPB geometry.
 *
 * Built with GNU as:  as --32 -o hpfsboot.o hpfsboot.S
 *                     ld -m elf_i386 -Ttext 0x7C00 --oformat binary -o hpfsboot.bin hpfsboot.o
 * The formatter fills in the BPB fields (bytes 3-61).
 */

        .code16
        .intel_syntax noprefix
        .text
        .globl  _start

/* BPB fields (offsets in the boot sector at 7C00h) */
#define BPB_SPT         0x7C18      /* sectors per track */
#define BPB_HEADS       0x7C1A
#define BPB_HIDDEN      0x7C1C      /* partition start (LBA) */
#define BPB_DRIVE       0x7C24

/* Work areas (0000:xxxx) */
#define SUPER_BUF       0x0500      /* one sector */
#define FNODE_BUF       0x0700      /* fnode / anode, one sector per tree level: 0700, 0900, 0B00 ... */
#define DNODE_BUF       0x1000      /* 2 KB */
#define LOAD_SEG        0x2000      /* NTLDR goes to 2000:0000 */
#define LOAD_LIMIT      0x9000      /* and must end below 9000:0000 */

_start:
        jmp     boot
        nop

        /* The BPB: written by the formatter */
        .fill   0x3E - 3, 1, 0

boot:
        cli
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     ds, ax
        mov     es, ax
        sti
        cld
        mov     [BPB_DRIVE], dl

        /* INT 13h extensions? */
        mov     byte ptr [use_ext], 0
        mov     ah, 0x41
        mov     bx, 0x55AA
        int     0x13
        jc      1f
        cmp     bx, 0xAA55
        jne     1f
        test    cl, 1
        jz      1f
        mov     byte ptr [use_ext], 1
1:
        /* Sectors 1-15 to 0000:7E00 */
        mov     eax, 1
        mov     cx, 15
        mov     bx, 0x7E00
        call    read_sectors
        jmp     stage2

/*
 * read_sectors: EAX = sector (relative to the volume), CX = count,
 * ES:BX = buffer. One sector at a time through CHS, up to 64 at a time
 * through the extensions. Preserves all registers; [save_es] is the
 * segment just past the data (ES + count * 20h).
 */
read_sectors:
        push    es
        pushad
2:
        push    eax
        push    cx
        add     eax, [BPB_HIDDEN]
        cmp     byte ptr [use_ext], 0
        je      3f

        /* Extended read: one disk address packet */
        cmp     cx, 64
        jbe     4f
        mov     cx, 64
4:
        mov     si, offset dap
        mov     word ptr [si + 2], cx
        mov     word ptr [si + 4], bx
        mov     word ptr [si + 6], es
        mov     dword ptr [si + 8], eax
        mov     dword ptr [si + 12], 0
        mov     dl, [BPB_DRIVE]
        mov     ah, 0x42
        push    cx
        int     0x13
        pop     cx
        jc      disk_error
        jmp     5f

3:      /* CHS: LBA -> C/H/S, one sector */
        xor     edx, edx
        movzx   ecx, word ptr [BPB_SPT]
        div     ecx                         /* eax = LBA / spt, edx = sector - 1 */
        inc     dx
        mov     cl, dl                      /* sector */
        xor     edx, edx
        movzx   esi, word ptr [BPB_HEADS]
        div     esi                         /* eax = cylinder, edx = head */
        cmp     eax, 1023
        ja      disk_error
        mov     dh, dl
        mov     ch, al
        shl     ah, 6
        or      cl, ah
        mov     dl, [BPB_DRIVE]
        mov     di, 5                       /* retries */
6:
        mov     ax, 0x0201
        push    di
        int     0x13
        pop     di
        jnc     7f
        xor     ah, ah
        int     0x13
        dec     di
        jnz     6b
        jmp     disk_error
7:
        mov     cx, 1
5:
        /* CX sectors were read: advance */
        mov     ax, cx
        shl     ax, 5                       /* paragraphs */
        mov     dx, es
        add     dx, ax
        mov     es, dx
        pop     dx                          /* the count asked for */
        pop     eax
        movzx   ecx, cx
        add     eax, ecx
        sub     dx, cx
        mov     cx, dx
        jnz     2b
        mov     [save_es], es
        popad
        pop     es
        ret

disk_error:
        mov     si, offset msg_disk
fatal:
        call    print
        mov     si, offset msg_restart
        call    print
        xor     ax, ax
        int     0x16
        int     0x19

print:
        lodsb
        or      al, al
        jz      8f
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        jmp     print
8:      ret

use_ext:        .byte   0
save_es:        .word   0
dap:            .byte   16, 0
                .word   0, 0, 0
                .long   0, 0
msg_disk:       .asciz  "\r\nHPFS boot: disk read error"
msg_restart:    .asciz  "\r\nPress a key to restart\r\n"

        .org    510
        .word   0xAA55

/* ================================================================== */
/* Sectors 1-15 (0000:7E00)                                           */
/* ================================================================== */

stage2:
        /* Superblock: the root fnode */
        mov     eax, 16
        mov     cx, 1
        mov     bx, SUPER_BUF
        call    read_sectors
        cmp     dword ptr [SUPER_BUF], 0xF995E849
        jne     bad_fs
        mov     eax, [SUPER_BUF + 12]
        mov     bx, FNODE_BUF
        call    read_one
        cmp     dword ptr [FNODE_BUF], 0xF7E40AAE
        jne     bad_fs
        /* The root dnode of a directory is its first extent's sector */
        mov     eax, [FNODE_BUF + 0x40 + 8]

        /* Down the dnode tree to NTLDR */
        mov     byte ptr [depth], 0
next_dnode:
        inc     byte ptr [depth]
        cmp     byte ptr [depth], 20
        ja      bad_fs
        mov     bx, DNODE_BUF
        mov     cx, 4
        call    read_sectors
        cmp     dword ptr [DNODE_BUF], 0x77E40AAE
        jne     bad_fs
        mov     si, DNODE_BUF + 20
next_entry:
        /* SI = entry: +0 length, +2 flags, +4 fnode, +30 name length, +31 name */
        mov     al, [si + 2]
        test    al, 0x08                    /* end entry: everything is below */
        jnz     go_down
        test    al, 0x01                    /* ".." */
        jnz     skip_entry
        call    compare_name                /* CF/ZF as for cmp(entry, "NTLDR") */
        je      found
        ja      go_down                     /* entry sorts after: the name is below it */
skip_entry:
        mov     ax, [si]
        cmp     ax, 32
        jb      bad_fs
        add     si, ax
        cmp     si, DNODE_BUF + 2048
        jb      next_entry
        jmp     bad_fs
go_down:
        test    byte ptr [si + 2], 0x04
        jz      not_found
        mov     bx, [si]
        mov     eax, [si + bx - 4]
        jmp     next_dnode

not_found:
        mov     si, offset msg_missing
        jmp     fatal
bad_fs:
        mov     si, offset msg_badfs
        jmp     fatal

/*
 * compare_name: compares the entry at SI with "NTLDR" in HPFS order (upper
 * case, then length). Flags as "cmp entry, target" (unsigned).
 */
compare_name:
        push    si
        push    di
        movzx   cx, byte ptr [si + 30]
        lea     si, [si + 31]
        mov     di, offset ntldr_name
        mov     dx, 5                       /* target length */
9:
        jcxz    10f
        or      dx, dx
        jz      11f                         /* target ended first: entry is longer */
        lodsb
        cmp     al, 'a'
        jb      12f
        cmp     al, 'z'
        ja      12f
        sub     al, 0x20
12:
        mov     ah, [di]
        inc     di
        cmp     al, ah
        jne     13f
        dec     cx
        dec     dx
        jmp     9b
10:     /* entry ended */
        or      dx, dx
        jz      13f                         /* both ended: equal (ZF set) */
        cmp     cx, dx                      /* 0 < dx: below */
        jmp     13f
11:     cmp     cx, dx                      /* cx > 0 = dx: above */
13:
        pop     di
        pop     si
        ret

found:
        /* NTLDR's fnode */
        mov     eax, [si + 4]
        mov     ecx, [si + 12]              /* file size */
        add     ecx, 511
        shr     ecx, 9
        mov     [remaining], ecx
        mov     bx, FNODE_BUF
        call    read_one
        cmp     dword ptr [FNODE_BUF], 0xF7E40AAE
        jne     bad_fs
        mov     ax, LOAD_SEG
        mov     [load_seg], ax
        mov     bx, FNODE_BUF + 0x38        /* the fnode's B+ tree header */
        mov     di, FNODE_BUF
        call    load_tree
        cmp     dword ptr [remaining], 0
        jne     too_short

        /* Start NTLDR as the NT boot sectors do */
        mov     dl, [BPB_DRIVE]
        mov     ax, 0x03E8
        mov     es, ax
        mov     ax, 0x07C0
        mov     ds, ax
        mov     si, 0x000B
        push    LOAD_SEG
        push    0
        retf

too_short:
        mov     si, offset msg_short
        jmp     fatal

/*
 * load_tree: BX = B+ tree header (in the sector buffer at DI), loads the
 * data of every leaf extent in order. Internal nodes: each child anode is
 * read into the next buffer (DI + 200h) and walked.
 */
load_tree:
        mov     al, [bx]                    /* flags */
        movzx   cx, byte ptr [bx + 5]       /* used count */
        lea     si, [bx + 8]
        test    al, 0x80
        jnz     internal
14:     /* leaves: file sector, length, disk sector */
        jcxz    15f
        push    cx
        push    si
        mov     eax, [si + 8]
        mov     ecx, [si + 4]
        call    load_extent
        pop     si
        pop     cx
        add     si, 12
        dec     cx
        jmp     14b
15:     ret

internal:
        jcxz    15b
        push    cx
        push    si
        push    di
        mov     eax, [si + 4]               /* down: an anode */
        add     di, 0x200
        cmp     di, FNODE_BUF + 0x200 * 6
        jae     bad_fs
        mov     bx, di
        call    read_one
        cmp     dword ptr [di], 0x37E40AAE
        jne     bad_fs
        lea     bx, [di + 12]               /* anode tree header */
        call    load_tree
        pop     di
        pop     si
        pop     cx
        add     si, 8
        dec     cx
        jmp     internal

/* load_extent: EAX = disk sector, ECX = sectors; at most [remaining] go in */
load_extent:
        cmp     ecx, [remaining]
        jbe     16f
        mov     ecx, [remaining]
16:
        or      ecx, ecx
        jz      17f
        sub     [remaining], ecx
18:
        /* chunks that stay inside one 64 KB block of memory */
        mov     dx, [load_seg]
        cmp     dx, LOAD_LIMIT
        jae     too_big
        push    eax
        push    ecx
        mov     ax, dx
        and     ax, 0x0FFF                  /* paragraphs into this 64 KB */
        mov     bx, 0x1000
        sub     bx, ax
        shr     bx, 5                       /* sectors to its end */
        jnz     19f
        mov     bx, 1
19:
        movzx   ebx, bx
        cmp     ecx, ebx
        jbe     20f
        mov     cx, bx
20:
        mov     es, dx
        mov     dx, cx                      /* sectors this time */
        pop     ecx
        pop     eax
        push    ecx
        push    eax
        mov     cx, dx
        push    dx
        xor     bx, bx
        call    read_sectors
        mov     bx, [save_es]
        mov     [load_seg], bx
        pop     dx
        pop     eax
        pop     ecx
        movzx   edx, dx
        add     eax, edx
        sub     ecx, edx
        jnz     18b
17:     xor     ax, ax
        mov     es, ax
        ret

too_big:
        mov     si, offset msg_big
        jmp     fatal

/* read_one: EAX = sector, 0000:BX */
read_one:
        push    es
        xor     cx, cx
        mov     es, cx
        mov     cx, 1
        call    read_sectors
        pop     es
        ret

depth:          .byte   0
load_seg:       .word   0
remaining:      .long   0
ntldr_name:     .ascii  "NTLDR"
msg_missing:    .asciz  "\r\nHPFS boot: NTLDR is missing"
msg_badfs:      .asciz  "\r\nHPFS boot: the file system is damaged"
msg_short:      .asciz  "\r\nHPFS boot: NTLDR is damaged"
msg_big:        .asciz  "\r\nHPFS boot: NTLDR is too large"
                .ascii  "\r\nHPFSNT boot loader"

        .org    16 * 512
