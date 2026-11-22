# data/logs

Markdown log files written by `lib/metalog.lua`:

- `<logger>_<YYYY-MM-DD>.md` - one file per plugin (logger) per day
- `errors_<YYYY-MM-DD>.md` - warnings and errors of every plugin in one place

Server console: `metalog status`, `metalog tail <logger> [lines]`, `metalog tail errors`.
Settings (master switch, level, size limit): the `M.CONFIG` table at the top of `lib/metalog.lua`.
Do not delete this folder (xmod cannot create folders; without it the logs go to `data/logs_*.md`).
