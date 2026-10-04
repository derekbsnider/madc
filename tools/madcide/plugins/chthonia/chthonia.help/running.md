# Running a program

**Run ▸ Run** (F5 in the Chthonia key style) runs the buffer's program
in the shell: a fresh session loads the buffer, unsaved edits included,
and runs its `main`. What the program prints appears in the shell, and
what it reads is typed there. When it finishes, its names stay at the
shell's prompt, and the **Variables** view lists them.

**Run ▸ Stop** stops a program that is still running.

**Run ▸ Language…** chooses the language standard programs are compiled
as.

**Build ▸ Build…** lists the build's steps: check the program, build a
native program from it, run that program, stop it. A build's messages
appear in **Output**, and the program it runs runs in **Terminal**.

See also: [The shell](shell.md).
