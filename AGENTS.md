# Working on JLang

- Build: `make`. Test: `make test`. Memory-safety check: `make sanitize`. Both must pass before committing.
- Code is C99 with no dependencies; it must compile warning-free with `-Wall -Wextra -Wpedantic -Wshadow`.
- The core (everything in `src/` except `main.c` and `board_sim.c`) must stay portable to microcontrollers: no OS calls, no unbounded recursion, fixed limits from `src/common.h`.
- Every language feature needs a test in `tests/` (`// expect:` lines; see `tests/run.sh`) and an entry in `docs/LANGUAGE.md` and `docs/llms.txt`.
- Error messages are part of the product: say what went wrong, where, and how to fix it.
