# tgterm

## Motivations

Since coding agents started reshaping the way we write code, many developers started to feel the need to access their terminals while away from the keyboard. Providing instructions to agents, for them to continue the development, is now possible without typing much, just observing, evaluating, and sending further instructions: something that can be done via a phone keyboard.

The usual setup now is to: setup some kind of SSH tunneling, or VPN between the computer and the phone, and then use Termux or a plain mobile SSH client in order to access remote sessions. But, I consider Telegram a better way to access my sessions for the following reasons:

1. Telegram is already on your phone, and the content of your terminals arrives as messages: you can read them at any size, copy from them, and have them updated while a program runs. To type, you just write a Telegram message, that is what you need to work with Claude Code, Codex and other agents.
2. No setup burden of SSH tunnels, VPNs, ...
3. On macOS, no need to remember to start sessions multiplexed with tmux: the tabs of Terminal.app can be controlled as well.

So I built this project, that allows to control tmux panes and Terminal.app tabs using Telegram.

## Version 2: from screenshots to text

The first version of tgterm captured screenshots of the terminal windows of a Mac, and injected keystrokes into them. Version 2 reads the terminal content as text instead, via tmux or Terminal.app scripting, for three reasons:

- Text is more usable from a phone: it is readable at any size, can be copied, and messages are small.
- Text can be streamed: the bot keeps the screen message updated as the program runs, something impractical with screenshots.
- Crucially, it works even when the Mac is locked or the screensaver is running, which prevents any program from taking screenshots of the windows.

The screenshot-based version is preserved in the `screenshot-based` branch, for the cases where it is still needed, like watching graphical output.

This is how it works:

1. You talk with a Telegram bot, that you create only for yourself.
2. After you setup your TOTP, you send the bot the first message, and you become its owner. It will only accept queries from you (your Telegram ID) and will require you to authenticate with an OTP for the first time, and again after a timeout.
3. At this point, you can ask for the list of terminals with `.list`, connect to one of them with (for instance) `.2`, then you can send any text that will be "typed" in the terminal, like if you are still at your computer. You have modifiers, ways to send `ESC`, and so forth.

## First run

To setup the project:

