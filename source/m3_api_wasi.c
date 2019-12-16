//
//  m3_api_wasi.c
//
//  Created by Volodymyr Shymanskyy on 11/20/19.
//  Copyright © 2019 Volodymyr Shymanskyy. All rights reserved.
//

#include "m3_api_wasi.h"

#include "m3_core.h"
#include "m3_env.h"
#include "m3_module.h"
#include "m3_exception.h"

#if defined(d_m3HasWASI)

#include "extra/wasi_core.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/random.h>

#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>

# if defined(__APPLE__) || defined(__ANDROID_API__) || defined(__OpenBSD__)
#   include <unistd.h>
# elif defined(_WIN32)
/* See http://msdn.microsoft.com/en-us/library/windows/desktop/aa387694.aspx */
#   define SystemFunction036 NTAPI SystemFunction036
#   include <NTSecAPI.h>
#   undef SystemFunction036
# endif

//TODO
#define PREOPEN_CNT   3
#define NANOS_PER_SEC 1000000000

typedef uint32_t __wasi_size_t;

struct wasi_iovec
{
    __wasi_size_t iov_base;
    __wasi_size_t iov_len;
};

typedef struct Preopen {
    char       *path;
    unsigned   path_len;
} Preopen;

Preopen preopen[PREOPEN_CNT] = {
    { .path = "<stdin>",  .path_len = 7, },
    { .path = "<stdout>", .path_len = 8, },
    { .path = "<stderr>", .path_len = 8, },
    //{ .path = "./",       .path_len = 2, }, //TODO
    //{ .path = "../",      .path_len = 3, },
    //{ .path = "/",        .path_len = 1, },
    //{ .path = "/tmp",     .path_len = 4, },
};

static
__wasi_errno_t errno_to_wasi(int errnum) {
    switch (errnum) {
    case EPERM:   return __WASI_EPERM;   break;
    case ENOENT:  return __WASI_ENOENT;  break;
    case ESRCH:   return __WASI_ESRCH;   break;
    case EINTR:   return __WASI_EINTR;   break;
    case EIO:     return __WASI_EIO;     break;
    case ENXIO:   return __WASI_ENXIO;   break;
    case E2BIG:   return __WASI_E2BIG;   break;
    case ENOEXEC: return __WASI_ENOEXEC; break;
    case EBADF:   return __WASI_EBADF;   break;
    case ECHILD:  return __WASI_ECHILD;  break;
    case EAGAIN:  return __WASI_EAGAIN;  break;
    case ENOMEM:  return __WASI_ENOMEM;  break;
    case EACCES:  return __WASI_EACCES;  break;
    case EFAULT:  return __WASI_EFAULT;  break;
    case EBUSY:   return __WASI_EBUSY;   break;
    case EEXIST:  return __WASI_EEXIST;  break;
    case EXDEV:   return __WASI_EXDEV;   break;
    case ENODEV:  return __WASI_ENODEV;  break;
    case ENOTDIR: return __WASI_ENOTDIR; break;
    case EISDIR:  return __WASI_EISDIR;  break;
    case EINVAL:  return __WASI_EINVAL;  break;
    case ENFILE:  return __WASI_ENFILE;  break;
    case EMFILE:  return __WASI_EMFILE;  break;
    case ENOTTY:  return __WASI_ENOTTY;  break;
    case ETXTBSY: return __WASI_ETXTBSY; break;
    case EFBIG:   return __WASI_EFBIG;   break;
    case ENOSPC:  return __WASI_ENOSPC;  break;
    case ESPIPE:  return __WASI_ESPIPE;  break;
    case EROFS:   return __WASI_EROFS;   break;
    case EMLINK:  return __WASI_EMLINK;  break;
    case EPIPE:   return __WASI_EPIPE;   break;
    case EDOM:    return __WASI_EDOM;    break;
    case ERANGE:  return __WASI_ERANGE;  break;
    default:      return errno;
    }
}

static inline
uint32_t addr2offset(IM3Runtime m, void *addr) {
    return (u8*)addr - m->memory.wasmPages;
}

static inline
void *offset2addr(IM3Runtime m, uint32_t offset) {
    return m->memory.wasmPages + offset;
}

