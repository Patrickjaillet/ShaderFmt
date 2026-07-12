# Contributing to ShaderFmt

Thanks for considering a contribution. This project's one non-negotiable
rule: the formatter must never change a shader's behavior. Only
whitespace, indentation, and brace placement may ever change - never
tokens, never their order.

## Commit messages

Commits follow [Conventional Commits](https://www.conventionalcommits.org/):
`type(optional-scope)!: description`, where `type` is one of `feat fix
docs style refactor perf test build ci chore revert` and a trailing `!`
(after the type/scope) marks a breaking change - e.g. `feat(parser): add
MSL attribute grammar` or `fix!: correct WGSL paren-less if parsing`.
This is what lets the release tooling (`semantic-release`) compute a
version bump and changelog straight from commit history, rather than by
hand.

A `commit-msg` hook enforcing this is checked into the repo at
`.githooks/commit-msg`, but git never runs hooks outside `.git/hooks`
unless you opt in - enable it once per clone with:

```bash
git config core.hooksPath .githooks
```

## Building and running the tests locally

`lib/` (the formatting engine) has no external dependencies and builds
with just CMake + a C++17 compiler:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`app/` (the Qt desktop editor) additionally needs Qt6 and QScintilla
built against it - see README.md's "Building from source" section for
the exact steps. If Qt6 isn't found, CMake skips `app/` automatically
and `lib/` + `tests/` remain fully usable on their own.

A couple of tests use optional external reference compilers to double-
check that formatting never breaks a shader: `glslangValidator`
(Khronos) for GLSL/HLSL, and `naga-cli` (`cargo install naga-cli`) for
WGSL. Without either, that one test reports `SKIP` instead of running;
everything else is unaffected.

## Adding a new language/dialect

Look at how HLSL, WGSL, MSL, or ShaderLab were added (each is a small,
self-contained set of changes across the same files) - Cg/ShaderLab in
particular is the most involved example, since it embeds a second
language inside its own grammar. In order:

1. **`lib/include/shaderfmt/Token.hpp`** - add a `Language` enum value,
   with a comment explaining how lexically close the new dialect is to
   the existing C-like family (this determines how much work steps 2-3
   actually need).
2. **`lib/src/Lexer.cpp`** - only needs changes if the new dialect's
   comments/preprocessor rules differ from the existing `!= WGSL` /
   `== WGSL` branches. Most C-like dialects (see MSL) need zero lexer
   changes.
3. **`lib/src/Parser.cpp`** - if the dialect is lexically C-like, extend
   `clikeQualifierKeywords()` with its qualifier keywords and it likely
   needs nothing else (see MSL's `parseTopLevelItem()`/`parseStatement()`
   dispatch already covering it). If the dialect's *grammar* genuinely
   differs (see WGSL, ShaderLab), give it its own top-level entry point
   dispatched from `Parser::run()`, reusing the shared low-level helpers
   (`scanChunkTokens()`, `parseBlock()`, `recoverAsErrorNode()`) wherever
   the shape is the same.
4. **`lib/include/shaderfmt/Ast.hpp`** - only if the new grammar needs
   node kinds the existing ones (`Chunk`, `ErrorNode`, `Block`,
   `StructDecl`, ...) can't already express. Read the file's header
   comment first: the tree is a *lossless CST*, deliberately shallow (no
   expression precedence tree) - keep new node kinds to the minimum
   needed for correct comment attachment and tolerant error recovery,
   nothing more.
5. **`lib/src/Formatter.cpp`**'s `emitNode()` - only needs a new `case`
   if a new NodeKind was added in step 4 and its emission order/shape
   isn't already covered by the default "tokens then children" case.
6. **`lib/src/LanguageDetector.cpp`** - add a `looksLikeXxx()` heuristic
   and wire it into `detectLanguage()`'s priority chain. Be conservative:
   avoid bare common-English-word substrings (a real false positive was
   found and fixed for MSL's `vertex`/`fragment`/`kernel` - they showed
   up in ordinary GLSL comments). If the new dialect can contain another
   dialect's own signals (see ShaderLab embedding HLSL), it must be
   checked *before* that other dialect in the priority chain.
7. **`tests/corpus/`** - add at least one hand-written fixture. Real
   open-source shaders (with a genuinely verified license - see
   `tests/corpus/licensed/SOURCES.md` for the pattern and prior art) are
   strongly encouraged: they've repeatedly caught real gaps that
   hand-written snippets missed (a WGSL paren-less `if`, an MSL
   detection false positive, a ShaderLab empty-brace formatting bug -
   all found this way, all now regression-tested).
8. **`tests/test_main.cpp`** - add the new files to `corpusFiles()` (they
   automatically get idempotence, token-sequence non-regression, AST
   round-trip, and fuzz/truncation coverage for free), and add a few
   targeted shape/detection tests the way existing dialects do.
9. **`app/src/MainWindow.cpp`** and **`app/src/ShaderfmtLexer.cpp`** -
   add the new `Language` case to every `switch` (the compiler will tell
   you if you miss one - none of these switches have a silent
   `default:` for known languages) and add the combo box entry.

Run the full test suite and the app's `--self-test` before opening a PR
(see README.md for the exact commands) - a new dialect isn't considered
done until it's demonstrably lossless, idempotent, and (where a
reference compiler is available) still compiles after formatting.

## Adding a new style rule

Style options live in `lib/include/shaderfmt/Formatter.hpp`'s
`FormatOptions` struct and are applied in `Formatter.cpp`'s `Emitter`
class. If the rule needs a UI control, add it to
`app/src/StylePanel.{hpp,cpp}` following the existing pattern (a control
that updates `options_`, calls `emitOptionsChangedAndPersist()`, and
gets covered by the app's `--self-test`). Never make a style choice that
could change a shader's behavior - only whitespace/indentation may vary.

## Code style

C++17, no exceptions thrown across `lib/`'s public API, no external
dependencies in `lib/`. Comments explain *why*, not *what* - see the
existing code for the tone this project uses. `.clang-format` in the
repo root defines the formatting for this project's own C++ source.