1. Create a Telegram bot via [@BotFather](https://t.me/botfather) and get the API key.
2. Install `tmux`, `libcurl` and `libsqlite3`. The project also uses my own `botlib` but it is included directly into the project, so no need to install anything.
3. Build with `make` and run:

```
./tgterm --apikey <your-api-key>
```

4. On the first run, the bot will display a QR code and a text secret on your terminal:

```
=== TOTP Setup ===
Scan this QR code with Google Authenticator:

 ▄▄▄▄▄▄▄  ▄▄ ▄▄▄   ▄    ▄  ▄▄▄▄▄▄▄
 █ ▄▄▄ █ █▄▄█▀▀ █▀▄▄▄▄▀▄█▀ █ ▄▄▄ █
 ...

Or enter this secret manually: KAPLXWZSYXOR64Y49HG75IIFXHSQLHTY
==================
```

Open Google Authenticator (or any TOTP app), scan the QR code or type the secret manually. This is the only time the secret is shown — you won't see it again on subsequent runs.

5. Send any message to your bot on Telegram (e.g. `/start` or just `hello`). The first user to message becomes the **owner**. The bot will only accept messages from you from now on.

6. The bot will reply asking for your OTP code. Type the 6-digit code from your authenticator app to unlock it.

You're ready. Type `.list` to see your terminals and `.help` for the full command reference.

## Usage

### Commands

- `.list` — List the panes of all the tmux sessions, as `session:window.pane` followed by the window name and the command running in the pane, then the tabs of all the Terminal.app windows, as `Terminal window.tab` followed by the tab title and the command running in it.
- `.1`, `.2`, ... — Connect to a terminal by its number.
- `.stream` — Keep the screen updated as the terminal content changes.
- `.stop` — Stop streaming.
- `.esc`, `.ctrl_c`, `.enter`, `.tab`, `.shift_tab`, `.up`, `.down`, `.ctrl_d` — Send the corresponding key.
- `.help` — Show the help message.
- `.otptimeout <seconds>` — Set the OTP inactivity timeout (range: 30–28800 seconds). Default is 300 (5 minutes).

The Telegram commands menu offers the same commands as `/_list`, `/_esc` and so forth, that the bot accepts as well: Telegram menus can only hold slash commands, and the underscore keeps them apart from the slash commands of the programs you control, like coding agents. Any other message starting with `/` is typed into the terminal as is. The key commands are handy from the menu: two taps to send ESC or Ctrl+C.

### Sending keystrokes

Once connected to a terminal, any text you send is typed into it as keystrokes. A newline (Enter) is automatically appended at the end.

**Suppressing the newline:** End your message with 💜 to prevent the automatic Enter at the end. This is useful for partially typing a command or entering text without submitting it.

**Modifier emojis:** The `.help` message includes modifier emojis formatted as code **so that you can tap to copy them on your phone**. Paste a modifier before a character to send that key combination:

- ❤️ — Ctrl (e.g. `❤️c` sends Ctrl+C)
- 💙 — Alt
- 💛 — ESC (sends Escape immediately, no following key needed)
- 🧡 — Enter (sends Enter at that position, useful for multi-line input)

Modifiers can be combined: `❤️💙x` sends Ctrl+Alt+X. A single modified keystroke (like `❤️c`) will not have an automatic newline appended. Modifiers work with ASCII keys only, while plain text can contain any UTF-8 character.

**Escape sequences:** `\n` sends Enter, `\t` sends Tab, `\\` sends a literal backslash.

### Screens

After every keystroke message, the bot waits briefly for the program to update and then sends back the visible content of the terminal, as a monospace block. The message includes a 🔄 Refresh button that you can tap to get an updated screen without sending any keystrokes.

Telegram messages are limited to 4096 characters: when the terminal content is larger, only the last lines are shown. Colors and other attributes are not shown, since Telegram does not allow formatting inside monospace blocks.

### Streaming

Type `.stream` to have the bot keep the screen updated: the screen message is edited in place every time the terminal content changes, at most every 2 seconds, so that you can watch a program working. While streaming, the button under the screen is ⏹ Stop instead of 🔄 Refresh: tap it, or type `.stop`, to stop.

Streaming always follows the last screen message: when you send keystrokes while streaming, the new screen message sent in reply is the one that gets updated from then on. Disconnecting, with `.list` or because the terminal is closed, stops streaming.

### Terminal.app tabs

On macOS the bot also lists the tabs of Terminal.app, if it is running, and controls them via AppleScript (the `osascript` command). The first time, macOS asks you to allow the program running the bot to control Terminal.

Terminal.app scripting has one limitation: text can only be typed followed by a newline, so 💜 has no effect on tabs, and a single key like `❤️c` or `.esc` is followed by Enter as well. Modifiers are sent as control characters (Alt as the ESC prefix), which is what the terminal would send for the same keys.

## Security

This tool allows remote control of terminals via Telegram. Given the sensitivity of this capability, multiple layers of security are in place:

**Ownership.** The first Telegram user to message the bot becomes its owner. All subsequent messages from other users are silently ignored. The owner's user ID is stored persistently in the database.

**TOTP authentication.** On first startup, the bot generates a TOTP shared secret and displays a QR code on the terminal for scanning with Google Authenticator (or any TOTP app). After a configurable period of inactivity (default 5 minutes), the bot locks and requires the owner to enter a valid one-time password before accepting further commands. This protects against scenarios where an attacker gains access to the owner's Telegram account. The timeout is configurable via `.otptimeout`.

**Resetting authentication.** To reset everything (owner, TOTP secret, all settings), delete the `mybot.sqlite` file and restart the bot. A new TOTP secret will be generated and a new QR code displayed.

**Disabling TOTP.** If you don't want OTP authentication (not recommended), run with `--use-weak-security`. The bot will still enforce ownership but will not require OTP codes.

## Credits

* **QR Code generator library** by [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library) — MIT license.
* **SHA-1 implementation** by Steve Reid — 100% public domain.
