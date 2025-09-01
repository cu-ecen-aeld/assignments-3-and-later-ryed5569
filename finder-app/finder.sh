#!/bin/sh

# finder-app/finder.sh
# Usage: finder.sh <filesdir> <searchstr>

# Fail on unset vars and pipe errors
set -u

# Requirements 1) and 2) using var names of filesdir and searchstr
# Validate arguments and return 1 if any arent specified
if [ $# -ne 2 ]; then
  echo "Error: Expected 2 arguments: <filesdir> <searchstr>"
  exit 1
fi

# our var names filesdir and searchstr
filesdir="$1"
searchstr="$2"

# Requirement 3)
# Verify filesdir is a directory
if [ ! -d "$filesdir" ]; then
  echo "Error: '$filesdir' is not a directory"
  exit 1
fi

# Count files recursively (regular files only)
X=$(find "$filesdir" -type f | wc -l)

# Requirement 4)
# Count matching lines across all files
#    -R: recursive, -F: fixed string (not regex)
#    Ignore binary matches; suppress errors for unreadable files.
Y=$(grep -R -F --binary-files=without-match "$searchstr" "$filesdir" 2>/dev/null | wc -l)

echo "The number of files are ${X} and the number of matching lines are ${Y}"
