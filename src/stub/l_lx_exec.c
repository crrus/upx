/* l_lx_exec.c -- generic stub loader for Linux using execve()

   This file is part of the UPX executable compressor.

   Copyright (C) 1996-2004 Markus Franz Xaver Johannes Oberhumer
   Copyright (C) 1996-2004 Laszlo Molnar
   All Rights Reserved.

   UPX and the UCL library are free software; you can redistribute them
   and/or modify them under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.
   If not, write to the Free Software Foundation, Inc.,
   59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

   Markus F.X.J. Oberhumer              Laszlo Molnar
   <mfx@users.sourceforge.net>          <ml1050@users.sourceforge.net>

   John F. Reiser
   <jreiser@users.sourceforge.net>
 */


#include "linux.hh"


/*************************************************************************
// configuration section
**************************************************************************/

// mmap() the temporary output file
#define USE_MMAP_FO


/*************************************************************************
// file util
**************************************************************************/

struct Extent {
    int  size;  // must be first to match size[0] uncompressed size
    char *buf;
};


#if !defined(USE_MMAP_FO)
#if 1
static __inline__ int xwrite(int fd, const void *buf, int count)
{
    // note: we can assert(count > 0);
    do {
        int n = write(fd, buf, count);
        if (n == -EINTR)
            continue;
        if (n <= 0)
            break;
        buf += n;               // gcc extension: add to void *
        count -= n;
    } while (count > 0);
    return count;
}
#else
#define xwrite(fd,buf,count)    ((count) - write(fd,buf,count))
#endif
#endif /* !USE_MMAP_FO */


/*************************************************************************
// util
**************************************************************************/

#if 1

extern char *
__attribute__ ((regparm(2), stdcall))  // be ruthless
upx_itoa(unsigned long v, char *buf);

#else

// Some versions of gcc optimize the division and/or remainder using
// a multiplication by (2**32)/10, and use a relocatable 32-bit address
// to reference the constant.  We require no relocations because we move
// the code at runtime.  See upx_itoa.asm for replacement [also smaller.]
static char *upx_itoa(unsigned long v, char *buf)
{
//    const unsigned TEN = 10;
    volatile unsigned TEN = 10;  // an ugly way to achieve no relocation
    char *p = buf;
    {
        unsigned long k = v;
        do {
            p++;
            k /= TEN;
        } while (k > 0);
    }
    buf = p;
    *p = 0;
    {
        unsigned long k = v;
        do {
            *--p = '0' + k % TEN;
            k /= TEN;
        } while (k > 0);
    }
    return buf;
}

#endif


static uint32_t ascii5(char *p, uint32_t v, unsigned n)
{
    do {
        unsigned char d = v % 32;
        if (d >= 26) d -= 43;       // 43 == 'Z' - '0' + 1
        *--p = (d += 'A');
        v /= 32;
    } while (--n > 0);
    return v;
}


static char *
do_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    (void)len; (void)prot; (void)flags; (void)fd; (void)offset;
    return mmap((void *)&addr);
}


#if defined(__i386__)
#  define SET2(p, c0, c1) \
        * (unsigned short *) (p) = ((c1)<<8 | (c0))
#  define SET4(p, c0, c1, c2, c3) \
        * (uint32_t *) (p) = ((c3)<<24 | (c2)<<16 | (c1)<<8 | (c0))
#  define SET3(p, c0, c1, c2) \
        SET4(p, c0, c1, c2, 0)
#else
#  define SET2(p, c0, c1) \
        (p)[0] = c0, (p)[1] = c1
#  define SET3(p, c0, c1, c2) \
        (p)[0] = c0, (p)[1] = c1, (p)[2] = c2
#  define SET4(p, c0, c1, c2, c3) \
        (p)[0] = c0, (p)[1] = c1, (p)[2] = c2, (p)[3] = c3
#endif


// go_self is a separate subroutine to spread the burden of local arrays.
// Otherwise the size of the stack frame in upx_main exceeds 128 bytes,
// which causes too many offsets to expand from 1 byte to 4.

static int
go_self(char const *tmpname, char *argv[], char *envp[])
{
    // Old FreeBSD does not have /proc/self, so use /proc/<pid> instead.

    // Open the temp file.
    int const fdi = open(tmpname, O_RDONLY, 0);

    if (0 <= fdi) {
        // 17 chars for "/proc/PPPPP/fd/XX" should be enough, but we
        // play safe in case there will be 32-bit pid_t at some time.
        //char procself_buf[17+1];
        char procself_buf[31+1];

        // Compute name of temp fdi.
        SET4(procself_buf + 0, '/', 'p', 'r', 'o');
        SET4(procself_buf + 4, 'c', '/',  0 ,  0 );
        {
            char *const procself = upx_itoa(getpid(), procself_buf + 6);
            SET4(procself, '/', 'f', 'd', '/');
            upx_itoa(fdi, procself + 4);
        }

        // Check for working /proc/self/fd/X by accessing the
        // temp file again, now via temp fdi.
        if (UPX2 == access(procself_buf, R_OK | X_OK)) {
            // Now it's safe to unlink the temp file (as it is still open).
            unlink(tmpname);
            // Set the file close-on-exec.
            fcntl(fdi, F_SETFD, FD_CLOEXEC);
            // Execute the original program via /proc/self/fd/X.
            execve(procself_buf, argv, envp);
            // NOTE: if we get here we've lost.
        }

        // The proc filesystem isn't working. No problem.
        close(fdi);
    }
    return fdi;
}

