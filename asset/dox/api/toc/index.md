# mulle-spawn Library Documentation for AI
<!-- Keywords: spawn, subprocess, process, capture, timeout, pipe, cross-platform -->
## 1. Introduction & Purpose

`mulle-spawn` is a small, cross-platform C library for getting a second process
without `fork()`, `system()` or a shell. It provides two shapes of the same
spawn:

- `mulle_spawn_run` **runs a program and waits**, capturing its `stdout` into a
  buffer under a wall-clock deadline. It returns the child's fate: exit code,
  signal, output.
- `mulle_spawn_open` **starts a program and keeps it**, returning a handle whose
  fields are descriptors for the child's standard streams and its pid. The child
  stays alive until the caller terminates and closes it.

The problem it solves is that `fork()` cannot be abstracted over POSIX and
Windows, and that the interesting failure mode of a child is not a wrong answer
but *no answer at all*. The API is identical on both platforms (POSIX
`fork`/`execvp` and Windows `CreateProcessW` underneath), down to which bytes
reach the caller.

It is a component of the `mulle-core` umbrella library; include it as
`<mulle-core/mulle-core.h>` or standalone as `<mulle-spawn/mulle-spawn.h>`.

## 2. Key Concepts & Design Philosophy

- **Two calls, one spawn.** `run` is the spawn plus waiting and capturing;
  `open` is the spawn without it. The difference is the whole shape of the API.
- **Deadline is unconditional.** There is no cooperative shutdown: when the
  timeout elapses the child is killed (`TerminateProcess` on Windows, `SIGKILL`
  on POSIX). Output and termination are independent -- a killed child's earlier
  output is still returned.
- **A handle deliberately does not capture.** A pipe has one reader and the
  caller is it. `wait` and `close` report the fate and leave `result.output`
  NULL.
- **Modes describe stream plumbing.** `MULLE_SPAWN_INHERIT`, `MULLE_SPAWN_PIPE`,
  `MULLE_SPAWN_DISCARD` and `MULLE_SPAWN_DESCRIPTOR` say what becomes of the
  child's stdin/stdout/stderr. Note the child's point of view: `in` is what the
  child reads, `out` is what it writes.
- **`options` grows.** `struct mulle_spawn_options` carries environment,
  working directory, per-stream descriptors and the Windows process group. A
  zeroed struct *is* the default, so `mulle_spawn_open` is exactly
  `mulle_spawn_open_with_options` with a zeroed struct.
- **`argv` is passed through as written.** No globbing, no redirection, no
  `PATH` search beyond `execvp`/`CreateProcessW` semantics. On Windows it is
  quoted for `CreateProcessW`, so paths with spaces work.
- **Platform differences are reconciled, not exposed.** A program that cannot
  be executed arrives as `mulle_spawn_run` returning -1 with
  `mulle_spawn_failed`; Windows `\r\n` is reduced to `\n` in captured output.
  `signal_number` is POSIX by definition and stays 0 where there are no signals.

## 3. Core API & Data Structures

All signatures below are copied verbatim from `mulle-spawn.h`.

### 3.1. `mulle-spawn.h`

#### `enum mulle_spawn_status`

```c
enum mulle_spawn_status
{
   mulle_spawn_unknown    = 0,   /* nothing was run (or the result was reset) */
   mulle_spawn_failed     = 1,   /* no child process was created at all */
   mulle_spawn_exited     = 2,   /* the child exited on its own, see exit_code */
   mulle_spawn_signalled  = 3,   /* POSIX: it died from signal_number */
   mulle_spawn_terminated = 4    /* it outlived the deadline and was killed */
};
```

#### `struct mulle_spawn_result`

- **Purpose:** the fate of a child: captured output and how it ended.
- **Key Fields:**
  - `char *output` -- captured bytes, allocated with `allocator`, owned by the
    struct. NUL terminated for convenience; `length` is the truth.
  - `size_t length` -- number of captured bytes.
  - `int status` -- one of `enum mulle_spawn_status`.
  - `int exit_code` -- valid when `status == mulle_spawn_exited`.
  - `int signal_number` -- POSIX signal; 0 where there are no signals.
  - `struct mulle_allocator *allocator` -- the allocator `output` was made with.
