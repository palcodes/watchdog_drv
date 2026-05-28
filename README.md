# watchdog_drv
A Linux kernel module that watches a process by PID and streams live status logs to userspace via a char device at `/dev/procwatch`.

## Kernel subsystems used

| Subsystem | Purpose |
|---|---|
| `cdev` / `chrdev_region` | Char device registration; udev creates `/dev/procwatch` automatically |
| `hrtimer` | High-resolution kernel timer; fires every 500ms to poll process state |
| `kfifo` | Lock-efficient ring buffer; timer callback pushes log lines, `read()` drains them |
| `wait_queue` | Blocks the userspace `read()` call until the fifo has data |
| `task_struct` | Direct kernel struct access; reads `comm`, `nvcsw`, `exit_state`, state char |
| `find_get_pid` / `pid_task` | Safe PID-to-task resolution under RCU |
| `procfs` | `/proc/procwatch`: lightweight status endpoint; `cat` shows current watch state |

---


**On write:** `wd_write()` receives the PID as an ASCII string, validates it, resolves the task, and starts the `hrtimer`.

**On timer tick:** `timer_cb()` resolves the PID under RCU, reads `task->comm` (process name), `task->nvcsw` (voluntary context switches, a lightweight activity proxy), and `task_state_to_char()` (`R`/`S`/`D`/`Z`). Pushes a formatted line into the `kfifo` and wakes the read wait queue.

**On read:** `wd_read()` blocks on a `wait_queue` until the fifo is non-empty, then copies to userspace via `kfifo_to_user()`.

**On process exit:** `exit_state` is non-zero; timer emits `PROCESS ENDED`, cancels itself, and clears the watch state.

---

## Terminal output

```sh
watchdog  pid 9029
─────────────────────────────────────────────────────
12:04:01  [watchdog] watching PID 9029 (firefox) — polling every 500ms
12:04:01  [watchdog] PID 9029  name=firefox           up=0s  nvcsw=1420  state=S
12:04:02  [watchdog] PID 9029  name=firefox           up=1s  nvcsw=1421  state=S
...
12:04:47  [watchdog] PID 9029 — PROCESS ENDED
─────────────────────────────────────────────────────
watchdog detached
```

---

## Build and run

```sh
make              # builds kernel module + wd binary
make load         # sudo insmod watchdog_drv.ko
./wd <pid>        # attach to any running process
```

Check current watch state at any time:

```sh
cat /proc/procwatch
```

Unload:

```sh
make unload       # sudo rmmod watchdog_drv
```

> Requires kernel headers: `sudo apt install linux-headers-$(uname -r)`

---
