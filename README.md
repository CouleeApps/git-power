# `git-power`
emPOWer your commits. Pointlessly flex on your coworkers with bespoke commit hashes,
all with the convenience of a single command. 

[<img width="795" alt="demo" src="https://user-images.githubusercontent.com/711973/124827890-92e23300-df44-11eb-8702-7627f7c4170b.png">](https://github.com/CouleeApps/git-power/commits/master)

## What is it?
A [Proof of Work](https://en.wikipedia.org/wiki/Proof_of_work) is a cryptographic proof 
that an amount of work has been done. Often, these are seen in the form of
`Hash(data || nonce)` where the result of the hash has some number of leading zero bits. 
Since hashes are one-way functions, this is effectively a O(2^n) brute force for n leading
zero bits. Since git commits are identified with a hash, and you can insert arbitrary
fields into a commit header, you can generate many hashes for one set of changes and
effectively compute a Proof of Work for a git commit. This tool does that, with a
configurable number of threads and leading zero bits on the commit hash.  

## Why?
Some joke about "Git is a blockchain" went too far, now we have this.

## Is it fast?
Reasonably. On my Intel i9 9880H @ 2.3GHz with 16 threads, it can compute about 12MH/s at
peak CPU core boost clocks. If you account for the less-than-stellar MacBook thermals, it
drops to about 8MH/s. But assuming you can get good speeds, and assuming you want to
calculate a hash with 32 leading zero bits, this should take
(2^32 / 12,000,000) ~= 360 seconds on average, though the variance is pretty high.
Hashcat's benchmark reports my CPU can do about 315MH/s for SHA-1, so OpenSSL's hash 
implementation is probably not optimized well for this. Maybe someone can look into
adapting Hashcat into this, but it's a bit beyond the scope I'm willing to do. 

## Usage

    git-power [bits [threads]]

`git-power` operates on the git repository in the current working directory.
The only supported run options are the number of leading bits to brute-force and the
number of threads created to do the work. By default, `git-power` will use 32 bits and
the max number of hardware threads supported.

When a matching commit hash is found, it will automatically update your repository HEAD
and replace the latest commit. NEW: If your commit is GPG-signed, it will stay signed
even after running this! See the source for details on how this witchcraft is performed.

### Retroactively

If you want to retroactively emPOWer all of your commits, you can combine `git power`
with the brilliance of `git rebase --interactive`:

    # emPOWer entire tree (preferred method)
    git rebase --interactive --exec "git power" --root

    # emPOWer unpushed commits
    git rebase --interactive --exec "git power" origin/master

    # emPOWer everything after a specific commit
    git rebase --interactive --exec "git power" 00000000da6a1220576d8c00dff8aa9619b44048

## Building

### Linux
This tool requires `cmake`, `libgit2`, and `OpenSSL` to build. You can get `libgit2` and
`OpenSSL` through your package manager, or at [libgit2.org](https://libgit2.org/).
Build steps are straight-forward from there:

    cmake -B build && cmake --build build

### macOS
On macOS, Apple is rude and won't let you link with the system-provided `libcrypto`, so
you need to `brew install openssl` (or build it yourself). Then you can pretend you have a
real unix system:

    cmake -B build -DOPENSSL_ROOT_DIR=/usr/local/opt/openssl && cmake --build build

### Windows
On Windows, you need to compile `libgit2` and `OpenSSL` yourself. Then just point `cmake`
at them, and you should be good: 

    # Be sure to specify the correct arch
    cmake -B build -A x64 "-DCMAKE_PREFIX_PATH=C:\Program Files\libgit2" "-DOPENSSL_ROOT_PATH=C:\Program Files\OpenSSL"
    cmake --build build

## Installing

### macOS / Linux
First, install via `cmake`:

    cmake --install build

Then, you can use it through `git` like any other utility:

    # Default settings: 32 and <hardware thread count>
    git power

    # MacBook-friendly
    git power 24 8

### Windows
Drop `git-power.exe`, `git2.dll`, and `crypto.dll` in your git installation's `bin`
directory. On my machine, that's at `C:\Program Files\Git\mingw64\bin`. Then you can use
it like normal.

    # Default settings: 32 and <hardware thread count>
    git power
    
    # APU-friendly
    git power 24 8

## License
MIT license. I'm really not sure who would want to reuse this, but it's here if you want.

## How long did you spend running it on this repo
Too long.

## Will you Rewrite it in Rust (TM)?
~~It wouldn't be too hard. Feel free to submit a pull request whose commit hash starts with
at least 32 zero bits.~~ [I have received news that someone is doing this.](https://github.com/mkrasnitski/git-power-rs)

## Please apologize for creating this
Sorry.
