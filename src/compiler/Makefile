# Makefile - compiler-local build for the board's make (/bin/make.bin).
# Dialect, same as the repo root's Makefile.board: one target per line; the
# target IS its output path; leading '-' tokens are that target's cc flags
# (-I takes the next token); the remaining tokens are prereqs - .c files are
# handed to cc, headers are existence-checked (cc ignores them, so listing
# them is safe and makes `make` fail early when one is missing), and a prereq
# naming another target in this file makes the line a collection. A leading #
# is a comment. Always rebuilds: make has no timestamps yet by design.
#
# Run it ON THE BOARD, from the compiler's own directory (the card mirrors
# this repo's src/ tree under /src and stages cc.h/sys.h in /lib):
#
#   cd /src/compiler
#   /bin/make.bin -n        dry-run: prints the cc command, builds nothing
#   /bin/make.bin           the first target is cc.bin, so this builds the
#                           compiler into the current directory
#
# The output path is the bare target name, so it is relative to wherever you
# run from: from /src/compiler you get /src/compiler/cc.bin (a locally built
# compiler, right next to its sources); from / you get /cc.bin. Use -n first
# to see the exact command before the real run.
#
# Same cc, same -I /lib, same source order as Makefile.board's /src/cc.bin
# line, so the binary is byte-identical to the PC build's root/bin/cc.bin
# (that is check_make.py --board's whole contract for the root file).
#
# The source list below is the FOURTH copy of the compiler's file list. When
# a stage file is added or removed, update all of: the root Makefile's
# CC_SRC, Makefile.board's /src/cc.bin line, tools/check_make.py's plan-board
# fixture (it pins the command built from that line and fails on drift), and
# this line.
cc.bin: -I /lib /src/compiler/cc.c /src/compiler/emit.c /src/compiler/expr.c /src/compiler/fp.c /src/compiler/lex.c /src/compiler/licm.c /src/compiler/opt.c /src/compiler/regs.c /src/compiler/state.c /src/compiler/stmt.c /src/compiler/types.c /lib/cc.h /lib/sys.h
