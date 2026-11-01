# Code reused from suyu

Files under `core/` and `common/` come from suyu (https://git.suyu.dev/suyu/suyu),
GPL-2.0-or-later, at the revision pinned by mk8-recomp. They are used unchanged except
where noted; `common/uuid.h`, `common/assert.h`, `common/random.h` and `core/hle/result.h`
are small replacements written for nxrt so the rest builds without the emulator.

Used for: Mii data structures and the default Mii tables (`core/hle/service/mii`).
