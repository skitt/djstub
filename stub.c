/*
 *  go32-compatible COFF stub.
 *  Copyright (C) 2022,  stsp <stsp@users.sourceforge.net>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <dos.h>
#include <dpmi.h>
#include "stubinfo.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#ifndef PAGE_MASK
#define PAGE_MASK	(~(PAGE_SIZE-1))
#endif
/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	(((addr)+PAGE_SIZE-1)&PAGE_MASK)

#define STUB_DEBUG 0
#if STUB_DEBUG
#define stub_debug(...) printf(__VA_ARGS__)
#else
#define stub_debug(...)
#endif

typedef struct
{
    uint32_t offset32;
    unsigned short selector;
} DPMI_FP;

struct coff_header {
    unsigned short 	f_magic;	/* Magic number */	
    unsigned short 	f_nscns;	/* Number of Sections */
    int32_t 		f_timdat;	/* Time & date stamp */
    int32_t 		f_symptr;	/* File pointer to Symbol Table */
    int32_t 		f_nsyms;	/* Number of Symbols */
    unsigned short 	f_opthdr;	/* sizeof(Optional Header) */
    unsigned short 	f_flags;	/* Flags */
};

struct opt_header {
    unsigned short 	magic;          /* Magic Number                    */
    unsigned short 	vstamp;         /* Version stamp                   */
    uint32_t 		tsize;          /* Text size in bytes              */
    uint32_t 		dsize;          /* Initialised data size           */
    uint32_t 		bsize;          /* Uninitialised data size         */
    uint32_t 		entry;          /* Entry point                     */
    uint32_t 		text_start;     /* Base of Text used for this file */
    uint32_t 		data_start;     /* Base of Data used for this file */
};

struct scn_header {
    char		s_name[8];	/* Section Name */
    int32_t		s_paddr;	/* Physical Address */
    int32_t		s_vaddr;	/* Virtual Address */
    int32_t		s_size;		/* Section Size in Bytes */
    int32_t		s_scnptr;	/* File offset to the Section data */
    int32_t		s_relptr;	/* File offset to the Relocation table for this Section */
    int32_t		s_lnnoptr;	/* File offset to the Line Number table for this Section */
    unsigned short	s_nreloc;	/* Number of Relocation table entries */
    unsigned short	s_nlnno;	/* Number of Line Number table entries */
    int32_t		s_flags;	/* Flags for this section */
};

enum { SCT_TEXT, SCT_DATA, SCT_BSS, SCT_MAX };

static struct scn_header scns[SCT_MAX];
static unsigned short psp_sel;
static unsigned short cs_sel;
static unsigned short ds_sel;
static char __far *client_memory;
static DPMI_FP clnt_entry;
static _GO32_StubInfo stubinfo;

static void dpmi_init(void)
{
    union REGPACK r = {};
    void __far (*sw)(void);
    unsigned mseg = 0, f;
    int err;

#define CF 1
    r.w.ax = 0x1687;
    intr(0x2f, &r);
    if ((r.w.flags & CF) || r.w.ax != 0) {
        fprintf(stderr, "DPMI unavailable\n");
        exit(EXIT_FAILURE);
    }
    if (!(r.w.bx & 1)) {
        fprintf(stderr, "DPMI-32 unavailable\n");
        exit(EXIT_FAILURE);
    }
    sw = MK_FP(r.w.es, r.w.di);
    if (r.w.si) {
        err = _dos_allocmem(r.w.si, &mseg);
        if (err) {
            fprintf(stderr, "malloc of %i para failed\n", r.w.si);
            exit(EXIT_FAILURE);
        }
    }
    asm volatile(
        "mov %5, %%es\n"
        "lcall *%6\n"
        "pushf\n"
        "pop %0\n"
        "mov %%es, %1\n"
        "mov %%cs, %2\n"
        "mov %%ds, %3\n"
        "push %%ds\n"
        "pop %%es\n"
        : "=r"(f), "=r"(psp_sel), "=r"(cs_sel), "=r"(ds_sel)
        : "a"(1), "r"(mseg), "m"(sw)
        : "cc", "memory");
    if (f & CF) {
        fprintf(stderr, "DPMI init failed\n");
        exit(EXIT_FAILURE);
    }
}

