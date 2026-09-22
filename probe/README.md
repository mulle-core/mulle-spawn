# probe

Not tests, and not built by anything here. This is a question that was answered
by measurement rather than by reading the documentation, kept because the next
person to doubt the answer should not have to redo the experiment.

## `env-block-probe.c` — does the platform honour `lpEnvironment`?

`mulle_spawn_open_with_options` hands `CreateProcessW` an environment block, and
a child launched with one reported an *empty* environment -- the same as being
handed no environment at all. This splits "the block is wrong" from "the
emulator cannot carry it":

1. it builds a block the way `mulle-spawn-windows.c` does and prints it, code
   point by code point, so the block can be read rather than assumed;
2. it launches itself with that block, and with `NULL` for comparison;
3. it hands Windows' **own** `GetEnvironmentStringsW()` blocks straight back.

Measured under wine:

| handed to `CreateProcessW` | what the child saw |
|---|---|
| the block mulle-spawn builds | nothing |
| `NULL` | the parent's environment, as expected |
| `GetEnvironmentStringsW()` unchanged | **nothing** |

The block is well formed — `004d 0055 … 0064 0000`, i.e. `NAME=VALUE\0\0` — and
the third row is what makes the answer unambiguous: if the platform's own block,
handed back untouched, also produces a child that sees nothing, then nothing
about the format can be the problem. **wine ignores `lpEnvironment` for any
non-NULL block**, so this cannot be verified here and needs a real Windows.

What follows for the test: `test/10-api/spawn.c` asserts what a replaced
environment *does* show under wine — that handing one over ends the inheritance
and the parent's marker goes with it — and leaves the block's contents judged on
POSIX, where they are judged exactly.

    cc=/opt/mulle-clang-project-windows/latest/bin/x86_64-w64-mingw32-clang
    $cc -std=gnu11 -o env-block-probe.exe env-block-probe.c
    wine env-block-probe.exe

## `cwd-probe.c` — why can a directory a child ran in not be removed?

The `directory` case makes a directory, runs a child in it and gives it back.
On Linux the removal works first try. Under wine it comes back `EACCES`, and
this splits the two reasons that could explain it — that removing a directory
fails here at all, or that it fails only once a child has used it — and reports
the errno of each attempt, and what waiting changes:

1. remove a directory nothing has used;
2. launch itself with that directory as its working directory, wait for the
   child *without* a timeout, remove it right after;
3. wait 500 ms and remove it again.

Measured under wine:

    a directory nobody ran in:
      _mkdir:                   0 (errno 0)
      _rmdir right after:      removed it
    a directory a child ran in:
      _mkdir:                   0 (errno 0)
      the child exited with 288
      _rmdir right after:      removed it
      _rmdir after 500ms:      failed, errno 2
      _rmdir after 2.5s:       failed, errno 2

The `errno 2` is `ENOENT`: the first removal worked, and the later attempts are
complaining about a directory that is already gone.

So removal is neither broken nor permanently held — the same call the test
cannot make on its first three attempts succeeds here on its first, with the
child reaped by an untimed wait and its process handle closed straight after.
What is left is timing inside wine's own bookkeeping: the directory can be
removed as soon as the process object is forgotten, and in the test that takes
about 150 ms.

What follows: `smoke_rmdir_when_released()` retries `EACCES`/`EBUSY` a bounded
number of times and says so on stderr if it gives up. See OPENENDS.md in the
demo for why this is recorded rather than hidden.

    $cc -std=gnu11 -o cwd-probe.exe cwd-probe.c
    wine cwd-probe.exe