static
void copy_iov_to_host(struct iovec* host_iov, IM3Runtime runtime, uint32_t iov_offset, int32_t iovs_len)
{
    // Convert wasi_memory offsets to host addresses
    struct wasi_iovec *wasi_iov = offset2addr(runtime, iov_offset);
    for (int i = 0; i < iovs_len; i++) {
        host_iov[i].iov_base = offset2addr(runtime, wasi_iov[i].iov_base);
        host_iov[i].iov_len  = wasi_iov[i].iov_len;
    }
}


/*
 * WASI API implementation
 */


uint32_t m3_wasi_unstable_args_get (IM3Runtime runtime,
                                    u32 *       argv_offset,
                                    u8 *        argv_buf_offset)
{
    if (runtime)
    {
        for (u32 i = 0; i < runtime->argc; ++i)
        {
            argv_offset [i] = addr2offset (runtime, argv_buf_offset);
            
            size_t len = strlen (runtime->argv [i]);
            memcpy (argv_buf_offset, runtime->argv [i], len);
            argv_buf_offset += len;
            * argv_buf_offset++ = 0;
        }
        
        return __WASI_ESUCCESS;
    }
    else return __WASI_EINVAL;
}

uint32_t m3_wasi_unstable_args_sizes_get (IM3Runtime        runtime,
                                          __wasi_size_t *   argc,
                                          __wasi_size_t *   argv_buf_size)
{
    if (runtime == NULL) { return __WASI_EINVAL; }

    *argc = runtime->argc;
    *argv_buf_size = 0;
    for (int i = 0; i < runtime->argc; ++i)
        * argv_buf_size += strlen (runtime->argv [i]) + 1;
    
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_environ_get(IM3Runtime runtime,
                                      uint32_t   environ_ptrs_offset,
                                      uint32_t   environ_strs_offset)
{
    if (runtime == NULL) { return __WASI_EINVAL; }
    // TODO
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_environ_sizes_get(IM3Runtime runtime,
                                            uint32_t   environ_count_offset,
                                            uint32_t   environ_buf_size_offset)
{
    if (runtime == NULL) { return __WASI_EINVAL; }
    __wasi_size_t *environ_count    = offset2addr(runtime, environ_count_offset);
    __wasi_size_t *environ_buf_size = offset2addr(runtime, environ_buf_size_offset);
    *environ_count = 0; // TODO
    *environ_buf_size = 0; // TODO
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_fd_prestat_dir_name(IM3Runtime runtime,
                                              uint32_t   fd,
                                              uint32_t   path_offset,
                                              uint32_t   path_len)
{
    if (runtime == NULL) { return __WASI_EINVAL; }
    if (fd < 3 || fd >= PREOPEN_CNT) { return __WASI_EBADF; }
    memmove((char *)offset2addr(runtime, path_offset), preopen[fd].path,
            min(preopen[fd].path_len, path_len));
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_fd_prestat_get(IM3Runtime runtime,
                                         uint32_t   fd,
                                         uint32_t   buf_offset)
{
    if (runtime == NULL) { return __WASI_EINVAL; }
    if (fd < 3 || fd >= PREOPEN_CNT) { return __WASI_EBADF; }
    *(uint32_t *)offset2addr(runtime, buf_offset) = __WASI_PREOPENTYPE_DIR;
    *(uint32_t *)offset2addr(runtime, buf_offset+4) = preopen[fd].path_len;
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_fd_fdstat_get(IM3Runtime       runtime,
                                        __wasi_fd_t      fd,
                                        __wasi_fdstat_t* fdstat)
{
    if (runtime == NULL || fdstat == NULL) { return __WASI_EINVAL; }

    struct stat fd_stat;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) { return errno_to_wasi(errno); }
    fstat(fd, &fd_stat);
    int mode = fd_stat.st_mode;
    fdstat->fs_filetype = (S_ISBLK(mode)   ? __WASI_FILETYPE_BLOCK_DEVICE     : 0) |
                          (S_ISCHR(mode)   ? __WASI_FILETYPE_CHARACTER_DEVICE : 0) |
                          (S_ISDIR(mode)   ? __WASI_FILETYPE_DIRECTORY        : 0) |
                          (S_ISREG(mode)   ? __WASI_FILETYPE_REGULAR_FILE     : 0) |
                          //(S_ISSOCK(mode)  ? __WASI_FILETYPE_SOCKET_STREAM    : 0) |
                          (S_ISLNK(mode)   ? __WASI_FILETYPE_SYMBOLIC_LINK    : 0);
    fdstat->fs_flags = ((fl & O_APPEND)    ? __WASI_FDFLAG_APPEND    : 0) |
                       //((fl & O_DSYNC)     ? __WASI_FDFLAG_DSYNC     : 0) |
                       ((fl & O_NONBLOCK)  ? __WASI_FDFLAG_NONBLOCK  : 0) |
                       //((fl & O_RSYNC)     ? __WASI_FDFLAG_RSYNC     : 0) |
                       ((fl & O_SYNC)      ? __WASI_FDFLAG_SYNC      : 0);
    fdstat->fs_rights_base = (uint64_t)-1; // all rights
    fdstat->fs_rights_inheriting = (uint64_t)-1; // all rights
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_fd_seek(IM3Runtime          runtime,
                                  __wasi_fd_t         fd,
                                  __wasi_filedelta_t  offset,
                                  __wasi_whence_t     whence,
                                  __wasi_filesize_t*  result)
{
    if (runtime == NULL || result == NULL) { return __WASI_EINVAL; }

    int wasi_whence = whence == __WASI_WHENCE_END ? SEEK_END :
                                __WASI_WHENCE_CUR ? SEEK_CUR : 0;
    int64_t ret;
#if defined(M3_COMPILER_MSVC)
    ret = _lseeki64(fd, offset, wasi_whence);
#else
    ret = lseek(fd, offset, wasi_whence);
#endif
    if (ret < 0) { return errno_to_wasi(errno); }
    *result = ret;
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_fd_read(IM3Runtime    runtime,
                                  __wasi_fd_t   fd,
                                  uint32_t      iovs_offset,
                                  __wasi_size_t iovs_len,
                                  __wasi_size_t* nread)
{
    if (runtime == NULL || nread == NULL) { return __WASI_EINVAL; }

    struct iovec iovs[iovs_len];
    copy_iov_to_host(iovs, runtime, iovs_offset, iovs_len);

    ssize_t ret = readv(fd, iovs, iovs_len);
    if (ret < 0) { return errno_to_wasi(errno); }
    *nread = ret;
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_fd_write(IM3Runtime    runtime,
                                   __wasi_fd_t   fd,
                                   uint32_t      iovs_offset,
                                   __wasi_size_t iovs_len,
                                   __wasi_size_t* nwritten)
{
    if (runtime == NULL || nwritten == NULL) { return __WASI_EINVAL; }

    struct iovec iovs[iovs_len];
    copy_iov_to_host(iovs, runtime, iovs_offset, iovs_len);

    ssize_t ret = writev(fd, iovs, iovs_len);
    if (ret < 0) { return errno_to_wasi(errno); }
    *nwritten = ret;
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_fd_close(uint32_t fd)
{
    int ret = close(fd);
    return ret == 0 ? __WASI_ESUCCESS : ret;
}

uint32_t m3_wasi_unstable_fd_datasync(uint32_t fd)
{
    int ret = fdatasync(fd);
    return ret == 0 ? __WASI_ESUCCESS : ret;
}

uint32_t m3_wasi_unstable_random_get(void* buf, __wasi_size_t buflen)
{
    while (1) {
        ssize_t retlen = 0;

#if defined(__wasi__)
        retlen = getentropy(buf, buflen);
        if (retlen == 0) {
            retlen = buflen;
        }
#elif defined(__APPLE__) || defined(__ANDROID_API__) || defined(__OpenBSD__)
        size_t pos = 0;
        for (; pos + 256 < buflen; pos += 256) {
            if (getentropy((char *)buf + pos, 256)) {
                return errno_to_wasi(errno);
            }
        }
        if (getentropy((char *)buf + pos, buflen - pos)) {
            return errno_to_wasi(errno);
        }
        return __WASI_ESUCCESS;

#elif defined(__NetBSD__)
        // TODO
        // sysctl(buf, buflen)
#elif defined(__FreeBSD__) || defined(__linux__)
        retlen = getrandom(buf, buflen, 0);
#elif defined(_WIN32)
        if (RtlGenRandom(buf, buflen) == TRUE) {
            return __WASI_ESUCCESS;
        }
#else
        // use syscall ?
        abort (); // unsupport
        retlen = -1;
#endif
        if (retlen < 0) {
            if (errno == EINTR) { continue; }
            return errno_to_wasi(errno);
        }
        if (retlen == buflen) { return __WASI_ESUCCESS; }
        buf = (void *)((uint8_t *)buf + retlen);
        buflen -= retlen;
    }
}

uint32_t m3_wasi_unstable_clock_res_get(IM3Runtime          runtime,
                                        __wasi_clockid_t    clock_id,
                                        __wasi_timestamp_t* resolution)
{
    if (runtime == NULL || resolution == NULL) { return __WASI_EINVAL; }

    struct timespec tp;
    if (clock_getres(clock_id, &tp) != 0)
        *resolution = 1000000;
    else
        *resolution = (tp.tv_sec * NANOS_PER_SEC) + tp.tv_nsec;
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_clock_time_get(IM3Runtime          runtime,
                                         __wasi_clockid_t    clock_id,
                                         __wasi_timestamp_t  precision,
                                         __wasi_timestamp_t* time)
{
    if (runtime == NULL || time == NULL) { return __WASI_EINVAL; }

    struct timespec tp;
    if (clock_gettime(clock_id, &tp) != 0) { return errno_to_wasi(errno); }

    //printf("=== time: %lu.%09u\n", tp.tv_sec, tp.tv_nsec);
    *time = (uint64_t)tp.tv_sec * NANOS_PER_SEC + tp.tv_nsec;
    return __WASI_ESUCCESS;
}

uint32_t m3_wasi_unstable_proc_exit(uint32_t code)
{
    exit(code);
}


static
M3Result SuppressLookupFailure(M3Result i_result)
{
    if (i_result == c_m3Err_functionLookupFailed)
        return c_m3Err_none;
    else
        return i_result;
}


M3Result  m3_LinkWASI  (IM3Module module)
{
    M3Result result = c_m3Err_none;

_   (SuppressLookupFailure (m3_LinkFunction (module, "args_sizes_get",      "i(R**)",       &m3_wasi_unstable_args_sizes_get)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "environ_sizes_get",   "i(Rii)",       &m3_wasi_unstable_environ_sizes_get)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "args_get",            "i(R**)",       &m3_wasi_unstable_args_get)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "environ_get",         "i(Rii)",       &m3_wasi_unstable_environ_get)));

_   (SuppressLookupFailure (m3_LinkFunction (module, "fd_prestat_dir_name",  "i(Riii)",     &m3_wasi_unstable_fd_prestat_dir_name)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "fd_prestat_get",       "i(Rii)",      &m3_wasi_unstable_fd_prestat_get)));

_   (SuppressLookupFailure (m3_LinkFunction (module, "fd_fdstat_get",       "i(Ri*)",       &m3_wasi_unstable_fd_fdstat_get)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "fd_write",            "i(Riii*)",     &m3_wasi_unstable_fd_write)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "fd_read",             "i(Riii*)",     &m3_wasi_unstable_fd_read)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "fd_seek",             "i(Riii*)",     &m3_wasi_unstable_fd_seek)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "fd_datasync",         "i(i)",         &m3_wasi_unstable_fd_datasync)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "fd_close",            "i(i)",         &m3_wasi_unstable_fd_close)));

//_   (SuppressLookupFailure (m3_LinkFunction (module, "sock_send",     "i(Riii*)",    &...)));
//_   (SuppressLookupFailure (m3_LinkFunction (module, "sock_recv",     "i(Riii*)",    &...)));

_   (SuppressLookupFailure (m3_LinkFunction (module, "random_get",          "v(*i)",        &m3_wasi_unstable_random_get)));

_   (SuppressLookupFailure (m3_LinkFunction (module, "clock_res_get",       "v(Ri*)",       &m3_wasi_unstable_clock_res_get)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "clock_time_get",      "v(RiI*)",      &m3_wasi_unstable_clock_time_get)));
_   (SuppressLookupFailure (m3_LinkFunction (module, "proc_exit",           "v(i)",         &m3_wasi_unstable_proc_exit)));

_catch:
    return result;
}

#else  // d_m3HasWASI

M3Result  m3_LinkWASI  (IM3Module module)
{
    return c_m3Err_none;
}

#endif // d_m3HasWASI

