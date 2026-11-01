# Testing and evidence

## Two kinds of test

**Host tests** run without a device and are the first gate: a change that breaks the card model, the
page renderer or a parser never reaches the card.

| Test | Covers |
|---|---|
| `scripts/h1/test-odin-sd.sh` | the real driver against a software model of the host and the card |
| `scripts/h1/test-stack-fold.sh` | stack-margin accounting used by the diagnostics |
| boot-progress console test | page rendering, panic formatting, truncation, fault retention |
| seed and medium tests | edits produce exactly the intended file changes |
| userland-log summary | parsing and comparing the on-card log |

**On-card tests** are one boot with one change, with a written prediction taken before it happens.

## Rules

1. **One change per medium**, instrumentation included. A boot that differed in three things proves
   nothing.
2. **A prediction table before the boot**: what will be seen, what each result would prove, what it
   would not.
3. **Every figure carries its boot and its source**: the medium, the boot, and where the number came
   from (log line, page, counter). Numbers from two boots are never mixed; units are written with the
   number.
4. **Evidence or nothing**: a file and line, a SHA-256, a size, a log line or a measurement. Anything
   unmeasured is **not determined** — never a guessed probability.
5. **The device is not a test rig.** Boots have a thermal cost; long boots are not repeated without a
   reason, and the panel's condition is a design constraint.
6. **Read the rules before acting**, and ask before any action with a cost: writing the card, booting,
   compiling a kernel, rewriting the Data partition.

## What a good result looks like

- Host tests pass, and a deliberate mutation makes them fail.
- The medium's hash table matches the intended change, file by file.
- The boot log agrees with the prediction. When it does not, the log wins.
- The conclusion, with its limits, is written before the next change is designed.

## What is not a test

- A boot that "looks fine" with no log line behind it.
- A change nothing measures.
- A hypothesis confirmed by the evidence that suggested it.
