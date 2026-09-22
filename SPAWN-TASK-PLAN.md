# mulle-spawn as the NSTask backbone

The plan for deleting the process-spawning half of `NSTask` and putting it in
`mulle-spawn`, where it is one implementation per platform instead of two
per-platform implementations that do not know about each other.

Written 2026-09-22, when `mulle-spawn` had just had its own repository and test
suite made, on top of a tree that still carries it as a constituent.

## Where this starts

- `mulle-spawn` is at `mulle-core/mulle-spawn/`, *not* in the demo, and it is
  still a mulle-core constituent: a `src/mulle-spawn` node in
  `mulle-core/.mulle/etc/sourcetree/config`, entries in
  `cmake/reflect/_Sources.cmake` / `_Headers.cmake`, a
  `# include <mulle-spawn/mulle-spawn.h>` in the umbrella header, and a
  verbatim copy of its sources under `mulle-core/src/mulle-spawn/`. Taking it
  out of the umbrella was tried and reverted as its own decision; step 0 is
  where it gets decided properly, and until then **every change to it is a
  change in two places**, the constituent and the amalgamated copy.
- The redis harness is the only consumer that exists. It reaches
  `struct mulle_spawn_handle` through the umbrella header today, which is the
  part that has to change.
- The customer this plan is *for* is `NSTask`, and `NSTask` lives in
  `/home/src/srcO/MulleFoundation` — a different sourcetree, in different
  repositories (`MulleObjCOSBaseFoundation`, `MulleObjCPosixFoundation`,
  `MulleObjCWindowsFoundation`, plus the `Linux` / `BSD` / `Darwin` / `FreeBSD`
  categories).

## The goal, as one sentence

`MulleObjCPosixFoundation` and `MulleObjCWindowsFoundation` end up with no
`fork`, `vfork`, `execve`, `waitpid`, `kill`, `environ`, `CreateProcessW`,
`CreatePipe`, `STARTUPINFO`, `SetHandleInformation` or `ntdll` import of their
own: `NSTask` keeps the Objective-C and calls `mulle-spawn`.

That is checkable, so it should be the acceptance criterion:

    grep -rn "fork\|execv\|waitpid\|kill(\|environ\|CreateProcess\|CreatePipe\|STARTUPINFO" \
        MulleObjCPosixFoundation/src MulleObjCWindowsFoundation/src

The answer at the end of this plan is "no hits", and the suite in
`MulleObjCOSFoundation/test` still passes.

## How much code this is about

Line counts as read off on 2026-09-22 (approximate, from the files themselves):

| file | now | after | what goes |
|---|---|---|---|
| `MulleObjCPosixFoundation/NSTask+Posix.m` | 531 | ~130 | `envp` built from `NSDictionary`, the descriptor `fds[2][3]` juggling, `vfork` + `chdir` + `close`/`dup` + `execve`, both `waitpid` calls, `kill`, `normalizedStatus`, `trace_launch` |
| `MulleObjCWindowsFoundation/NSTask+Windows.m` | 426 | ~60 | `_buildCommandLineString`, `_createWideCString`, `_buildEnvironmentBlock`, all of `launch` (`CreatePipe`, `SetHandleInformation`, `STARTUPINFO`, `CreateProcessW`), `WaitForSingleObject`, `GetExitCodeProcess`, the ntdll resolver for `NtSuspendProcess`/`NtResumeProcess` |
| `MulleObjCOSBaseFoundation/NSTask+System.m` | 600 | ~455 | the `mulleDataSystemCallWithArguments:environment:workingDirectory:standardInputData:options:` orchestrator: 166 lines, of which about 25 are the actual work |
| `NSTask+Linux.m`, `+Darwin.m`, `+FreeBSD.m` | 168 | 0 | `_environment`, which exists only to hand `execve` the parent's `char **` |
| `MulleObjCOSBaseFoundation/NSTask.m` | 208 | 208 | nothing. `dealloc`, the accessors, `launchedTaskWithLaunchPath:arguments:` — this is the class |
| `NSTask-Private.h` | 48 | ~44 | `_NSTaskIsIdle` / `_NSTaskHasFailedLaunching` / `_NSTaskIsPresumablyRunning` / `_NSTaskHasTerminated` collapse onto `mulle_spawn_status` |

