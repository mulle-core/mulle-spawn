# mulle-sde Build System Guidelines
<!-- Keywords: craft, build, run, check -->

mulle-sde manages the build from beginning to end. Do not run `cmake` or edit
build system files manually unless absolutely necessary.

## Core commands

```bash
mulle-sde check                                    # fast syntax/type check (no link)
mulle-sde reflect
mulle-sde craft
mulle-sde craft -g
mulle-sde -DCFLAGS="-DSOME_FLAG=value" craft --clean
```

## Workflow

Use `mulle-sde check` after editing source files to catch type errors
instantly (~0.7s). Only run `mulle-sde craft` when you need a full build
with linking (e.g. before `mulle-sde run` or `mulle-sde test`).

## Build rules

- do not add sources to build files by hand
- add dependencies with `mulle-sde dependency`
- add libraries with `mulle-sde library`
- missing header → likely `dependency add`
- missing symbol → likely `library add`

## Troubleshooting

```bash
mulle-sde library add <name>
mulle-sde dependency add github:<user>/<repo>
mulle-sde reflect && mulle-sde craft
```
