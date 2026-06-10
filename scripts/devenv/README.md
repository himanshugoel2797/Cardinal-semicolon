# Cardinal; developer environment

A self-contained, **repo-local** toolchain so a clean machine can build the OS
with one command. Everything lives under the repo and is git-ignored:

| Path        | What                                                        |
|-------------|-------------------------------------------------------------|
| `.devenv/`  | conda env holding the whole toolchain (clang/lld/llvm, cmake, ninja, astyle) |

## Toolchain choice

The cross compiler is **Clang/LLVM**. Clang is natively a cross compiler, so we
build the freestanding kernel with `--target=x86_64-elf` directly — there is no
separate GCC/binutils cross build to babysit. `lld` is the linker and
`llvm-tools` supplies `llvm-ar` / `llvm-objcopy` / `llvm-nm` / `llvm-ranlib`.

> The historical `x86_64-elf-cardinalsemi-gcc` (a patched GCC with a custom
> Cardinal target + newlib) is retired.

## Usage

```bash
# One-time (or after editing environment.yml): build the env + verify the toolchain
./scripts/devenv/setup.sh

# Each shell where you want to build: put the toolchain on PATH
source ./scripts/devenv/activate.sh
```

`setup.sh` is idempotent — re-running updates the env in place.

## Not in the conda env

A few tools are not packaged for conda-forge/linux-64 and are only needed to
build/boot a **bootable ISO** (not to compile the kernel + modules). Install
them from your system package manager if you want them:

```bash
sudo apt install qemu-system-x86 grub-pc-bin grub-efi-amd64-bin mtools xorriso
```