Roughly 1000 of 1930 lines, and every line of it is the part that differs per
platform.

## What stays in Objective-C, on purpose

- `NSPipe`, `NSFileHandle`, and the private `_fileDescriptorForReading` /
  `_fileDescriptorForWriting` accessors. Spawn takes descriptors; it has no
  business knowing about objects, and the object layer is what makes the public
  API pleasant.
- The `NSThread` pumping in `NSTask+System.m` (the two functions that write
  `stdin` and read `stderr` on their own thread while `stdout` is read on this
  one). That is the ObjC half of a very old design, and it is fine ObjC.
- `terminationHandler`, KVO on the properties, `MulleObjCFuture`, and throwing
  instead of returning `errno`. Spawn reports; Foundation throws.
- `mulleFindExecutable:inPATH:` — `NSTask` wants the resolved path *before*
  launching so it can name it in the error. This may later become
  `mulle_spawn_which()`, but it does not have to.

## Step 0 — a home that the customer can reach

Prerequisite, not a nicety. `MulleObjCPosixFoundation` and
`MulleObjCWindowsFoundation` are separate repositories in a separate
sourcetree; they can depend on *a repository*, not on a directory inside
`mulle-core-demo`. Two consequences:

- `mulle-spawn` needs to be its own fetchable repository (its `.git` already
  exists and has no commits), and the demo consumes it the way any other
  project would — one `MULLE_SPAWN_SOURCE_DIR` beside the existing
  `MULLE_CORE_SOURCE_DIR`.
- The demo is not a git repository yet either. A `mulle-spawn/.git` sitting
  inside it means a later `git init` + `git add` treats it as an embedded
  repository and will not track its contents.

**Gate:** `clib:mulle-core/mulle-spawn` resolves, the demo builds against the
checkout, `mulle-sde test run` passes in the spawn tree.

Nothing later in this plan is worth doing before this is true: improvements to
a library the customer cannot fetch are improvements to nothing.

## Step 1 — the four launch inputs mulle-spawn does not have  *[done]*

`mulle_spawn_open( h, argv, in_mode, out_mode, err_mode)` could not express
three of the things `NSTask` sets before a launch, and it was missing one mode:

| `NSTask` | in spawn now |
|---|---|
| `setEnvironment:` | **done** — `options.env`, replacing rather than merging |
| `setCurrentDirectoryPath:` | **done** — `options.directory`, checked before anything is created |
| `setStandardInput/Output/Error:` with *any* `NSFileHandle` (a file, `/dev/null`, a pipe someone else made) | **done** — `MULLE_SPAWN_DESCRIPTOR` and one descriptor per stream |
| `NSTask` dealloc | **done** — `mulle_spawn_detach` lets the child run, which `close` will not |

So: a parameter struct with `env` and `directory`, a fourth stdio mode
`MULLE_SPAWN_DESCRIPTOR` that takes a caller-supplied descriptor (the
`dup2` from a descriptor instead of from a pipe), and a `mulle_spawn_detach`
that closes the descriptors and forgets the child without killing or reaping
it. `mulle_spawn_open` stays as a wrapper over the new call so the existing
tests and the redis harness do not move.

**Gate:** `test/10-api/spawn.c` grows a case per input — an environment that
*replaces* rather than merges, a working directory, a descriptor redirected to
a file and to the null device, and a detached child that outlives its handle —
with `.stdout` goldens on Linux and Windows and a `.darwin` one if it differs.
`mulle_spawn_run` is *not* touched: the demo uses it, and it is not on this
plan's critical path.

**Done:** `env`, as `struct mulle_spawn_options` plus
`mulle_spawn_open_with_options`, with `mulle_spawn_open` left as a wrapper over
it so no existing caller moved. `NULL` means the parent's environment, a
non-NULL array means that and nothing else, `{ NULL }` means none. macOS
replaces it through `_NSGetEnviron()`, Windows through a UTF-16 block for
`CreateProcessW`; Linux and macOS assert the contents exactly, and Windows
asserts that handing one over ends the inheritance, because wine drops any
block (see `probe/`).

