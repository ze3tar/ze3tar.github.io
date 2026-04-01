---
layout: post
title: "Poking at Sublime Text internals: things I did not expect to find"
date: 2026-04-01
categories: research
tags: [sublime-text, linux, internals, ipc, build-systems]
description: "I started trying to understand how Sublime Text communicates with its own CLI. I ended up somewhere I did not plan to go. Notes from that journey."
---

I have been using Sublime Text for years without thinking much about how it actually works. I type, it renders, builds run. That is usually enough. But a few weeks ago I started wondering about one specific thing: when you run `subl somefile.py` in a terminal while Sublime is already open, how does that command reach the running GUI? There is no parent-child relationship. They are separate processes. Something is passing messages between them.

That question is what started this. What follows is everything I learned by following it, including a few places where the internals behaved in ways I did not expect.

---

## The socket in /tmp

The first thing I did was run `ls /tmp` while Sublime was open.

```
/tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock
```

A Unix domain socket. The hash suffix is derived from the Sublime binary itself, which means it is the same value on every machine running the same build. Not random, not session-based. Consistent.

The socket has permissions `0755`. On Unix, connecting to a domain socket requires write permission on the socket file, so only the owner (whoever is running Sublime) can connect. But the path is predictable and the file is visible to everyone on the system.

When I ran `strace` on the `subl` binary, the whole communication model became clear:

```
execve("/usr/bin/subl", ["subl", "somefile.py"], ...)
execve("/opt/sublime_text/sublime_text", [..., "--fwdargv0", "/usr/bin/subl", "somefile.py"], ...)
connect(3, {sa_family=AF_UNIX, sun_path="/tmp/Sublime Text.18bba2f919d04e82c4d74b410f5a4aca.sock"}, 110)
```

The `subl` wrapper is a single-line shell script:

```sh
#!/bin/sh
exec /opt/sublime_text/sublime_text --fwdargv0 "$0" "$@"
```

It just calls the main binary with an extra flag. The binary then looks up the deterministic socket path and connects. No configuration file, no environment variable, no discovery mechanism. It knows the path because it can compute the same hash from its own binary.

---

## The wire protocol

Once I knew there was a socket, I wanted to understand what was going across it. I used `strace -e trace=write` and captured the raw bytes when running `subl somefile.py`.

The format is simple binary, little-endian:

```
[8 bytes]  payload length  (uint64)
[4 bytes]  message type    (uint32, always 9)
[4 bytes]  flags           (uint32)
[8 bytes]  padding         (zeros)
[4 bytes]  string count    (uint32, always 1)
[4 bytes]  string length   (uint32)
[N bytes]  string          (UTF-8)
[16 bytes] trailing zeros
```

The `flags` field is what determines what kind of request this is. `flags=0` means run a command. `flags=1` means open a file (the `--wait` mode). The string payload is either a command like `exec {"shell_cmd": "make"}` or a file path.

The response from the server is four bytes: just an exit code.

What stood out to me immediately is that there is no authentication in this protocol. No handshake, no token, no challenge. The binary connects and sends a command and the server runs it. The only protection is the filesystem permission on the socket file itself, which limits who can connect. We will come back to why that matters.

---

## What commands the socket accepts

Once I understood the framing I started experimenting with what commands the socket would execute. The `exec` command accepts a JSON argument with a `shell_cmd` key, and Sublime runs it through `/usr/bin/env bash -c`. I confirmed this by sending:

```python
import socket, struct, glob

sock_path = glob.glob("/tmp/Sublime Text.*.sock")[0]
cmd = 'exec {"shell_cmd": "env > /tmp/sublime_env.txt"}'
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

This ran silently, no UI feedback whatsoever. The file `/tmp/sublime_env.txt` appeared with the full process environment, which included things I had not thought about. The Sublime process inherits the entire desktop session environment. On my machine that included:

```
SSH_AUTH_SOCK=/root/.ssh/agent/s.RmUMDvbv44.agent.HumTNor1XO
SSH_AGENT_PID=2266
```

A running SSH agent, with the socket path exposed. Anything running as the same user with access to that socket can use it to authenticate as you to any server your SSH key has access to. That is not a flaw in Sublime specifically, it is just a consequence of how Unix process environments work, but it is worth being aware of. Any code that runs inside Sublime's `exec` command has access to your full session.

---

## The --wait flag and git integration

Sublime's own documentation recommends this setup:

```bash
git config --global core.editor "subl --wait"
```

With this in place, every git operation that needs a text editor (commits, rebases, tags) will open the file in Sublime and wait for it to be closed before continuing. The idea is that you edit, review, close the window, and git proceeds.

The `--wait` mode uses `flags=1` in the protocol, with the file path as the string payload. The CLI blocks until it receives the exit code response from the server.

I got curious about what happens during the startup sequence, specifically when Sublime is not yet running. Running `subl --wait somefile` when no socket exists triggers the full GUI to launch (Sublime forks itself), and then the CLI polls the socket path until it appears and connects. The GUI creates the socket on startup.

This means there is a window between when the old socket is gone (Sublime closed) and when a new one appears (Sublime reopened) where the path is unclaimed. During that window any process could create a file at that path. The `subl` binary does no ownership check before connecting. It just calls `connect()` on whatever socket exists at the known path.

I tested this. If you create your own socket at that path before Sublime does, the `subl` CLI will connect to your socket instead of the real one. You receive the packets, you can read the file path out of them, and you can return whatever exit code you want. The CLI has no way to tell the difference.

From a "how does this work" perspective this is interesting because it means the security model of `subl --wait` as a gate (the assumption that someone reviewed the file in Sublime before the exit code was returned) depends entirely on Sublime holding the socket. It is not enforced by the CLI itself.

---

## How build systems actually run

I spent a while in `exec.py`, which is the default package file that handles build system execution. A few things in there surprised me.

The first was how environment variables flow from project files into builds. The `exec` build runner calls `self.window.active_view().settings().get('build_env')` to pick up additional environment variables, and it merges them into the subprocess environment. The source of those variables is the view's settings, which cascade through several layers. One of those layers is the `settings` key in a `.sublime-project` file.

That means a project file can inject environment variables into every build that runs in that project, without those variables being visible in the `build_systems` section. If someone puts this in a `.sublime-project`:

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

Every build that runs in that project, regardless of which build system it uses, will have a modified PATH. The user would not see this in the build system list because there is no entry there. It sits quietly in the `settings` key.

The second thing I noticed is that the `path` key in a build system definition temporarily modifies `os.environ["PATH"]` for the entire `plugin_host` process, not just the subprocess being spawned. The code saves and restores it in a try/finally block, so it is cleaned up correctly. But `plugin_host` is multithreaded, and between the modification and the restoration, other threads in the same process see the modified value. Whether that matters in practice depends on what else is happening concurrently.

The third thing, and probably the most surprising, is this line:

```python
if working_dir != "":
    os.chdir(working_dir)
