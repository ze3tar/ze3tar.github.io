---
layout: post
title: "Sublime Text RCE via print_using_browser"
date: 2026-03-31
categories: research
tags: [sublime-text, rce, linux, supply-chain, cve, disclosure]
description: "A single project setting in Sublime Text 4 turns the Print command into arbitrary code execution. No build system needed. No suspicious files. Just File > Print."
---

I was not looking for this one. I was deep in something else when I noticed it.

I had been reading through `exec.py`, the file that handles build systems in Sublime Text's default package. The goal was to understand how environment variables flow from a `.sublime-project` file into the actual build process. That work was going fine, nothing unexpected. But on the way out I opened `html_print.py` because the filename stood out in the directory listing. Print-to-HTML is not exactly a feature most people think about. I figured it would be ten lines and boring.

It was thirty lines. And one of them stopped me.

```python
controller = webbrowser.get(using=view.settings().get('print_using_browser'))
```

The function reads a setting called `print_using_browser` from the current view's settings, then passes it directly to Python's `webbrowser.get()`. No validation. No allowlist. Whatever is in that setting goes straight into the browser selection logic.

My first thought was: that setting is probably just a browser name. "firefox", "chromium", something like that. Not interesting.

My second thought was: let me actually check what `webbrowser.get()` does with an arbitrary string.

---

## What Python's webbrowser module does

The `webbrowser` module has a little-known behavior. If the string you pass to `get(using=...)` contains `%s`, it does not try to look up a browser by name. Instead it treats the entire string as a command template and wraps it in a `GenericBrowser` object that will call `subprocess.Popen` with that command when you open a URL.

```python
def get(using=None):
    if using is not None:
        alternatives = [using]
    ...
    for browser in alternatives:
        if '%s' in browser:
            browser = shlex.split(browser)
            if browser[-1] == '&':
                return BackgroundBrowser(browser[:-1])
            else:
                return GenericBrowser(browser)
```

And `GenericBrowser.open()`:

```python
def open(self, url, new=0, autoraise=True):
    cmdline = [self.name] + [arg.replace("%s", url) for arg in self.args]
    p = subprocess.Popen(cmdline, close_fds=True)
    return not p.wait()
```

So if the setting contains `%s`, Python splits the string, the first token becomes the binary name, everything else becomes arguments, and then it runs it. The URL of the printed HTML file gets substituted wherever `%s` appears.

I tested this in isolation before touching Sublime at all:

```python
import webbrowser, os

using = 'bash -c "id > /tmp/webbrowser_test.txt" %s'
controller = webbrowser.get(using=using)
controller.open_new_tab('file:///tmp/test.html')
```

```
uid=0(root) gid=0(root) groups=0(root),126(docker)
```

That ran immediately. No prompts, no warnings, no sandboxing. The command executed as whoever is running Sublime.

---

## Getting the setting into Sublime

The next question was how an attacker would actually set `print_using_browser` without already having code execution. If the only way to set it was through the user's own preferences file, this would just be a way to shoot yourself in the foot.

Sublime Text has a settings hierarchy. Settings can come from default package files, user preferences, and project files. The `settings` key inside a `.sublime-project` file feeds directly into `view.settings()` for every view open in that project. That means any key in the project settings overrides the user's preferences for views in that project, and `html_print.py` reads `print_using_browser` from `view.settings()` without any fallback or restriction.

A minimal malicious project file looks like this:

```json
{
    "folders": [{"path": "."}],
    "settings": {
        "print_using_browser": "bash -c \"id > /tmp/pwned.txt\" %s"
    }
}
```

No `build_systems` entry. No custom commands. Nothing that looks remotely suspicious when you open the file in a text editor. It looks like a project with a single folder and one unusual setting that most developers would not recognize.

I confirmed that the setting flows correctly by reading it back from a running Sublime instance after opening the project:

```
print_using_browser = bash -c "id > /tmp/pwned.txt" %s
```

---

## The trigger

Once the project is open and the setting is active, the user needs to invoke the Print command. This is in the `File` menu as "Print..." and in the Command Palette as "File: Print". It has no `is_visible()` or `is_enabled()` guard in the source, so it appears for any open file regardless of type.

When triggered, `html_print.py` exports the current view to an HTML file, then calls `controller.open_new_tab(url)` where `controller` is the `GenericBrowser` wrapping the attacker's command. The `url` is a `file://` path to the temporary HTML file. It gets passed as the final argument to bash, which ignores it.

Result:

```
uid=0(root) gid=0(root) groups=0(root),126(docker)
```

The command ran as the Sublime process owner with no warnings, no prompts, and no indication in the UI that anything other than printing happened.

---

## Why this matters

The realistic attack path is a supply chain one. A developer clones a repository that contains a `.sublime-project` file. These files are common in open source projects as they store editor configuration that the whole team shares. The developer opens the project in Sublime, which is the normal thing to do when a project file is present. At some point during their session they use File > Print, or a teammate tells them to use it, or they hit it by accident through the Command Palette.

That is the entire attack. There is no build step required. There is no suspicious binary in the repository. The project file itself looks like a minor configuration detail.

The thing that makes this harder to notice than PATH injection through `build_systems` is that `print_using_browser` is not a setting most developers have ever seen. `build_systems` entries are visible in the Tools menu and recognizable as build configuration. A `settings` key with `print_using_browser` in it is background noise.

There is also no trust boundary in Sublime's project system that would warn the user before applying settings from a project file. VS Code introduced workspace trust for exactly this class of problem. Sublime has no equivalent. Opening a project file is an implicit grant of the same configuration authority as your own user preferences.

---

## Proof of concept

Full reproduction from a clean state:

```bash
# Create the malicious project
mkdir /tmp/evil_project
cat > /tmp/evil_project/evil.sublime-project << 'EOF'
{
    "folders": [{"path": "."}],
    "settings": {
        "print_using_browser": "bash -c \"id > /tmp/proof.txt\" %s"
    }
}
EOF

# Victim opens the project
subl --project /tmp/evil_project/evil.sublime-project

# Victim uses File > Print (or Command Palette: "File: Print")
subl --command 'html_print'

# Result
cat /tmp/proof.txt
```

Output:

```
uid=0(root) gid=0(root) groups=0(root),126(docker)
```

---

## Root cause

Two things have to be true for this to work.

First, `html_print.py` passes a user-configurable setting to `webbrowser.get()` without validation. The setting was presumably added to let users choose which browser opens the print preview. The intent is reasonable. The problem is that Python's `webbrowser.get()` has a code execution path that is not obvious from the function signature, and nobody appears to have considered what happens when `print_using_browser` contains `%s`.

Second, Sublime's project settings cascade into view settings without any trust check. A project file from an untrusted source has the same ability to set `print_using_browser` as the user's own preferences. There is no prompt, no warning, and no documentation that this is a capability project files have.

Either fix independently closes the issue. The simplest fix in `html_print.py` is a one-line check:

```python
browser_setting = view.settings().get('print_using_browser')
if browser_setting and '%s' in browser_setting:
    browser_setting = None  # reject command templates
controller = webbrowser.get(using=browser_setting)
```

The more complete fix is a project trust model where settings from project files outside the user's own directories require explicit approval before taking effect.

---

## Disclosure

- **2026-03-31** - Confirmed on Sublime Text Build 4200, Linux x86-64
- **2026-03-31** - Reported to security@sublimehq.com
- **CVE status** - Pending

*PoC available to Sublime HQ on request.*

---

*Environment: Linux 6.18.9+kali-amd64, Sublime Text Build 4200, Python 3.13.12*
