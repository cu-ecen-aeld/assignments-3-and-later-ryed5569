#!/bin/sh
# finder-app/writer.sh
# Usage: writer.sh <writefile> <writestr>

# Strict mode for sh (no pipefail in POSIX sh)
set -u

# Requirements 1) and 2), use writefile and writestr and exit 1 if there arent not valid
# Validate arguments
if [ "$#" -ne 2 ]; then
  echo "Error: Expected 2 arguments: <writefile> <writestr>"
  exit 1
fi

# use var names writefile and writestr
writefile=$1
writestr=$2

# Create parent directory path if needed
dirpath=$(dirname "$writefile")
if ! mkdir -p "$dirpath" 2>/dev/null; then
  echo "Error: Could not create directory '$dirpath'"
  exit 1
fi

# Write content, overwriting if it exists
# Use printf without trailing newline so content equals writestr exactly
if ! printf "%s" "$writestr" > "$writefile"; then
  echo "Error: Could not create or write to '$writefile'"
  exit 1
fi