`directory`, as `options.directory`, entered before the program is looked up so
a relative `argv[ 0]` resolves against it. On POSIX the directory is `stat`ed in
the parent first -- a child that choked on it would come back as an exit code
and its own errno, and the launch would have "succeeded" -- then `chdir`ed in the
child; on Windows it is `lpCurrentDirectory`, which `CreateProcessW` refuses
itself. Both platforms assert that a child which reports its own working
directory reports the named one, that an absent directory is refused, and, on
POSIX, that `errno` is `ENOENT`. One thing that showed up on Windows alone: the
directory a child ran in stays *in use* for a moment after the child has been
waited for and its process handle closed, so removing it takes a bounded retry
(`smoke_rmdir_when_released` in the test, and see OPENENDS.md). Nothing in
mulle-spawn was failing there -- but a test that litters on one platform is
still a test that fails on one platform.

`MULLE_SPAWN_DESCRIPTOR`, as `options.in/out/err_descriptor`, with
`options.*_mode` saying which streams use it. The descriptor stays the caller's:
the child gets a copy, the handle owns nothing, and neither `close` nor `detach`
closes it -- which is why the handle's fields stay -1 in that mode. A descriptor
that is not open is refused in the parent on POSIX (`fcntl`, `errno` EBADF)
rather than becoming a child that dies on its first write.

`mulle_spawn_detach`, the opposite of `close`: the descriptors go, and the child
is neither terminated nor reaped. It is the one thing on this list that is asked
for by `NSTask`'s *absence* of behaviour -- `dealloc` lets a running child alone
-- and it is what a relauncher or a daemon launcher needs. The price is stated
in its own documentation: nothing will wait for that process again, so on POSIX
it is an orphan for init and on Windows its exit code is never collected.

**Step 1 is complete.** Five cases in `test/10-api/spawn.c` -- environment,
directory, descriptor, detach, plus the refusals -- with goldens re-recorded on
all three platforms. The macOS run earned its keep here: the second descriptor
case passed an `argv` whose last slot was outside the array, and Linux and
Windows happened to have a zero there while Darwin's `execvp` answered EFAULT.
The bug was in the test, not in mulle-spawn, and it only existed because macOS
is run for real rather than assumed.

## Step 2 — interrupt, suspend, resume

`mulle_spawn_terminate` is the only signal there is. `NSTask` needs three more,
and they are the platform differences with the least room to be clever:

- POSIX: `SIGINT`, `SIGSTOP`, `SIGCONT` — `kill(2)`, no surprises.
- Windows: `interrupt` wants `CREATE_NEW_PROCESS_GROUP` plus
  `GenerateConsoleCtrlEvent`. That flag also disables the child's own Ctrl+C
  handling, so it is a decision, not a detail — and it has to be applied at
  `CreateProcessW` time, which is a flag on the options struct that step 1 did
  *not* close over (it is Step 2's to add, and nothing else needed it).
  `suspend` / `resume` are
  `NtSuspendProcess` / `NtResumeProcess`, exported by `ntdll` but undocumented;
  the resolution dance currently lives in `NSTask+Windows.m` and moves here.
- Where a platform genuinely cannot: return a named failure, the way the
  `accept` timeout is absent rather than emulated. `NSTask`'s `BOOL` return
  already has a slot for it.

**Gate:** the same suite, three new cases on POSIX, and on Windows whatever the
platform can honestly report — including the case that cannot be supported,
asserted as unsupported rather than skipped silently.

## Step 3 — port `NSTask+Posix.m`

Replace the internals, keep the class: `launch` becomes build-argv + `open`,
`waitUntilExit` becomes `mulle_spawn_wait( NO_TIMEOUT)`,
`mulleCheckIfRunning` becomes `wait( 0)`, `terminate` / `interrupt` /
`suspend` / `resume` become the four calls, `terminationStatus` and
`terminationReason` read the result instead of `waitpid` macros, and
`_environment` disappears from `Linux`, `Darwin` and `FreeBSD` because the
child's environment is now a parameter.

Semantics that must survive, and the ones that improve:

| behaviour | today | after |
|---|---|---|
| launch of a program that cannot be executed | child sets `_NSTaskHasFailedLaunching` **in its own copy of the ivar**, so the parent cannot see it and it arrives as exit 1 | spawn's error pipe: `open` returns -1, and the caller can say why |
| `dealloc` while running | child keeps running | must be `detach`, or the child dies on release |
| `isRunning` | `waitpid` WNOHANG, reaps | `wait( 0)`, reaps |
| `suspend` / `resume` | `SIGSTOP` / `SIGCONT`, always `YES` | platform call, `BOOL` means it |
| `terminationReason` | `WIFEXITED` / `WIFSIGNALED` | `status` + `signal_number` |

**Gate:** the `MulleObjCOSFoundation` test suite on Linux and macOS, plus a
fresh eye on whether `NSTask` still throws where it used to.

## Step 4 — port `NSTask+Windows.m`

The same, and the one where the payoff is largest: `NSTask+Windows.m` cannot
capture a child's output at all today (`plans/NSTask-IN-PROGRESS.md` calls pipe
support its blocker), while `mulle-spawn-windows.c` does exactly that with
`CreatePipe` + `SetHandleInformation` + `STARTUPINFO` and is verified under
wine. What is ported there is already written here, and it is already tested.