```

The working directory for a build is set by calling `os.chdir()` on the `plugin_host` process itself. There is no cleanup after the build finishes. The process's current working directory is permanently changed to wherever the last build ran. I do not know if this causes real problems in practice but it is not the behavior I would expect from a build runner.

---

## Package auto-reload

Sublime loads Python plugins from `~/.config/sublime-text/Packages/User/`. That much I knew. What I did not know is how fast it reacts to new files appearing there.

I created a minimal plugin file:

```python
import sublime_plugin

class TestListener(sublime_plugin.EventListener):
    def on_activated(self, view):
        pass
```

Then I dropped it into `Packages/User/` and watched the Sublime console. Within about eight seconds, without any restart or manual reload, the plugin was loaded and active. Sublime has inotify watches on that directory and reacts to filesystem changes in near real time.

This is by design and it makes plugin development much smoother. But it does mean that write access to `Packages/User/` is equivalent to code execution inside the Sublime process, with access to everything the Sublime process has access to.

---

## The html_print behavior

The last thing I looked at was `html_print.py`. I opened it because the filename was in the directory listing and I had not looked at it yet. The file is short, about thirty lines, and most of it is straightforward. But one line made me pause:

```python
controller = webbrowser.get(using=view.settings().get('print_using_browser'))
```

The `print_using_browser` setting is passed directly to Python's `webbrowser.get()`. The intent is clearly to let users choose which browser opens the print preview. Reasonable idea. But Python's `webbrowser` module has a path that is easy to miss.

If the string passed to `get(using=...)` contains `%s`, the module does not look it up as a browser name. Instead it treats the whole string as a command template, splits it with `shlex.split()`, and wraps it in a `GenericBrowser` object that calls `subprocess.Popen` when you open a URL. The `%s` gets replaced with the URL.

So the setting `"print_using_browser": "xdg-open %s"` opens the file with `xdg-open`, as intended. But `"print_using_browser": "bash -c \"something\" %s"` runs `bash`. There is nothing in `html_print.py` that checks whether the setting contains a command template or validates it in any way.

And since `print_using_browser` is a view setting, and view settings are populated from project files through the `settings` key, a `.sublime-project` file can set it. The chain is:

1. Project file sets `print_using_browser` in `settings`
2. User opens the project in Sublime
3. User uses File > Print
4. `html_print.py` reads the setting and passes it to `webbrowser.get()`
5. `webbrowser.get()` sees `%s`, treats it as a command, runs it

I verified this works by creating a test project with the setting pointing at a harmless command and confirming the output. It behaves exactly as described.

The interesting question this raises is: what is the intended trust boundary for project settings? The `settings` key in a `.sublime-project` can affect a lot of things beyond just editor appearance. There is currently no distinction between settings that affect UI and settings that affect process execution. Sublime applies them all the same way.

VS Code addressed this with workspace trust, a model where projects from outside your own directories require explicit approval before their settings take effect. Sublime has no equivalent mechanism. Whether that matters to you depends on whether you open project files from sources you do not fully control.

---

## What I took away from this

The IPC architecture is clean and simple. One socket, one binary protocol, no dependencies. Easy to understand.

The place where my mental model shifted the most was around project files. I had thought of `.sublime-project` as a relatively inert configuration file that stores folder paths and maybe some syntax associations. After reading through how settings cascade and what settings are actually consumed by running code, I think of them differently now. A project file can influence build environment variables, the browser used for print preview, and potentially other things I have not traced yet.

This is not necessarily wrong, it is just more powerful than it looks. Worth knowing if you are the kind of person who opens project files from repositories without reading them first, which is most people, most of the time.

---

*Tested on: Linux 6.18.9+kali-amd64, Sublime Text Build 4200*
