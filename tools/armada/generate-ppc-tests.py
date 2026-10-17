#!/usr/bin/env python3
"""Assemble SDK instruction fixtures and generate tests with the host rexglue."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rexglue", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=root / "out/ppc-tests")
    args = parser.parse_args()
    sdk = root / "thirdparty/rexglue-sdk"
    binaries = args.output.resolve() / "bin"
    generated = args.output.resolve() / "generated"
    binaries.mkdir(parents=True, exist_ok=True)
    generated.mkdir(parents=True, exist_ok=True)
    tools = sdk / "tools/binutils"
    assembly = sdk / "tests/ppc/asm"

    def assemble(source):
        obj = binaries / (source.stem + ".o")
        subprocess.run([tools / "powerpc-none-elf-as", "-a32", "-be", "-mregnames",
                        "-mpower7", "-maltivec", "-mvsx", "-mvmx128", "-R",
                        "-o", obj, source], check=True)
        subprocess.run([tools / "powerpc-none-elf-ld", "-A", "powerpc:common32",
                        "-melf32ppc", "-EB", "-nostdlib", "--oformat=binary",
                        "-Ttext=0x82010000", "-e", "0x82010000", "-o",
                        binaries / (source.stem + ".bin"), obj], check=True)
        with (binaries / (source.stem + ".map")).open("w") as symbols:
            subprocess.run([tools / "powerpc-none-elf-nm", "--numeric-sort", obj],
                           stdout=symbols, check=True)

    sources = sorted(assembly.glob("*.s"))
    with ThreadPoolExecutor(max_workers=4) as workers:
        list(workers.map(assemble, sources))
    subprocess.run([args.rexglue.resolve(), "recompile-tests", f"--output={generated}",
                    f"--bin-dir={binaries}", f"--asm-dir={assembly}"], check=True)
    print(f"Generated instruction tests from {len(sources)} assembly fixtures in {generated}")


if __name__ == "__main__":
    main()