One bug falls out: `NSTask+Windows.m` calls `TerminateProcess( hProcess, 1)`,
so after a `terminate` the task reports `terminationStatus == 1` — the same
answer as a child that exited with 1. Spawn keeps a `mulle_spawn_terminated`
verdict, which `terminationReason` can map honestly.

**Gate:** the Windows category compiles and `NSTask`'s own test runs under
wine, which would be the first time a Windows `NSTask` captured anything.

## Step 5 — the orchestrator and the leftovers

`mulleDataSystemCallWithArguments:environment:workingDirectory:standardInputData:options:`
becomes build-argv + `open( PIPE, PIPE, PIPE)` + write `stdin` on the existing
thread + read `stdout` to EOF + join + wait + build the dictionary. About 25
lines where 166 were today. The 275 lines of dictionary sugar above it do not
move. `NSTask-Private.h`'s four-state enum becomes spawn's status, and
`plans/NSTask-IN-PROGRESS.md` / `NSTask-Windows-Plan.md` get written down as
done rather than in progress.

**Gate:** `MulleObjCOSFoundation/test/NSTask/nstask-simple` plus the generated
`test-NSTask` / `test-NSPipe` / `test-NSFileHandle` suites, and the acceptance
`grep` above returning nothing.

## Explicitly not in this plan

- The run loop, `terminationHandler`, and anything that would make spawn know
  what an object is. The moment spawn grows a callback, this plan has failed.
- `NSPipe+Windows` / `NSFileHandle+Windows`, which are their own workstreams
  and whose plans already exist. Spawn consumes their descriptors; it does not
  replace them.
- Job objects, process groups as a *feature*, resource limits, and the rest of
  what a `posix_spawn`-shaped API could grow into. `NSTask` needs four inputs
  and three signals, and the plan is sized for that.
- `MulleObjCOSFoundation.borked/` and `pre-split/`, which carry older copies of
  the same files (including another `NSTask+Posix.m`). Deleting them is
  housekeeping and should not be mixed into this.

## What could still kill it

1. **`mulle-spawn` is asked to be something else.** If it stays demo
   scaffolding, step 0 never happens and Foundation keeps its own platform
   code. That is a legitimate outcome; it just makes steps 1-5 pointless.
2. **Windows `interrupt`.** `GenerateConsoleCtrlEvent` only reaches processes
   that share a console and are in the group, and the flag that enables it
   changes how the child handles Ctrl+C. If this turns out not to be
   expressible, `interrupt` reports unsupported on Windows and the POSIX
   behaviour is unaffected — the same call shape, one honest gap.
3. **`detach` on POSIX.** Leaving a child unreaped is what `NSTask` does today,
   so it is not a regression, but it is a zombie by construction and the
   documentation has to say so rather than pretend otherwise.
