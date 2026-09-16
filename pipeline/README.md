# The pipeline

Two models working on zelr without a person in the loop, and a gate neither
of them can talk its way past.

## Why it is shaped like this

Both Claude Code and Codex can be run from a shell script: given a prompt and
a directory, each edits files in place and prints a report. So a script can
hand work to one, hand the result to the other, and keep going.

The interesting question is what they should each do. Having both write
features is the obvious answer and the wrong one: they collide in the same
files and duplicate each other's thinking. What has actually worked on this
project is one writing and the *other* reading. Every serious bug found here
so far was found by someone other than the author, or by a test failing, and
never by the author re-reading their own code. Codex found a double free in
`wm_close`, a permission check that accepted merely-present pages, and a
quadratic allocator in the FAT driver, all in code whose own tests passed.

So: one model writes, the other reviews, and they swap by task.

The other half is that neither of them decides whether the work was any
good. `gate.sh` does, mechanically, and nothing reaches `main` without it.
An agent reporting success is not evidence.

## Before the first run

Both agents have to be signed in, and they sign in separately from whatever
session you are reading this in.

    bash pipeline/preflight.sh

That asks each one a trivial question and checks the emulator, python, and
the state of the tree. It takes a few seconds and it exists because the
expensive failure is not a task going wrong, it is a task going wrong for a
reason that would have stopped every task. An expired login turns the whole
backlog into blocked tasks one cycle at a time.

If it says claude did not answer, run `claude` once, sign in, and quit. The
command line tool keeps its own credentials.

`loop.sh` runs preflight first and refuses to start if it fails.

## Running it

    bash pipeline/batch.sh                three tasks at once, one full gate
    BATCH=2 bash pipeline/batch.sh        two
    PUSH=1 bash pipeline/batch.sh         and push the batch when it lands

    bash pipeline/loop.sh                 one task at a time, in order
    bash pipeline/cycle.sh                exactly one task

`batch.sh` is the one to use. `cycle.sh` does a single task properly and
takes about forty minutes, thirty-five of which is the full gate; eight tasks
that way is most of a day, nearly all of it waiting for QEMU.

Two things fix that. Each task gets its own git worktree, so three agents
write three changes at once without seeing each other's files, and the fast
gate runs in each: it is a build and two QEMU runs with no monitor port, so
several at a time contend for nothing but the processor. Then the branches
that passed are merged onto one integration branch and the full gate runs
**once for the batch**.

The screen harnesses used to need a fixed monitor port each, so two runs
that overlapped drove each other's machines. They take a free port now, and
the gate takes a lock so two of them cannot start at all.

Three tasks a batch turns three forty minute cycles into roughly one.

If the batch fails the full gate together, it falls back to trying each
branch on its own, which is slower but says which one was the problem.

`PUSH` is off by default. Landing on local `main` is reversible; pushing to a
public repository is less so, and the difference should be a decision rather
than an accident.

## What one cycle does

1. Takes the top `todo` task from `backlog.md` and makes a branch for it.
2. The task's `author` writes the code.
3. `gate.sh fast` runs: build, the kernel's 234 self checks, the serial shell
   test. About five minutes. If it fails, the branch is deleted and the task
   is marked `blocked` with its logs kept.
4. The *other* model reviews the diff and answers in JSON: sound, or a list
   of findings with severities.
5. The author answers every finding, fixing it or refusing with a reason.
   Nothing is silently dropped.
6. `gate.sh full` runs: everything above plus all four boot paths and the
   three harnesses that drive the desktop and the keyboard through QEMU's
   monitor. About thirty-five minutes.
7. Only then does it commit and merge to `main`.

A task that fails at any gate never reaches `main`. Its branch is deleted and
`pipeline/state/<id>/` keeps the prompts, both agents' reports, the review
findings and the gate output, so it can be picked up by hand.

## Which model does what

`models.sh`. Claude runs on Opus, so its counterpart is Codex's strongest
rather than its everyday one: a review by something weaker than the thing
that wrote the code is worth very little, and reviewing is what this whole
arrangement exists for.

    CLAUDE_MODEL   opus
    CODEX_MODEL    gpt-5.6-sol