- **Lifecycle Functions:**
  - `void mulle_spawn_result_init( struct mulle_spawn_result *p, struct mulle_allocator *allocator);`
    -- initializes; `allocator` may be NULL for the default.
  - `void mulle_spawn_result_done( struct mulle_spawn_result *p);`
    -- releases `output` and resets the struct, keeping the allocator.
- **Note:** an empty capture is `output == NULL, length == 0`.

#### Version Functions

```c
#define MULLE__SPAWN_VERSION  ((1UL << 20) | (0 << 8) | 0)

static inline unsigned int   mulle_spawn_get_version_major( void);
static inline unsigned int   mulle_spawn_get_version_minor( void);
static inline unsigned int   mulle_spawn_get_version_patch( void);
uint32_t                     mulle_spawn_get_version( void);
```

#### `mulle_spawn_run` -- bounded capture

```c
int   mulle_spawn_run( char * const *argv,
                       mulle_relativetime_t timeout,
                       struct mulle_spawn_result *p);
```

Runs `argv[0]` (resolved with `execvp` semantics, so `PATH` is honored),
replacing its `stdout` with a pipe and capturing it. `stdin` and `stderr` are
inherited unchanged. The child runs to completion or until `timeout` seconds
elapse; a negative `timeout` means no deadline (`MULLE_SPAWN_NO_TIMEOUT`).
Returns 0 if a child was created (fate in `p`), -1 if not (then `p->status` is
`mulle_spawn_failed` and `p->output` is NULL).

```c
/* run forever */
#define MULLE_SPAWN_NO_TIMEOUT   (-1.0)
```

#### `mulle_spawn_get_executable_path`

```c
int   mulle_spawn_get_executable_path( char *buf, size_t length);
```

Writes the running executable's path as UTF-8 into `buf`. Returns the length of
the path, or -1 if it does not fit or cannot be determined. This is the "run me
again" case; the result is the OS's resolved path, typically not the symlink
name it was called by.

#### Stream Mode Macros

```c
#define MULLE_SPAWN_INHERIT     0   /* the parent's own stream, untouched */
#define MULLE_SPAWN_PIPE        1   /* a pipe, and a descriptor in the handle */
#define MULLE_SPAWN_DISCARD     2   /* the null device, for output nobody wants */
#define MULLE_SPAWN_DESCRIPTOR  3   /* the descriptor named alongside, as it is */
```

#### `struct mulle_spawn_options`

- **Purpose:** everything `open` needs besides the program and its arguments.
  A zeroed struct is the default.
