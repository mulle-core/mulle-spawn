# research

Not tests, and not built by anything here. A test whose output cannot be
compared against a golden is still a test: it belongs in the suite, prints to
`stderr` so a reader can see the numbers, and passes by completing -- that is how
the benchmarks in other projects here are wired. What separates this directory
is not the missing golden but the subject: a question whose answer is about the
platform rather than about this code, that does not change when the code does,
and that nobody needs re-run on every craft. Reading the documentation does not
settle it, so it is answered by running something once, and kept so the next
person to doubt the answer does not have to redo the experiment.

## `env-block-probe.c` — does the platform honour `lpEnvironment`?

`mulle_spawn_open_with_options` hands `CreateProcessW` an environment block, and
a child launched with one reported an *empty* environment -- the same as being
handed no environment at all. This splits the two halves of it -- the
block's contents, and the flag that says how to read them:

1. it builds a block the way `mulle-spawn-windows.c` does and prints it, code
   point by code point, so the block can be read rather than assumed;
2. it launches itself with that block, and with `NULL` for comparison;
3. it hands Windows' **own** `GetEnvironmentStringsW()` blocks straight back.

Measured under wine:

| handed to `CreateProcessW` | what the child saw |
|---|---|
| the block as `mulle-spawn-windows.c` built it | nothing: marker `<unset>`, `MULLE_SPAWN_TEST=<unset>`, `PATH` `<unset>` |
| the same block, with `CREATE_UNICODE_ENVIRONMENT` | **the block**: `MULLE_SPAWN_TEST=replaced`, marker `<unset>`, `PATH` `<unset>` |
| `NULL` | the parent's environment: marker `inherited`, `PATH` set |
| `GetEnvironmentStringsW()`, with the flag | the parent's environment unchanged (`0047 0049 0054` = `GIT…`) |

The block is well formed — `004d 0055 … 0064 0000`, i.e. `NAME=VALUE\0\0` — so
the first row is not about its contents: a UTF-16 block announced as ANSI does
not parse, and a block that does not parse is no environment at all. The second
row is the answer, and it retires what this file used to say: **wine does not
ignore `lpEnvironment`; `mulle-spawn` left out `CREATE_UNICODE_ENVIRONMENT`.**
The fourth row is the control, and it is what made the first reading look
conclusive — the platform's own block, handed back *without* the flag, fails in
exactly the same way.

What follows for the test: `test/10-api/spawn.c` judges the contents of a *given*
environment exactly, on every platform. On Windows that costs one entry. A child
inherits its loader search path through its `PATH`, and the child there is the
test program itself, which links the library -- so the two given environments
carry the parent's `PATH`, and `emptied` is "empty except for the loader path"
on that platform. Carrying `WINEPATH` instead does nothing for an import, which
was measured too: the two cases kept dying in the loader with `import_dll Library
libmulle-spawn.dll ... not found`, and stopped the moment the entry was `PATH`.

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