Effort is separate from the model and matters as much. Sol at `low` is what
the desktop app defaults to and is not what you want writing a page table
walker, so implementing and reviewing both run at `high`, and answering a
specific finding runs at `medium` because the thinking has already been done
and written down.

Override any of it to spend less:

    CODEX_MODEL=gpt-5.6-luna bash pipeline/batch.sh

## Releasing

    bash pipeline/release.sh v0.16.0 notes.md

The readme tells people to download zelr.exe and zelr.iso from the latest
release. For fifteen releases neither was attached: the notes were written by
hand and nobody built the artifacts, so following the readme led to a page
offering a source tarball and nothing else. This builds both, refuses to
continue if the version compiled into the kernel disagrees with the tag, and
checks the version string is actually inside each file before uploading.

## The gate

    bash pipeline/gate.sh fast
    bash pipeline/gate.sh screen
    bash pipeline/gate.sh full

screen is fast plus everything that drives the desktop, the terminal, the
windows, USB and the input path. It exists because four of full's steps take
six minutes each and all four are about disks and boot sectors, which a change
to the compositor or the keyboard cannot reach. Four and a half minutes
against seventeen.

This is the only thing in the pipeline that decides anything, so it is worth
being suspicious of. It has been checked three ways: with a deliberately
failing kernel check (fails, exit 1), with a deliberate syntax error (fails,
and stops rather than testing the previous build), and with the tree in a
known good state (passes).

If the build fails it stops there. `build/zelr.bin` is whatever the last
successful build left behind, so carrying on would say something true about
code that no longer exists.

## The backlog

`backlog.md` is a markdown table. The scripts only ever write the `state`
column; everything else is for people to read and edit. Add tasks, reorder
them, change who writes them.

Keep them small. "Add a driver" is a task. "Finish the operating system" is
not, and an agent given that will produce something shaped like an answer
rather than an answer.

A `blocked` task needs a person: read the log, work out whether the task was
wrong or the attempt was, then either fix the task and set it back to `todo`
or delete it.

## What the agents are allowed to do

They run with an explicit tool allowlist: Bash, Read, Write, Edit, Glob,
Grep. Nothing that reaches the network.

Bash is the one that matters and it is not optional. The first real cycle
failed because the author had `acceptEdits` and nothing else: it wrote a
CMOS driver, then asked permission to build it, got no answer because nobody
was there, and reported back having compiled nothing. The shell test caught
the regression, which is the gate working, but the whole cycle was wasted on
an agent that could not run the check it was told to run.

So they can run commands on your machine unattended. What contains that is
that each works in a git worktree, nothing reaches `main` without the full
gate, and nothing is pushed unless you ask.

## What this will not do

**It will not know when a task was a bad idea.** The gate proves a change
builds, boots and passes its tests. It cannot tell you the feature was not
worth having, or that it is the wrong shape, or that two tasks should have
been one. That judgement stays with you, and the place to exercise it is the
backlog.

**It will not catch what nothing tests.** The gate is strong where this
project has tests and blind where it does not. A change to something with no
harness gets past on the reviewer's reading alone. When you add a subsystem,
add a way to check it, or the pipeline quietly gets weaker as the project
grows.

**It will not stop two agents disagreeing forever.** The author gets one pass
to answer the reviewer; then the full gate decides. If they are going back
and forth on something real, the task is probably wrong and wants a person.

**It is not free.** Every cycle spends tokens on both accounts and about
forty minutes of wall clock, most of it in QEMU.

## Files

    preflight.sh        checks both agents answer before spending an hour
    models.sh           which model and how much effort, by role
    lib.sh              finding the agents, running them, reading the backlog
    batch.sh            several tasks at once, sharing one full gate
    parse_review.py     reading a reply that was supposed to be JSON
    gate.sh             the verification, fast and full
    cycle.sh            one task, start to finish
    release.sh          building the iso and the exe, and attaching them
    loop.sh             cycles, with a budget and a stop rule
    backlog.md          what to do next
    prompts/
      implement.md      writing a change
      review.md         reading someone else's
      address.md        answering a review
    selftest.sh         the pipeline's own test, with scripted agents
    state/<task-id>/    everything a cycle produced, kept whether it worked
