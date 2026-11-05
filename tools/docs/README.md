# docs/ENV_REFERENCE.md generator

`docs/ENV_REFERENCE.md` is generated. Never edit it by hand.

```
python -I tools/docs/fill_env.py . tools/docs/env_template.md docs/ENV_REFERENCE.md
```

- `env_template.md` holds the sections and rows; the last column of a row is `@@VARNAME@@`, which the generator
  replaces with the source locations (`file:line`, up to three plus "+N more").
- The generator prints `unresolved:` (placeholders whose variable is not read anywhere: typo, or the helper that reads
  it is not in the `READ` regex) and `read in source but not documented:` (variables that need a row). Both must be
  empty after a change.
- The `READ` regex lists the env-reading helpers (`getenv`, `env_int`, `env_f`, `env_float`, `mfd_env_list`, ...);
  extend it when code introduces a new helper.
- Add rows with a small script (see `tools/scratch/*` for the `env_add_rows*.py` pattern in the backups) or by hand in
  the template; keep rows one line each: `| \`VAR\` | default | values | description | @@VAR@@ |`.