- **Key Fields:**
  - `char * const *env` -- the child's environment as NULL-terminated
    `NAME=VALUE` strings (`execve`'s `envp`). It *replaces* the parent's
    environment: `NULL` means "the parent's", `{ NULL }` means "none". Not
    copied; must stay valid until `open` returns. A replaced `PATH` is also the
    one searched for `argv[0]`.
  - `char const *directory` -- the child's working directory, or NULL for the
    inherited one. A missing directory fails the launch. Not copied; must stay
    valid until `open` returns.
  - `int in_descriptor; int out_descriptor; int err_descriptor;` -- the
    caller's own descriptor per stream, used when the matching mode is
    `MULLE_SPAWN_DESCRIPTOR`. -1 is not open. The descriptor stays the caller's;
    `close` does not close it.
  - `int new_process_group` -- Windows: give the child its own console process
    group (`CREATE_NEW_PROCESS_GROUP`). Required for `mulle_spawn_interrupt` on
    Windows. Opt-in because the child then stops handling Ctrl+C itself.
  - `int in_mode; int out_mode; int err_mode;` -- the stream modes.
- **Lifecycle:**
  - `void mulle_spawn_options_init( struct mulle_spawn_options *p);`
    -- sets `env`/`directory` NULL, descriptors -1, all modes
    `MULLE_SPAWN_INHERIT`.

#### `struct mulle_spawn_handle`

- **Purpose:** a running child and the ends of its streams.
- **Key Fields:**
  - `int in; int out; int err;` -- descriptors owned by the handle; `close`
    closes them. If you close one yourself, set the field to -1 so it is not
    closed twice.
  - `unsigned long pid` -- the OS process id; 0 once reaped.
  - `int opened` -- nonzero if a child was opened.
  - `void *platform[4]` -- opaque backend state; do not touch.
- **Lifecycle:**
  - `void mulle_spawn_handle_init( struct mulle_spawn_handle *h);`
    -- makes a handle that was never opened; `close` is safe on it.

#### `mulle_spawn_open` / `mulle_spawn_open_with_options`

```c
int   mulle_spawn_open( struct mulle_spawn_handle *h,
                        char * const *argv,
                        int in_mode,
                        int out_mode,
                        int err_mode);

int   mulle_spawn_open_with_options( struct mulle_spawn_handle *h,
                                     char * const *argv,
                                     struct mulle_spawn_options const *options);
```

Starts `argv` and returns 0, or -1 if the program could not be run at all.
Nothing is left open on failure, the handle is reset, and on POSIX `errno` says
why. By the time `open` returns 0 the program *is* running. `options` may be
NULL for defaults; `mulle_spawn_open` is this call with a zeroed struct.

#### Handle Operations

```c
int   mulle_spawn_wait( struct mulle_spawn_handle *h,
                        mulle_relativetime_t timeout,
                        struct mulle_spawn_result *p);

int   mulle_spawn_terminate( struct mulle_spawn_handle *h);
int   mulle_spawn_interrupt( struct mulle_spawn_handle *h);
int   mulle_spawn_suspend( struct mulle_spawn_handle *h);
int   mulle_spawn_resume( struct mulle_spawn_handle *h);
int   mulle_spawn_close( struct mulle_spawn_handle *h,
                         struct mulle_spawn_result *p);
int   mulle_spawn_detach( struct mulle_spawn_handle *h);
```

- `wait` -- waits up to `timeout` seconds for the child to stop by itself.
  Returns 1 if stopped (and reaps it), 0 if still running, -1 on error.
  `p` may be NULL. `MULLE_SPAWN_NO_TIMEOUT` waits as long as it takes.
- `terminate` -- asks the child to stop: POSIX `SIGTERM`, Windows
  `TerminateProcess`. Does not wait (terminate, wait, close).
- `interrupt` -- cooperative stop: POSIX `SIGINT`, Windows `CTRL_BREAK_EVENT`
  aimed at the child's process group, requiring `options.new_process_group`;
  without it returns -1 with `errno` `ENOTSUP`.
- `suspend` / `resume` -- POSIX `SIGSTOP`/`SIGCONT`; Windows lazily resolves
  `NtSuspendProcess`/`NtResumeProcess`, returning -1 with `ENOTSUP` if
  unavailable.
- `close` -- releases descriptors, process and backend state. A still-running
  child is terminated unconditionally and reaped first. `p` may be NULL.
- `detach` -- lets the child go alive: closes the handle's descriptors but does
  not terminate and does not wait. Read `h->pid` *before* detaching; the handle
  is reset exactly as `close` leaves it.

#### Internal

```c
int   _mulle_spawn_append( struct mulle_spawn_result *p,
                           size_t *capacity,
                           char *bytes,
                           size_t length);
```

Appends to the capture, growing as needed. Shared by the platform backends;
not for caller use.

## 4. Performance Characteristics

- `run` is blocking: it reads the child's `stdout` to EOF and enforces the
  deadline. Cost is dominated by the child, not by the API.
- Captured output grows geometrically via `_mulle_spawn_append`; amortized
  O(1) per byte appended.
- The library is **not thread-safe** as such. A `result` or `handle` belongs to
  one caller; there is no internal locking. Separate handles are independent.
- `open` returns only once the program is running, so no polling is needed to
  distinguish "cannot start" from "started and died".
- `wait` with a `timeout` may poll internally; with `MULLE_SPAWN_NO_TIMEOUT` it
  blocks until the child stops.
- `terminate`, `suspend` and `resume` do not wait, so they are effectively O(1).

## 5. AI Usage Recommendations & Patterns

- **Always use the lifecycle functions.** `mulle_spawn_result_init` before a
  run, `mulle_spawn_result_done` after; `mulle_spawn_close` on every opened
  handle. `close` is safe on a handle initialized but never opened.
- **Check the return value, not the exit code, for "could it run".** A child
  that cannot be executed is `run == -1` with `mulle_spawn_failed` (Windows has
  no exit code for it).
- **Do not free `result.output` yourself** -- hand the result to
  `mulle_spawn_result_done`.
- **A handle does not capture.** Do not expect `output` from `wait` or `close`
  on an opened handle; read the descriptors yourself.
- **Set a descriptor field to -1 if you close it yourself**, or `close` will
  close a reused descriptor number.
- **Wait before closing** when a child must flush what it printed: `close`
  terminates a running child, and a killed child never flushes.
- **`MULLE_SPAWN_DESCRIPTOR` descriptors stay yours**; the child gets a copy and
  the handle does not own them.
- **Read `h->pid` before `detach`** -- detach empties the handle.
- **On Windows, `interrupt` needs `options.new_process_group`**, otherwise it
  returns -1 with `ENOTSUP` rather than signalling your own console group.
- **Treat `argv` as literal words**: no shell, no globbing, no redirection.

## 6. Integration Examples

### Example 1: Run a program and read what it said

```c
#include <mulle-spawn/mulle-spawn.h>
#include <stdio.h>

struct mulle_spawn_result   r;
char                        *argv[ 3];
int                         rval;

argv[ 0] = "/usr/bin/uname";
argv[ 1] = "-s";
argv[ 2] = NULL;

mulle_spawn_result_init( &r, NULL);
rval = mulle_spawn_run( argv, 5.0, &r);

if( rval == 0)
{
   printf( "status %d, exit %d, said <%.*s>\n",
           r.status, r.exit_code, (int) r.length,
           r.output ? r.output : "");
}

mulle_spawn_result_done( &r);
```

### Example 2: A child that may not come back

```c
struct mulle_spawn_result   r;
char                        *argv[ 3];

argv[ 0] = "/usr/bin/sleep";
argv[ 1] = "30";
argv[ 2] = NULL;

mulle_spawn_result_init( &r, NULL);
mulle_spawn_run( argv, 1.0, &r);

if( r.status == mulle_spawn_terminated)
{
   /* it was killed; whatever it printed before is still in r.output */
}

mulle_spawn_result_done( &r);
```

### Example 3: Start a program and keep talking to it

```c
struct mulle_spawn_handle   h;
char                        buf[ 256];
char                        *argv[ 3];
int                         n;

argv[ 0] = "/usr/bin/cat";
argv[ 1] = NULL;
argv[ 2] = NULL;

if( mulle_spawn_open( &h, argv, MULLE_SPAWN_PIPE, MULLE_SPAWN_PIPE,
                      MULLE_SPAWN_INHERIT) == 0)
{
   n = (int) write( h.in, "PING\r\n", 6);
   n = (int) read( h.out, buf, sizeof( buf));

   mulle_spawn_terminate( &h);
   mulle_spawn_wait( &h, 5.0, NULL);
   mulle_spawn_close( &h, NULL);
}
```

### Example 4: Giving the child an environment and a directory

```c
struct mulle_spawn_options   options;
struct mulle_spawn_handle    h;
char                         *env[ 2];
char                         *argv[ 3];

env[ 0] = "PATH=/usr/bin:/bin";
env[ 1] = NULL;

argv[ 0] = "/usr/bin/env";
argv[ 1] = NULL;
argv[ 2] = NULL;

mulle_spawn_options_init( &options);
options.env       = env;
options.directory = "/tmp";
options.out_mode  = MULLE_SPAWN_PIPE;

if( mulle_spawn_open_with_options( &h, argv, &options) == 0)
{
   mulle_spawn_wait( &h, 5.0, NULL);
   mulle_spawn_close( &h, NULL);
}
```

### Example 5: A child that outlives the handle

```c
struct mulle_spawn_handle   h;
char                        *argv[ 3];
unsigned long               pid;

argv[ 0] = "/usr/bin/daemon";
argv[ 1] = NULL;
argv[ 2] = NULL;

if( mulle_spawn_open( &h, argv, MULLE_SPAWN_INHERIT, MULLE_SPAWN_DISCARD,
                      MULLE_SPAWN_INHERIT) == 0)
{
   pid = h.pid;               /* read it first: detach empties the handle */
   mulle_spawn_detach( &h);   /* descriptors closed; child neither killed nor reaped */
}
```

## 7. Dependencies

- `mulle-c11`
- `mulle-allocator` (supplies `struct mulle_allocator` used by `mulle_spawn_result`)
- `mulle-time` (supplies `mulle_relativetime_t` used by timeouts)
