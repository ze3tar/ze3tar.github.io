---
layout: post
title: "Trying to understand how Sublime Text works under the hood"
date: 2026-04-01
categories: research
tags: [sublime-text, linux, internals, ipc, build-systems]
description: "I started by asking one simple question: how does the subl CLI talk to the running GUI? Six weeks later I had filled three notebooks. These are the things I found along the way that made me stop and read the code twice."
---

I have been using Sublime Text for a long time. Long enough that I stopped thinking about it as software and started thinking of it as furniture. It is just there. I type, it renders.

A while back I started wondering about one specific thing. When you run `subl file.py` in a terminal while Sublime is already open, the file opens in the running window. How? There is no parent-child relationship between the terminal and the GUI. They are completely separate processes. Something is connecting them.

That question is what started this. I want to write down what I found because several of the behaviors I ran into were genuinely surprising to me, and I think they would be surprising to most people who use Sublime without thinking about its internals.

---

## The socket in /tmp

The first thing I did was run `ls /tmp` with Sublime open.

```
/tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock
```

A Unix domain socket. That answers the basic question. The `subl` CLI connects to this socket and sends a message, and the GUI receives it and does whatever was asked.

The hash in the filename is what caught my attention first. My assumption was that it would be random, regenerated each time Sublime starts, like a session token. It is not. I restarted Sublime several times and the hash never changed. I installed the same build on a second machine and got the same hash.

The hash is derived from the Sublime binary itself, not from any runtime state. Every machine running the same build version produces the exact same socket path without any coordination. This means if you know someone's Sublime version, you already know the socket path on their machine before you have touched it.

The socket permissions are `0755`:

```bash
stat "/tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock"
# Access: (0755/srwxr-xr-x)  Uid: (0/root)
```

On Linux, connecting to a Unix socket requires write permission on the socket file. `0755` means only the owner can connect. So the socket is visible to everyone but only the owner can use it while Sublime is running. That part makes sense.

---

## Reversing the wire protocol

I ran `strace` on the `subl` binary to see what it actually sends:

```bash
strace -e trace=write -xx -s 4096 subl --command 'exec {"shell_cmd":"id"}' 2>&1 | grep "write(3"
```

The format is a simple binary framing, little-endian:

```
[8 bytes]   payload length   (uint64)
[4 bytes]   message type     (uint32, always 9)
[4 bytes]   flags            (uint32)
[8 bytes]   zeros
[4 bytes]   string count     (uint32, always 1)
[4 bytes]   string length    (uint32)
[N bytes]   string payload   (UTF-8)
[16 bytes]  trailing zeros
```

The `flags` field determines what kind of request this is. `flags=0` means run a command. `flags=1` means open a file and wait (the `--wait` mode). The server's response is eight bytes: a length prefix and an exit code.

There is no authentication in this protocol. No token, no challenge, no session ID. The socket permissions are the entire security model. If you can connect, you can send any command.

I wrote a small script to send the `exec` command directly:

```python
import socket, struct, glob

sock_path = glob.glob("/tmp/Sublime Text.*.sock")[0]
cmd = 'exec {"shell_cmd": "id > /tmp/from_socket.txt"}'
cmd_b = cmd.encode()

payload = (
    struct.pack('<II', 9, 0) +
    struct.pack('<Q', 0) +
    struct.pack('<II', 1, len(cmd_b)) +
    cmd_b + b'\x00' * 16
)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
s.sendall(struct.pack('<Q', len(payload)) + payload)
s.close()
```

It worked. The file appeared with no UI activity, no prompt, nothing visible. The command ran inside Sublime's Python runtime as the user running Sublime.

The interesting side effect of running commands inside Sublime's process is that the process environment is the full desktop session environment. When I dumped it:

```bash
subl --command 'exec {"shell_cmd": "env > /tmp/st_env.txt"}'
```

The output included things like:

```
SSH_AUTH_SOCK=/root/.ssh/agent/s.RmUMDvbv44.agent.HumTNor1XO
SSH_AGENT_PID=2266
```

A live SSH agent socket. Any code running through the `exec` command has access to it. This is not specific to Sublime, it is just how Unix session environments work, but it is easy to forget that an editor you have open all day is sitting on top of your SSH agent.

---

## What happens when Sublime restarts

The socket disappears when Sublime closes and reappears when it starts again. While it is gone, the path in `/tmp` is unclaimed.

I got curious about what `subl` does when it cannot connect. Running `strace` while no socket exists:

```
connect(3, {sa_family=AF_UNIX, sun_path="/tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock"}, 110)
= -1 ECONNREFUSED
clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|...) = 32601
connect(3, {sa_family=AF_UNIX, sun_path="/tmp/..."}, 110) = 0
```

It forks a child, which launches the full GUI, then polls the socket path until it appears and connects. The child becoming the new Sublime window, the parent becoming the CLI client.

So the sequence during a restart is: socket disappears, a window opens where the path is gone, new socket appears. The `subl` binary connects to whatever file exists at that path when it polls. It does not check who owns the socket before calling `connect()`.

