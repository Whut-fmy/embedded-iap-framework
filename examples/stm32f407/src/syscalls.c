#include <sys/stat.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern char _end;
extern char _estack;

int _close(int file)
{
    (void)file;
    errno = EBADF;
    return -1;
}

int _fstat(int file, struct stat *status)
{
    (void)file;
    if (status == NULL) {
        errno = EFAULT;
        return -1;
    }
    status->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

int _lseek(int file, int offset, int whence)
{
    (void)file;
    (void)offset;
    (void)whence;
    return 0;
}

int _read(int file, char *buffer, int length)
{
    (void)file;
    (void)buffer;
    (void)length;
    errno = ENOSYS;
    return -1;
}

int _write(int file, const char *buffer, int length)
{
    (void)file;
    (void)buffer;
    (void)length;
    errno = ENOSYS;
    return -1;
}

void *_sbrk(ptrdiff_t increment)
{
    static char *heap_end;
    char *previous;

    if (heap_end == NULL) {
        heap_end = &_end;
    }
    if (heap_end + increment >= &_estack) {
        errno = ENOMEM;
        return (void *)-1;
    }

    previous = heap_end;
    heap_end += increment;
    return previous;
}

int _getpid(void)
{
    return 1;
}

int _kill(int pid, int signal)
{
    (void)pid;
    (void)signal;
    errno = EINVAL;
    return -1;
}

void _exit(int status)
{
    (void)status;
    for (;;) {
    }
}