static long _long_read(int handle, char __far *buf, unsigned long offs,
                       long size)
{
    unsigned ret_hi, ret_lo, dummy;
    int c;
    asm volatile(
          "push %%ds\n"
          "mov %%di, %%ds\n"
          "mov %[size], %%cx\n"
          "data32 rol $16, %%cx\n"
          "mov 2+%[size], %%cx\n"
          "data32 rol $16, %%cx\n"
          "add %[offs], %%dx\n"
          "data32 rol $16, %%dx\n"
          "mov 2+%[offs], %%dx\n"
          "data32 rol $16, %%dx\n"
          "int $0x21\n"
          "pop %%ds\n"
          ".byte 0x0f, 0x92, 0xc3\n" // setc %%bl
          "movb $0, %%bh\n"
          "mov %%ax, %0\n"
          "data32 shr $16, %%ax\n"
          "data32 xor %%cx, %%cx\n"  // clear high part of ecx
          "data32 xor %%dx, %%dx\n"  // clear high part of edx
        : "=r"(ret_lo), "=a"(ret_hi), "=b"(c), "=c"(dummy), "=d"(dummy)
        : "a"(0x3f00), "b"(handle), "D"(FP_SEG(buf)), "d"(FP_OFF(buf)),
          [size]"m"(size), [offs]"m"(offs)
        : "cc", "memory");
    return (c ? -1 : (((long)ret_hi << 16) | ret_lo));
}

static void read_section(FILE *ifile, long coffset, int sc)
{
    long bytes;
    fseek(ifile, coffset + scns[sc].s_scnptr, SEEK_SET);
    bytes = _long_read(fileno(ifile), client_memory, scns[sc].s_vaddr,
            scns[sc].s_size);
    stub_debug("read returned %li\n", bytes);
    if (bytes != scns[sc].s_size) {
        fprintf(stderr, "err reading %li bytes, got %li\n",
                scns[sc].s_size, bytes);
        _exit(EXIT_FAILURE);
    }
}

static void farmemset_bss(void)
{
    char __far *p = client_memory;
    uint32_t size = scns[SCT_BSS].s_size;
    unsigned dummy;
    asm volatile(
          "push %%es\n"
          "mov %%dx, %%es\n"
          "mov %[size], %%cx\n"
          "data32 rol $16, %%cx\n"
          "mov 2+%[size], %%cx\n"
          "data32 rol $16, %%cx\n"
          "add %[offs], %%di\n"
          "data32 rol $16, %%di\n"
          "mov 2+%[offs], %%di\n"
          "data32 rol $16, %%di\n"
          "data32 shr $1, %%cx\n"
          "addr32 rep stosw\n"
          "pop %%es\n"
          "data32 xor %%cx, %%cx\n"
          "data32 xor %%di, %%di\n"
        : "=c"(dummy), "=D"(dummy)
        : "a"(0), "d"(FP_SEG(p)), "D"(FP_OFF(p)),
          [size]"m"(size), [offs]"m"(scns[SCT_BSS].s_vaddr)
        : "memory");
}

static char *_basename(char *name)
{
    char *p, *p1;
    p = strrchr(name, '\\');
    if (!p)
        p = name;
    else
        p++;
    p1 = strrchr(p, '.');
    if (p1)
        p1[0] = '\0';
    return p;
}