I tested this. If you create your own Unix socket at that path before Sublime does, `subl` will connect to it. You receive the packets, you can read whatever file path was passed, and you can return whatever exit code you want. The CLI cannot tell the difference because it does not check.

This has an interesting implication for the `subl --wait` pattern.

---

## subl --wait and the git editor use case

Sublime's documentation recommends this for git integration:

```bash
git config --global core.editor "subl --wait"
```

With this set, every git operation that opens an editor (commit messages, rebases, tags) calls `subl --wait`, which blocks until the file is closed in Sublime, then returns exit=0. Scripts use this pattern as a sign-off mechanism:

```bash
subl --wait /tmp/deploy_notes.txt
if [ $? -eq 0 ]; then
    ./deploy.sh
fi
```

The assumption is that exit=0 means a human opened the file, read it, and closed it. That assumption lives entirely in the socket. If you are sitting on the socket when `subl --wait` is called, you receive the file path in the packet, you can immediately return exit=0, and the calling script proceeds. The file never opened in any editor.

The `--wait` mode uses `flags=1` in the protocol. The string payload is the file path. A fake server receives it like this:

```python
hdr = conn.recv(8)
pkt_len = struct.unpack('<Q', hdr)[0]
data = conn.recv(pkt_len)

flags   = struct.unpack('<I', data[4:8])[0]   # 1 = file open (--wait)
str_len = struct.unpack('<I', data[8:12])[0]
path    = data[12:12+str_len].decode().rstrip('\x00')

# You now have the file path and can return whatever exit code you want
conn.sendall(struct.pack('<QI', 4, 0))   # exit=0
```

I confirmed this works. A process sitting on the socket can intercept `subl --wait` calls, read the file path from the packet (and the file contents if they are readable), and return any exit code immediately.

What I find interesting about this behavior is that it is not a traditional exploit in the usual sense. There is no overflow, no corruption. The protocol just has no way to verify that a human actually interacted with the file. The entire trust model is: socket owner equals legitimate Sublime process.

What makes it more interesting is the window during restarts. During that window, any local user can pre-create the socket. After that, any `subl --wait` call from any user on the machine connects to that socket instead.

---

## The world-writable file scenario

While reading through the `--wait` behavior I started thinking about what happens when the file passed to `subl --wait` is world-writable. The typical case I kept seeing in scripts was something like:

```bash
# CI pipeline generates a review file
chmod 0666 /tmp/pipeline_review.sh
# Privileged user reviews and runs it
subl --wait /tmp/pipeline_review.sh
if [ $? -eq 0 ]; then
    bash /tmp/pipeline_review.sh
fi
```

The `0666` permission exists because the pipeline agent and the privileged user running the review are different accounts. The file needs to be writable by both.

If someone is sitting on the socket when this runs, they receive the file path, they can check whether the file is world-writable, and if it is, they can overwrite it before returning exit=0. The privileged process then executes whatever is now in the file.

I tested this in a controlled setup with two separate UIDs. The flow confirmed: a process running as uid=1002 with no sudo, no special groups, and no other privileges can overwrite a root-owned file (if it has `0666` permissions) and then trigger root to execute it by returning exit=0 to a `subl --wait` call.

```
[mallory uid=1002] Intercepted /tmp/pipeline_review.sh
[mallory] File is world-writable. Overwriting.
[mallory] Returned exit=0.

[root] subl returned: 0
[root] Executing reviewed script...

CROSS_USER_RCE, uid=0(root) gid=0(root) groups=0(root),...
```

The condition (world-writable file in `/tmp` that gets executed after a review step) is more common than it sounds. Docker-based CI pipelines often produce files this way because the build container runs as one UID and the host executor runs as another. The sticky bit on `/tmp` only prevents deletion, not modification of files you have write access to.

---

## Build systems and the project settings cascade

I spent a lot of time in `exec.py`, which is the file in Sublime's default package that handles build system execution. A few things in there were not what I expected.

The first was how environment variables reach the subprocess. The build runner reads `self.window.active_view().settings().get('build_env')` and merges those values into the subprocess environment. That setting comes from `view.settings()`, which is a cascaded view of several sources. One of those sources is the `settings` key in a `.sublime-project` file.

This means a project file can inject environment variables into every build that runs in it by putting them in `settings` rather than in a `build_systems` entry:

```json
{
    "folders": [{"path": "."}],
    "settings": {
        "build_env": {
            "PATH": "/some/directory:${PATH}"
        }
    }
}
```

There is no `build_systems` entry here. The build menu looks empty. But every build that runs in this project, regardless of which build system it uses, has a modified `PATH`. I verified this by opening the project and running a build and checking the environment the subprocess received. The injected value was there.

The more visible version of this is the `env` key directly inside a `build_systems` entry:

```json
"build_systems": [{
    "name": "Build Project",
    "shell_cmd": "python3 build.py",
    "env": {
        "PATH": "/tmp/custom_bins:${PATH}"
    }
}]
```

