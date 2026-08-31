#!/usr/bin/env python3
"""Extract ReShade64.dll from the official ReShade setup executable.

The setup exe is a self-extracting zip: the installer opens its own file as an archive
(setup/MainWindow.xaml.cs:987-1009 in the ReShade source). .NET's ZipArchive refuses it
because of the prepended PE image -- "Number of entries expected in End Of Central Directory
does not correspond to entries found" -- but Python's zipfile scans for the central directory
and handles prepended data fine.

We extract the SHIPPED binary rather than building ReShade ourselves, because the point of the
lane that uses this is to test the DLL the user actually runs.
"""
import sys
import zipfile

WANTED = "ReShade64.dll"


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <setup.exe> <output.dll>", file=sys.stderr)
        return 2
    setup, output = sys.argv[1], sys.argv[2]

    with zipfile.ZipFile(setup) as archive:
        names = archive.namelist()
        print(f"setup archive entries: {', '.join(names)}")
        if WANTED not in names:
            print(f"error: {WANTED} is not in the setup archive", file=sys.stderr)
            return 1
        data = archive.read(WANTED)

    # A PE always starts 'MZ'. Without this check a silently truncated or wrong payload would
    # be written and the failure would surface much later as a confusing load error.
    if data[:2] != b"MZ":
        print(f"error: extracted payload is not a PE (starts {data[:2]!r})", file=sys.stderr)
        return 1

    with open(output, "wb") as f:
        f.write(data)
    print(f"extracted {WANTED} -> {output}, {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
