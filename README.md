# `git-power`
emPOWer your commits.

## What is it?
A [Proof of Work](https://en.wikipedia.org/wiki/Proof_of_work) is a cryptographic proof 
that an amount of work has been done. Often, these are seen in the form of
`Hash(data || nonce)` where the result of the hash has some number of leading zero bits. 
Since git commits are identified with a hash, and you can use the timestamps as a nonce,
you can generate many hashes for one set of changes and effectively compute a Proof of
Work for a git commit. This tool does that, with a configurable number of threads and
leading zero bits on the commit hash.  

## Why?
Some joke about "Git is a blockchain" went too far, now we have this.

## Is it fast?
Reasonably. On my Intel i9 9880H @ 2.3GHz with 16 threads, it can compute about 6MH/s.
Assuming you want to calculate a hash with 32 leading zero bits, this  should take
(2^32 / 6,000,000) ~= 715 seconds on average, though the variance is pretty high.
Hashcat's benchmark reports my CPU can do about 315MH/s for SHA-1, which smells like
the algorithm in `libgit2` is not very well-optimized. Since this is not a serious 
attempt at brute forcing, getting ~2% of maximum optimized performance is pretty good.

## Usage

    git-power [bits [threads]]

`git-power` operates on the git repository in the current working directory.
The only supported run options are the number of leading bits to brute-force and the
of threads created to do the work. By default, `git-power` will use 32 bits and the max
number of hardware threads supported.

When a matching commit hash is found, it will automatically update your repository HEAD
and replace the latest commit. Note that this will break any GPG signature on the commit
(for obvious reasons), but all other non-date metadata will be maintained.

## Building

This tool requires `cmake` and `libgit2` to build. You can get `libgit2` through your
package manager, or at [libgit2.org](https://libgit2.org/).
Build steps are straight-forward from there:

    cmake -B build && cmake --build build

## Installing

First, install via `cmake`:

    cmake --install build

Then, you can use it through `git` like any other utility:

    git power 32 16


## License
MIT license. I'm really not sure who would want to reuse this, but it's here if you want.

## Will you Rewrite it in Rust (TM)?
It wouldn't be too hard. Feel free to submit a pull request whose commit hash starts with
at least 32 zero bits.

## Please apologize for creating this

Sorry.