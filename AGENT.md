A Telegram bot to control tmux panes and macOS Terminal.app tabs: it reads
the screen content with `tmux capture-pane` or the AppleScript `contents`
property, and injects keystrokes with `tmux send-keys` or AppleScript
`do script`, running `osascript`. Version 1, that captured screenshots of
terminal windows and injected keystrokes with Core Graphics, is preserved
in the `screenshot-based` branch.

# File Structure

Put the file structure of the project below. Update if needed.

```
bot.c                  - Telegram bot main source
Makefile               - Build system
botlib.*, sds.*, cJSON.*, sqlite_wrap.*, json_wrap.* - From botlib
qrcodegen.c, qrcodegen.h - QR code generation (Nayuki, MIT license)
sha1.c, sha1.h             - SHA-1 + HMAC-SHA1 (Steve Reid, public domain)
```

# Development rules

- We don't add any dependency if possible. Ask the user if there is to add a dependency.
- Don't accept speed improvements that are just marginal, like 1%: they may be just random fluctations among runs. Refuse small speed improvements especially if they make the code more complicated, however more complex code for important speed improvements is ok.
- Always test a code modification after committing it. Don't assume things work.
- Once you reach a positive result, commit it.
- Never add or commit unstaged files, unless you created them for a specific purpose.
- Code must be simple and understandable.
- No dead code must be left around.
- Stick to standard C, no compiler-specific tricks, pragmas, ...

# Debugging

- `--debug` prints every Telegram API request. Screen messages are sent
  with POST requests, so the body is printed as well.
- tmux and osascript errors are silenced: run the same command by hand to
  see them. The AppleScript programs are the TERMINAL_*_SCRIPT macros in
  bot.c, they can be pasted in Script Editor.
