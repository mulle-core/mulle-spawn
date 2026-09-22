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