/*************************************************************************
// UPX & NRV stuff
**************************************************************************/

typedef void f_unfilter(
    nrv_byte *,  // also addvalue
    nrv_uint,
    unsigned cto8  // junk in high 24 bits
);
typedef int f_expand(
    const nrv_byte *src, nrv_uint  src_len,
          nrv_byte *dst, nrv_uint *dst_len );


/*************************************************************************
// upx_main - called by our entry code
//
// This function is optimized for size.
**************************************************************************/

void upx_main(
    f_unfilter *const f_unf,
    unsigned cprLen,
    f_expand *const f_decompress,
    int junk2,
    char /*const*/ *cprSrc,
    char *envp[],
    char *argv[],
    int argc
) __asm__("upx_main");
void upx_main(
    f_unfilter *const f_unf,
    unsigned cprLen,
    f_expand *const f_decompress,
    int junk,
    char /*const*/ *cprSrc,
    char *envp[],
    char *argv[],
    int argc
)
{
    // file descriptor
    int fdo;

    // decompression buffer
    unsigned char *buf;

    char *tmpname;

    struct Extent xi = { cprLen, cprSrc };

    char *next_unmap = (char *)(PAGE_MASK & (unsigned)xi.buf);
    struct p_info header;

    // temporary file name
    char tmpname_buf[20];

    (void)junk;

    //
    // ----- Step 0: set /proc/self using /proc/<pid> -----
    //

    //personality(PER_LINUX);


    //
    // ----- Step 1: prepare input file -----
    //

    // Read header.
    {
        register char *__d0, *__d1;
        __asm__ __volatile__( "movsl; movsl; movsl"
            : "=&D" (__d0), "=&S" (__d1)
            : "0" (&header), "1" (xi.buf)
            : "memory");
        xi.buf   = __d1;
        xi.size -= sizeof(header);
    }

    // Paranoia. Make sure this is actually our expected executable
    // by checking the random program id. (The id is both stored
    // in the header and patched into this stub.)
    if (header.p_progid != UPX3)
        goto error1;


    //
    // ----- Step 2: prepare temporary output file -----
    //

    tmpname = tmpname_buf;
    SET4(tmpname + 0, '/', 't', 'm', 'p');
    SET4(tmpname + 4, '/', 'u', 'p', 'x');

    // Compute name of temporary output file in tmpname[].
    // Protect against Denial-of-Service attacks.
    {
        char *p = tmpname_buf + sizeof(tmpname_buf) - 1;

        // Compute the last 4 characters (20 bits) from getpid().
        uint32_t r = ascii5(p, (uint32_t)getpid(), 4); *p = '\0'; p -= 4;

        // Provide 4 random bytes from our program id.
        r ^= header.p_progid;
        // Mix in 4 runtime random bytes.
        // Don't consume precious bytes from /dev/urandom.
        {
#if 1
            struct timeval tv;
            gettimeofday(&tv, 0);
            r ^= (uint32_t) tv.tv_sec;
            r ^= ((uint32_t) tv.tv_usec) << 12;      // shift into high-bits
#else
            // using adjtimex() may cause portability problems
            struct timex tx;
            adjtimex(&tx);
            r ^= (uint32_t) tx.time.tv_sec;
            r ^= ((uint32_t) tx.time.tv_usec) << 12; // shift into high-bits
            r ^= (uint32_t) tx.errcnt;
#endif
        }
        // Compute 7 more characters from the 32 random bits.
        ascii5(p, r, 7);
    }

    // Just in case, remove the file.
    {
        int err = unlink(tmpname);
        if (err != -ENOENT && err != 0)
            goto error1;
    }

    // Create the temporary output file.
#if defined(USE_MMAP_FO)
    fdo = open(tmpname, O_RDWR | O_CREAT | O_EXCL, 0700);
#else
    fdo = open(tmpname, O_WRONLY | O_CREAT | O_EXCL, 0700);
#endif
#if 0
    // Save some bytes of code - the ftruncate() below will fail anyway.
    if (fdo < 0)
        goto error;
#endif

    // Set expected uncompressed file size.
    if (ftruncate(fdo, header.p_filesize) != 0)
        goto error;


    //
    // ----- Step 3: setup memory -----
    //

#if defined(USE_MMAP_FO)
    // FIXME: packer could set length
    buf = do_mmap(0, header.p_filesize,
        PROT_READ | PROT_WRITE, MAP_SHARED, fdo, 0);
    if ((unsigned long) buf >= (unsigned long) -4095)
        goto error;

    // Decompressor can overrun the output by 3 bytes.
    // Defend against SIGSEGV by using a scratch page.
    // FIXME: packer could set address delta
    do_mmap(buf + (PAGE_MASK & (header.p_filesize + ~PAGE_MASK)),
        -PAGE_MASK, PROT_READ | PROT_WRITE,
        MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0 );
#else
    // Temporary decompression buffer.
    // FIXME: packer could set length
    buf = do_mmap(0, (header.p_blocksize + OVERHEAD + ~PAGE_MASK) & PAGE_MASK,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0 );
    if ((unsigned long) buf >= (unsigned long) -4095)
        goto error;
#endif

    //
    // ----- Step 4: decompress blocks -----
    //

    for (;;)
    {
        struct b_info h;
        int i;

        // Read and check block sizes.
        {
            register char *__d0, *__d1;
            __asm__ __volatile__( "movsl; movsl; movsl"
                : "=&D" (__d0), "=&S" (__d1)
                : "0" (&h), "1" (xi.buf)
                : "memory");
            xi.buf   = __d1;
            xi.size -= sizeof(h);
        }
        if (h.sz_unc == 0)                      // uncompressed size 0 -> EOF
        {
            if (h.sz_cpr != UPX_MAGIC_LE32)     // h.sz_cpr must be h->magic
                goto error;
            if (header.p_filesize != 0)         // all bytes must be written
                goto error;
            break;
        }
        //   Note: if sz_unc == sz_cpr then the block was not
        //   compressible and is stored in its uncompressed form.

        if (h.sz_cpr > h.sz_unc || h.sz_cpr > header.p_blocksize)
            goto error;
        // Now we have:
        //   assert(h.sz_cpr <= h.sz_unc);
        //   assert(h.sz_unc > 0 && h.sz_unc <= blocksize);
        //   assert(h.sz_cpr > 0 && h.sz_cpr <= blocksize);

        if (h.sz_cpr < h.sz_unc) { // Decompress block.
            nrv_uint out_len;
            i = (*f_decompress)(xi.buf, h.sz_cpr, buf, &out_len);
            if (i != 0 || out_len != (nrv_uint)h.sz_unc)
                goto error;
            // Right now, unfilter is combined with decompression.
            // (*f_unfilter)(buf, out_len, cto8);
            (void)f_unf;
        }
        else
        {
            // Incompressible block
#if defined(USE_MMAP_FO)
            //memcpy(buf, xi.buf, h.sz_unc);
            register unsigned long int __d0, __d1, __d2;
            __asm__ __volatile__( "rep; movsb"
                : "=&c" (__d0), "=&D" (__d1), "=&S" (__d2)
                : "0" (h.sz_unc), "1" (buf), "2" (xi.buf)
                : "memory");
#endif
        }

#if defined(USE_MMAP_FO)
        // unmap part of the output
        munmap(buf, h.sz_unc);
        buf     += h.sz_unc;
#else
        // write output file
        if (xwrite(fdo, buf, h.sz_unc) != 0)
            goto error;
#endif

        header.p_filesize -= h.sz_unc;

        xi.buf  += h.sz_cpr;
        xi.size -= h.sz_cpr;

        if (xi.size < 0) {
// error exit is here in the middle to keep the jumps short.
        error:
            (void) unlink(tmpname);
        error1:
            // Note: the kernel will close all open files and
            //       unmap any allocated memory.
            for (;;)
                (void) exit(127);
        }

        // We will never touch these pages again.
        i = (PAGE_MASK & (unsigned)xi.buf) - (unsigned)next_unmap;
        munmap(next_unmap, i);
        next_unmap += i;
    }


    //
    // ----- Step 5: release resources -----
    //


#if !defined(USE_MMAP_FO)
    // Free our temporary decompression buffer.
    munmap(buf, malloc_args.ma_length);
#endif

    if (close(fdo) != 0)
        goto error;


    //
    // ----- Step 6: try to start program via /proc/self/fd/X -----
    //

    // Many thanks to Andi Kleen <ak@muc.de> and
    // Jamie Lokier <nospam@cern.ch> for this nice idea.

    if (0 > go_self(tmpname, argv, envp))
        goto error;


    //
    // ----- Step 7: start program in /tmp  -----
    //

    // Fork off a subprocess to clean up.
    // We have to do this double-fork trick to keep a zombie from
    // hanging around if the spawned original program doesn't check for
    // subprocesses (as well as to prevent the real program from getting
    // confused about this subprocess it shouldn't have).
    // Thanks to Adam Ierymenko <api@one.net> for this solution.

    if (fork() == 0)
    {
        if (fork() == 0)
        {
            // Sleep 3 seconds, then remove the temp file.
            struct timespec ts; ts.tv_sec = UPX4; ts.tv_nsec = 0;
            nanosleep(&ts, 0);
            unlink(tmpname);
        }
        exit(0);
    }

    // Wait for the first fork()'d process to die.
    waitpid(-1, (int *)0, 0);

    // Execute the original program.
    (void)argc;
    execve(tmpname, argv, envp);


    //
    // ----- Step 8: error exit -----
    //

    // If we return from execve() there was an error. Give up.
    goto error;
}


/*
vi:ts=4:et:nowrap
*/
