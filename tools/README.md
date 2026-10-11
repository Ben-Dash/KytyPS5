# Native shader journal audit

The v8 `.shaders` journal stores successful specialized permutations. It omits original userdata and runtime addresses, and omits some Pixel fields consumed by the current compiler. Auditing it reconstructs a permutation from recorded fields/defaults; it does not replay an original failed attempt or prove original SPIR-V, rendering or guest resource correctness.

Use a frozen journal copy and its verified zero-based index. Prepare one exact record by ordinal, avoiding ambiguous hash-prefix selection:

```powershell
pwsh.exe -NoProfile -File .\tools\prepare-shader-journal.ps1 `
  -Journal .\_Build\analysis\journal-copy.shaders `
  -RecordIndex .\_Build\analysis\journal-v8-record-index.json `
  -CompilerSettings .\tools\shader-journal-profile.example.json `
  -SelectRecord 0 -OutputDirectory .\_Build\analysis\journal-record-0
```

Omit `-SelectRecord` to prepare every indexed record. The importer checks the complete journal SHA256, header, envelope/index offsets, lengths, stored checksum metadata, every body SHA256 and complete tail. Each selected body remains in ignored local artifacts. Identical bodies share a binary while each ordinal retains its manifest/provenance. The worker independently checks XXH3 and native codec roundtrip before compiling.

The example compiler profile is an explicitly declared conservative CPU experiment, not the original GPU profile. Supply the desired actual settings rather than interpreting the example as measured capability data. All supported compiler switches must be explicit, including null for unset; each worker restores them before first compiler use.

```powershell
pwsh.exe -NoProfile -File .\tools\audit-shaders.ps1 `
  -CorpusDirectory .\_Build\analysis\journal-record-0 `
  -Auditor .\_Build\windows-integration\shader_batch_audit.exe `
  -OutputDirectory .\_Build\shader-audits\journal-record-0 -Jobs 1
```

The native worker calls the shared decoder, translator and compiler with recorded specialization, then validates raw SPIR-V for Vulkan1.3. It never creates a Vulkan device, materializes guest memory, runs a driver pipeline or accepts a skipped shader as a pass. Results use `scope=journal_specialized_permutation`, `reconstructed_defaults=true` and `original_input_equivalent=false`. Pixel omissions, userdata count-only use and absent materialization are reported explicitly.

The bounded runner ports the previous proven workflow: frozen inputs and binary hashes, dedup of distinct inputs, auditor/profile identity, `-Resume`/`-RetryFailed`, async stream drain into files, output/deadline bounds, hidden owned-process-tree cleanup (including Windows PowerShell5 taskkill fallback), overlapping cause groups and bounded replay commands. Before launch it removes inherited `KYTY_`, `VK_` and `SPIRV_` variables; the worker restores the manifest profile. Direct manual worker calls need the same clean environment. It preserves prior journal/results. JSON/Markdown reports distinguish capture aliases, distinct inputs, scope, checked phases and input errors. Counts do not imply complete runtime captures.

Run script-only lifecycle fixtures with `pwsh.exe -NoProfile -File .\tests\ShaderAuditRunnerTests.ps1`. They use fake PowerShell workers and do not verify native compiler semantics. Native fixture/build checks are coordinated separately. Original failure capture needs the separate fork-native schema3 attempt/callback archive; journal reconstruction never supplies missing original runtime data.
