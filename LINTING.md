# wirelog Linting Guide

This document describes the linting and formatting tools used in wirelog, their configuration rationale, and the rollout plan.

## Tool Versions

All linting tools are pinned to **LLVM 18** in CI.

| Tool | Version | Purpose |
|------|---------|---------|
| clang-format-18 | LLVM 18.x | Code formatting |
| clang-tidy-18 | LLVM 18.x | Static linting (C-safe allowlist) |
| editorconfig-checker | latest | Editor settings enforcement |

### Local Installation (LLVM 18)

**Ubuntu/Debian:**
```sh
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
echo "deb https://apt.llvm.org/$(lsb_release -cs)/ llvm-toolchain-$(lsb_release -cs)-18 main" | \
  sudo tee /etc/apt/sources.list.d/llvm-18.list
sudo apt-get update
sudo apt-get install -y clang-format-18 clang-tidy-18 clang-tools-18
```

**macOS (Homebrew):**
```sh
brew install llvm@18
# Add to PATH: export PATH="/opt/homebrew/opt/llvm@18/bin:$PATH"
```

## Configuration Files

| File | Purpose |
|------|---------|
| `.editorconfig` | Universal editor settings (indent, line endings, charset) |
| `.clang-format` | C code formatting rules (GNU-based, K&R control braces) |
| `.clang-tidy` | Static analysis checks (C-safe allowlist) |
| `.clang-tidy-expected-checks.txt` | Deterministic verification of enabled checks |

## Running Checks Locally

### Format check (dry-run)
```sh
find wirelog/ tests/ -name '*.c' -o -name '*.h' | xargs clang-format-18 --dry-run --Werror
```

### Auto-format
```sh
find wirelog/ tests/ -name '*.c' -o -name '*.h' | xargs clang-format-18 -i
```

### clang-tidy
```sh
meson setup builddir-tidy --reconfigure -Dtests=true 2>/dev/null || meson setup builddir-tidy -Dtests=true
run-clang-tidy-18 -p builddir-tidy wirelog/ tests/
```

### Verify expected checks (C++ check gate)
```sh
diff <(clang-tidy-18 --list-checks --config-file=.clang-tidy -- /dev/null 2>/dev/null | \
  tail -n+2 | sed 's/^ *//' | sort) .clang-tidy-expected-checks.txt
```
This diff must be empty. If any unexpected check appears, the `.clang-tidy` config or expected-checks file needs updating.

## clang-tidy Check Rationale

### Check Selection Strategy

We use an **explicit C-safe allowlist** rather than wildcards (e.g., `bugprone-*`). This guarantees no C++-specific checks are enabled by construction. To add a new check, first verify it applies to C11 code (not just C++).

### Enabled Check Groups

**bugprone (33 checks):** C-applicable bug detection -- sizeof errors, macro pitfalls, integer issues, signal handlers, memory manipulation, suspicious patterns. All C++-only checks (use-after-move, exception-escape, virtual-near-miss, etc.) are excluded by not listing them.

**cert (6 checks):** Only CERT C rules (not CERT C++ rules):
- `cert-dcl03-c`: Static assertions
- `cert-env33-c`: Do not call system()
- `cert-err34-c`: Error checking on conversion functions
- `cert-flp30-c`: Do not use floating-point variables as loop counters
- `cert-msc30-c`: Do not use the rand() function for generating pseudorandom numbers
- `cert-str34-c`: Cast characters to unsigned char before converting to larger integer sizes

**clang-analyzer (scoped wildcards):** Uses sub-category wildcards (`core.*`, `deadcode.*`, `nullability.*`, `security.*`, `unix.*`, `valist.*`) which are C-safe by design. The `cplusplus.*` sub-category is excluded by not including it. `security.insecureAPI.DeprecatedOrUnsafeBufferHandling` is disabled because it flags every `memcpy`/`snprintf` on glibc.

**portability (2 checks):** Explicit C-safe portability checks (`restrict-system-includes`, `simd-intrinsics`). No wildcards used.

**readability (2 checks):** Only `misleading-indentation` and `redundant-declaration`. `readability-braces-around-statements` is deferred to Phase 2.

**misc (1 check):** `misc-redundant-expression` catches copy-paste bugs.

## NOLINT Comment Style Exception

The wirelog codebase enforces C-style `/* */` comments exclusively. However, clang-tidy's suppression mechanism requires `//`-style comments.

**Policy exception:** `// NOLINT` and `// NOLINTNEXTLINE` are the **sole permitted use** of `//`-style comments. These are tool-required pragmas, not code comments.

### Suppression Policy

- Use `// NOLINTNEXTLINE(check-name)` with a brief reason
- Example: `// NOLINTNEXTLINE(bugprone-narrowing-conversions) -- intentional truncation to uint32`
- No blanket `// NOLINT` without specifying the check name
- Suppressions should be reviewed periodically; avoid accumulation of permanent suppressions

## CI Gate Enforcement

All lint checks run as **blocking gates** in CI. The `lint.yml` workflow is a reusable workflow (`workflow_call`) that must pass before any downstream CI or static-analysis job runs.

**Dependency graph:**
```
lint (format-check + clang-tidy + editorconfig)
 ├── ci.yml: sanitizers → build
 └── static-analysis.yml: cppcheck, scan-build
```

If any lint job fails, all downstream jobs are skipped.

### Future: readability-braces-around-statements

`readability-braces-around-statements` is deferred. Enabling it requires a dedicated braces-addition PR first, followed by adding the check to `.clang-tidy` and `.clang-tidy-expected-checks.txt`.

## Adding or Removing Checks

1. Edit `.clang-tidy` -- add/remove the specific check name
2. Run locally: verify the check applies to C11 (not C++ only)
3. Update `.clang-tidy-expected-checks.txt` with the new sorted check list
4. Run the verification gate to confirm consistency:
   ```sh
   diff <(clang-tidy-18 --list-checks --config-file=.clang-tidy -- /dev/null 2>/dev/null | \
     tail -n+2 | sed 's/^ *//' | sort) .clang-tidy-expected-checks.txt
   ```
5. Commit `.clang-tidy` and `.clang-tidy-expected-checks.txt` together

## Known Compromises

1. **Pointer alignment:** `PointerAlignment: Right` means return-type pointer declarations like `wirelog_program_t*` will be reformatted to `wirelog_program_t *`. This is acceptable because `Right` correctly handles the dominant case (variable declarations).

2. **Test formatting divergence:** Test files use same-line function braces. The baseline formatting PR (Phase 2 prerequisite) will reformat them to match the project convention.

3. **ColumnLimit 80:** Standard 80-column limit. All source files have been formatted to comply.