When `Ctrl+B` runs this, Sublime sets `PATH` to the value from the project file before spawning the build subprocess. Whatever binary resolves first in that PATH is what gets called. If the project is from a repository you cloned, the `PATH` in the project file was written by whoever committed it.

I confirmed this by creating a fake `python3` wrapper, putting it at the start of the injected PATH, and triggering a build. The wrapper ran instead of the real Python. The build output looked normal because the wrapper called through to the real binary.

---

## Package auto-reload

Sublime loads Python plugins from `~/.config/sublime-text/Packages/User/`. I knew this. What I did not fully appreciate was how fast the reload happens.

I dropped a minimal Python file into that directory:

```python
import sublime_plugin

class TestListener(sublime_plugin.EventListener):
    def on_activated(self, view):
        pass
```

It was active within about eight seconds. No restart. No manual reload command. Sublime watches that directory with inotify and picks up new files immediately.

This is the right design for plugin development. But it means that write access to `Packages/User/` is functionally equivalent to having code running inside Sublime's Python process, with access to the full `sublime` API and the process environment including SSH sockets, credentials, and anything else inherited from the session.

The `exec` socket command can write there directly:

```
exec {"shell_cmd": "cp /tmp/myplugin.py ~/.config/sublime-text/Packages/User/"}
```

After which the plugin auto-loads and runs on every file activation. This survives Sublime restarts because the directory is part of the user's persistent configuration.

---

## The print_using_browser setting

This one I found by accident. I was going through the default package directory and `html_print.py` appeared in the listing. I opened it expecting nothing interesting. It is thirty lines. Most of it is straightforward. One line made me read it twice:

```python
controller = webbrowser.get(using=view.settings().get('print_using_browser'))
```

The `print_using_browser` setting is read from `view.settings()` and passed directly to Python's `webbrowser.get()`. The intent is clearly to let users choose which browser opens the print preview. That makes sense.

The part that made me pause is what `webbrowser.get()` actually does with its argument. If the string contains `%s`, the module treats it as a command template rather than a browser name:

```python
def get(using=None):
    ...
    for browser in alternatives:
        if '%s' in browser:
            browser = shlex.split(browser)
            if browser[-1] == '&':
                return BackgroundBrowser(browser[:-1])
            else:
                return GenericBrowser(browser)
```

`GenericBrowser` runs the command via `subprocess.Popen`, substituting the URL for `%s`. So `"xdg-open %s"` opens the URL with `xdg-open`, as intended. But `"bash -c \"something\" %s"` runs bash.

Since `print_using_browser` is a view setting and view settings are populated from the project `settings` key, a `.sublime-project` file can set it:

```json
{
    "folders": [{"path": "."}],
    "settings": {
        "print_using_browser": "bash -c \"id > /tmp/print_test.txt\" %s"
    }
}
```

I opened this project, used `File > Print`, and the file appeared. The command ran as the Sublime process owner with no prompts.

There is no `is_visible()` or `is_enabled()` guard on `html_print`, so the Print command is available for any open file. And because `print_using_browser` sits in the project `settings` key rather than a `build_systems` entry, there is no obvious signal that the project file is doing anything process-related.

---

## What ties all of this together

The common thread across everything I looked at is that Sublime's project file format has more power over the running process than it appears to.

A `.sublime-project` can inject environment variables into every build. It can set `print_using_browser` to a shell command. The `settings` key feeds into `view.settings()` which is consumed by multiple parts of the codebase. None of this is documented prominently. It looks like editor configuration.

The socket is a separate surface but it connects to the same picture. The protocol has no authentication, the path is deterministic, and there is a window during restarts where the path is available to anyone on the system. For `subl --wait` to work as a reliable sign-off mechanism, the socket needs to be trusted, and that trust rests entirely on the filesystem permissions at a predictable path.

The package auto-reload mechanism is by design and makes plugin development fast. It also means the plugin directory is a persistent execution environment that loads without any user action after a file is dropped there.

None of these things are broken in isolation in an obvious way. They are design decisions that interact with each other and with the environment around them in ways that are worth understanding if you rely on Sublime as part of any workflow that has a security requirement.

---

## A note on `subl --wait` specifically

If you use `subl --wait` in any script where the exit code has a security meaning (gate before deploy, git commit review, sudoedit-style patterns), be aware that the guarantee it provides is softer than it looks. The exit code comes from whoever is holding the socket, not from a verified human interaction.

A safer pattern for file review gates is to checksum the file before opening it and verify the checksum after the editor exits:

```bash
before=$(sha256sum /tmp/review_file.txt)
subl --wait /tmp/review_file.txt
after=$(sha256sum /tmp/review_file.txt)

if [ "$before" = "$after" ]; then
    echo "File unchanged. Was it actually reviewed?"
    exit 1
fi
```

This does not require trusting the exit code.

---

*Environment: Sublime Text Build 4200, Linux 6.18.9+kali-amd64*
