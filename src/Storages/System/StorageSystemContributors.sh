#!/usr/bin/env bash

echo "THIS IS HEAVILY DEPRECATED, USE tests/ci/version_helper.py:update_contributors()"
set -x

LC_ALL=C

# doesn't actually cd to directory, but return absolute path
CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# cd to directory
cd $CUR_DIR

CONTRIBUTORS_FILE=${CONTRIBUTORS_FILE=$CUR_DIR/StorageSystemContributors.generated.cpp}

# if you don't specify HEAD here, without terminal `git shortlog` would expect input from stdin
git shortlog HEAD --summary | perl -lnE 's/^\s+\d+\s+(.+)/    "$1",/; next unless $1; say $_' | sort > $CONTRIBUTORS_FILE.tmp

# If git history not available - dont make target file
if [ ! -s $CONTRIBUTORS_FILE.tmp ]; then
    echo Empty result of git shortlog
    git status -uno
    exit
fi

echo "// autogenerated by $0"             >  $CONTRIBUTORS_FILE
echo "const char * auto_contributors[] {" >> $CONTRIBUTORS_FILE
cat  $CONTRIBUTORS_FILE.tmp               >> $CONTRIBUTORS_FILE
echo -e "    nullptr};"                   >> $CONTRIBUTORS_FILE

echo "Collected `cat $CONTRIBUTORS_FILE.tmp | wc -l` contributors."
rm $CONTRIBUTORS_FILE.tmp