int main(int argc, char *argv[], char *envp[])
{
    FILE *ifile;
    off_t coffset = 0;
    long coff_file_size;
    int rc, i;
    char buf[16];
    struct coff_header chdr;
    struct opt_header ohdr;
    int done = 0;
    unsigned short clnt_ds;
    unsigned short stubinfo_fs;
    unsigned short mem_hi, mem_lo, si, di;
    unsigned long alloc_size;
    unsigned long stubinfo_mem;
    dpmi_dos_block db;
    char *argv0 = strdup(argv[0]);

    if (argc == 0) {
        fprintf(stderr, "no env\n");
        exit(EXIT_FAILURE);
    }
    dpmi_init();

    ifile = fopen(argv[0], "r");
    if (!ifile) {
        fprintf(stderr, "cannot open %s\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    while (!done) {
        int cnt = 0;

        stub_debug("Expecting header at %lx\n", coffset);
        rc = fread(buf, 1, 6, ifile);
        if (rc != 6) {
            perror("fread()");
            exit(EXIT_FAILURE);
        }
        if (buf[0] == 'M' && buf[1] == 'Z') {
            long blocks = (unsigned char)buf[4] + (unsigned char)buf[5] * 256;
            long partial = (unsigned char)buf[2] + (unsigned char)buf[3] * 256;

            cnt++;
            stub_debug("Found exe header %i at %lx\n", cnt, coffset);
            coffset += blocks * 512;
            if (partial)
                coffset += partial - 512;
        } else if (buf[0] == 0x33 && buf[1] == 0x50) { /* CauseWay 3P */
            cnt++;
            stub_debug("Found CW header %i at %lx\n", cnt, coffset);
            coffset += *(uint32_t *)&buf[2];
        } else if (buf[0] == 0x4c && buf[1] == 0x01) { /* it's a COFF */
            done = 1;
        } else {
            fprintf(stderr, "not an exe %s at %lx\n", argv[0], coffset);
            exit(EXIT_FAILURE);
        }
        fseek(ifile, coffset, SEEK_SET);
    }

    fseek(ifile, 0, SEEK_END);
    coff_file_size = ftell(ifile) - coffset;
    fseek(ifile, coffset, SEEK_SET);
    if (coff_file_size < sizeof(chdr) + sizeof(ohdr) + sizeof(scns)) {
        fprintf(stderr, "bad COFF payload, size %lx off %lx\n",
                coff_file_size, coffset);
        exit(EXIT_FAILURE);
    }
    fread(&chdr, sizeof(chdr), 1, ifile); /* get the COFF header */
    if (chdr.f_opthdr != sizeof(ohdr)) {
        fprintf(stderr, "bad COFF header\n");
        exit(EXIT_FAILURE);
    }
    fread(&ohdr, sizeof(ohdr), 1, ifile); /* get the COFF opt header */
    fread(scns, sizeof(scns[0]), SCT_MAX, ifile);
#if STUB_DEBUG
    for (i = 0; i < SCT_MAX; i++) {
        struct scn_header *h = &scns[i];
        stub_debug("Section %s pa %lx va %lx size %lx foffs %lx\n",
                h->s_name, h->s_paddr, h->s_vaddr, h->s_size, h->s_scnptr);
    }
#endif
    strncpy(stubinfo.magic, "go32stub,v3,stsp", sizeof(stubinfo.magic));
    stubinfo.size = sizeof(stubinfo);
    i = 0;
    while(*envp) {
        i += strlen(*envp) + 1;
        envp++;
    }
    if (i) {
        i += strlen(argv0) + 1;
        i += 2;
    }
    stub_debug("env size %i\n", i);
    stubinfo.env_size = i;
    stubinfo.minstack = 0x80000;
    stubinfo.minkeep = 0x4000;
    strncpy(stubinfo.argv0, argv0, sizeof(stubinfo.argv0));
    strncpy(stubinfo.basename, _basename(argv0), sizeof(stubinfo.basename));
    strncpy(stubinfo.dpmi_server, "CWSDPMI.EXE", sizeof(stubinfo.dpmi_server));
#define max(a, b) ((a) > (b) ? (a) : (b))
    stubinfo.initial_size = max(scns[SCT_BSS].s_vaddr + scns[SCT_BSS].s_size,
        0x10000);
    stubinfo.psp_selector = psp_sel;
    /* DJGPP relies on ds_selector, cs_selector and ds_segment all mapping
     * the same real-mode memory block. */
    db = _DPMIAllocateDOSMemoryBlock(stubinfo.minkeep >> 4);
    stubinfo.ds_selector = db.pm;
    stubinfo.ds_segment = db.rm;
    /* create alias */
    asm volatile("int $0x31\n"
        : "=a"(stubinfo.cs_selector)
        : "a"(0xa), "b"(db.pm));
    /* set descriptor access rights */
    asm volatile("int $0x31\n"
        :
        : "a"(9), "b"(stubinfo.cs_selector), "c"(0x00fb)
        : "cc");

    clnt_entry.selector = _DPMIAllocateLDTDescriptors(1);
    clnt_entry.offset32 = ohdr.entry;
    clnt_ds = _DPMIAllocateLDTDescriptors(1);
    alloc_size = PAGE_ALIGN(stubinfo.initial_size);
    /* allocate mem */
    asm volatile("int $0x31\n"
        : "=b"(mem_hi), "=c"(mem_lo), "=S"(si), "=D"(di)
        : "a"(0x501),
          "b"((uint16_t)(alloc_size >> 16)),
          "c"((uint16_t)(alloc_size & 0xffff))
        : "cc");
    stubinfo.memory_handle = ((uint32_t)si << 16) | di;
    client_memory = MK_FP(clnt_ds, 0);
    /* set base */
    asm volatile("int $0x31\n"
        :
        : "a"(7), "b"(clnt_entry.selector), "c"(mem_hi), "d"(mem_lo)
        : "cc");
    /* set descriptor access rights */
    asm volatile("int $0x31\n"
        :
        : "a"(9), "b"(clnt_entry.selector), "c"(0xc0fb)
        : "cc");
    /* set limit */
    asm volatile("int $0x31\n"
        :
        : "a"(8), "b"(clnt_entry.selector),
          "c"((uint16_t)(stubinfo.initial_size >> 16)),
          "d"(0xffff)
        : "cc");
    /* set base */
    asm volatile("int $0x31\n"
        :
        : "a"(7), "b"(clnt_ds), "c"(mem_hi), "d"(mem_lo)
        : "cc");
    /* set descriptor access rights */
    asm volatile("int $0x31\n"
        :
        : "a"(9), "b"(clnt_ds), "c"(0xc0f3)
        : "cc");
    /* set limit */
    asm volatile("int $0x31\n"
        :
        : "a"(8), "b"(clnt_ds),
          "c"((uint16_t)(stubinfo.initial_size >> 16)),
          "d"(0xffff)
        : "cc");

    /* create alias */
    asm volatile("int $0x31\n"
        : "=a"(stubinfo_fs)
        : "a"(0xa), "b"(ds_sel));
    stubinfo_mem = _DPMIGetSegmentBaseAddress(ds_sel) + (uintptr_t)&stubinfo;
    mem_hi = stubinfo_mem >> 16;
    mem_lo = stubinfo_mem & 0xffff;
    /* set base */
    asm volatile("int $0x31\n"
        :
        : "a"(7), "b"(stubinfo_fs), "c"(mem_hi), "d"(mem_lo)
        : "cc");

    read_section(ifile, coffset, SCT_TEXT);
    read_section(ifile, coffset, SCT_DATA);
    farmemset_bss();
    fclose(ifile);

    asm volatile(
          ".byte 0x8e, 0xe0\n"  // mov %%ax, %%fs
          "push %%ds\n"
          "pop %%es\n"
          "mov %1, %%ds\n"
          "data32 ljmp *%2\n"
        :
        : "a"(stubinfo_fs), "r"(clnt_ds), "m"(clnt_entry)
        : "memory");
    return 0;
}
