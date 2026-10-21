# Preparing the Steam Frame

Frame Studio and the Frame Home installers talk to the headset over SSH, the standard way to log in to another
computer over the network. This guide sets that up once. After it, `ssh frame` logs you in to the headset without a
password, and Studio can send homes to it.

You need:

- the Steam Frame, on the same network as your computer,
- a computer with an SSH client (Windows 10 and 11, Linux and macOS all have one, see below).

## 1. Turn on developer mode and SSH

1. On the headset, open the system settings and turn on **developer mode**.
2. In the developer settings, turn on **SSH** (remote login).

The exact names of these options may change between SteamOS versions. If you cannot find them, look for "Developer"
in the headset's settings.

The headset's user name is `steamos`. To log in with a password the first time, that user needs one. If you have
never set it, switch the headset to Desktop Mode, open a terminal (Konsole) and run `passwd`.

## 2. Find the headset's address

You need either its IP address or a name your network knows it by.

- **In the headset:** the network (Wi-Fi) settings show the IP address of the connection, for example
  `192.168.1.50`.
- **In Desktop Mode:** open Konsole and run `hostname -I` (prints the IP addresses).
- **On your router:** its list of connected devices shows the headset and its address.

Some networks also let you reach a device by its host name with `.local` added (`<hostname>.local`). That depends on
your network, so the IP address is the safe choice.

Tip: give the headset a fixed address in your router (often called "DHCP reservation"). Then the address never
changes.

The examples below use `192.168.1.50`. Use your headset's address instead.

## 3. Check that SSH works

On your computer, open a terminal (on Windows: PowerShell or Command Prompt) and run:

```sh
ssh steamos@192.168.1.50
```

The first time, SSH asks whether you trust the headset's key. Type `yes`. Then enter the `steamos` password. If you
see a prompt on the headset, SSH works. Type `exit` to log out.

## 4. Set up an SSH key

Frame Studio never types a password for you: it needs a key. A key also saves you typing the password every time.

### Windows

Windows 10 and 11 include an SSH client called "OpenSSH Client". If `ssh` is not found, turn it on in
**Settings > Apps > Optional features**, or install Git for Windows (its SSH works too; Studio finds either).

In PowerShell:

1. Make a key (press Enter to accept the default file; a passphrase is optional):

   ```powershell
   ssh-keygen -t ed25519
   ```

2. Copy the public key to the headset (Windows has no `ssh-copy-id`, so this line does the same job):

   ```powershell
   type $env:USERPROFILE\.ssh\id_ed25519.pub | ssh steamos@192.168.1.50 "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 700 ~/.ssh && chmod 600 ~/.ssh/authorized_keys"
   ```

   Enter the `steamos` password one last time.

### Linux and macOS

1. Make a key:

   ```sh
   ssh-keygen -t ed25519
   ```

2. Copy it to the headset:

   ```sh
   ssh-copy-id steamos@192.168.1.50
   ```

If you already have a developer key for the headset, you can use that key instead of making a new one. Name it in
the `IdentityFile` line below.

## 5. Add a "frame" entry to your SSH config

Studio and the installers use the address `frame` unless you tell them otherwise. Make that name point at your
headset.

Open (or create) the SSH config file:

| System | File |
|---|---|
| Windows | `C:\Users\<you>\.ssh\config` (no file extension) |
| Linux, macOS | `~/.ssh/config` |

Add:

```
Host frame
    HostName 192.168.1.50
    User steamos
    IdentityFile ~/.ssh/id_ed25519
```

Replace `192.168.1.50` with your headset's address. On Windows, `~` in this file means your user folder.

## 6. Test it

```sh
ssh frame
```

You should be logged in to the headset without a password. Type `exit` to leave.

If it asks for a password, the key was not copied (step 4) or the `IdentityFile` line points at the wrong file. If
it cannot connect, check the address and that SSH is on. See [troubleshooting.md](troubleshooting.md).

## Using a different address

You do not have to use the name `frame`:

- In Studio, **Frame > Install on Steam Frame...** has a **Headset** field. Type an SSH alias or
  `steamos@<address>`. Studio remembers it and uses it for **Send to Frame** too.
- The installers take `--host` (Linux, macOS) or ask for the address (Windows). See
  [install-frame-home.md](install-frame-home.md).

Next: [install-frame-home.md](install-frame-home.md).
