## Summary

<!-- What does this PR change, and why? -->

## Related issue

<!-- Link an issue if one exists, or "N/A" -->

## Type of change

- [ ] Bug fix
- [ ] New feature / module
- [ ] NUMAflow atomic op / strategy / preset
- [ ] Documentation
- [ ] Build / CI
- [ ] Other

## Testing performed

<!-- See TESTING.md for the full tier breakdown. At minimum: -->

- [ ] `cd src && make clean && make -j$(nproc)` builds clean
- [ ] `make test` (standard Redis Tcl suite) passes
- [ ] `cd numaflow && make test` passes (if NUMAflow code changed)
- [ ] Ran `./run_full_validation.sh --quick` (if touching NUMA modules or NUMAflow bridge)

## Checklist

- [ ] Docs updated (`docs/new/`, `ARCHITECTURE.md`, `CLAUDE.md`) if this changes module behavior or architecture
- [ ] `CHANGELOG.md` updated
- [ ] No `jemalloc`-only assumptions introduced (this project forces `MALLOC=libc` on Linux, see `src/Makefile`)
