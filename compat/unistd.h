#ifndef MINIEAP_WIN_UNISTD_H
#define MINIEAP_WIN_UNISTD_H

#include <io.h>
#include <process.h>

/* Only the CRT facilities used by the foreground Windows port.  There is
 * deliberately no fork/setsid/alarm emulation here. */
#define getpid _getpid
#define close _close
#define read _read
#define write _write
#define unlink _unlink
#define access _access

#ifndef F_OK
#define F_OK 0
#define W_OK 2
#define R_OK 4
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

#endif
