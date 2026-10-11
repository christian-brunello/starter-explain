#!/bin/bash
# Generate Autotools configuration files
set -e

echo "Initializing Autotools..."

if [ -d "m4" ]; then
    aclocal -I m4
else
    aclocal
fi

autoheader
autoconf
libtoolize --copy --force
automake --add-missing --copy

echo "Autotools setup complete. Now run: ./configure"
