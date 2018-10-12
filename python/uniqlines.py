#!/usr/bin/env python
# vim: set ts=8 sts=4 sw=4 et filetype=python:
from __future__ import with_statement
import sys

def main():
    uniqlines = set()

    for filename in sys.argv[1:]:
        with open(filename) as f:
            for line in f:
                uniqlines.add(line)

        print "Total unique items in file '%s': %d" % (filename, len(uniqlines),)

if __name__ == "__main__":
    main()
