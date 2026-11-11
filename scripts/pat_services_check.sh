#!/bin/bash
# <PAT:Phase3> Looks for links/hosts of Firestorm services (firestormviewer.org, phoenixviewer.com)
# that the viewer could contact or show. Allowed on purpose:
#   - copyright / license header lines (www.firestormviewer.org, @phoenixviewer.com addresses)
#   - build-time dependency downloads (autobuild.xml: 3p.firestormviewer.org; not used at run time)
#   - disabled code and trusted-domain regexes: fsdata.cpp (PAT_FSDATA_DOWNLOADS_ENABLED=false), llurlentry.*
#   - tooltip texts "official domain ... SecondLife.com ... firestormviewer.org" (trusted-link icon, see llurlentry.cpp)
#   - tooltip texts "official domain SecondLife.com ... firestormviewer.org" (trusted-link icon, see llurlentry.cpp)
#   - macOS bundle ids, Firestorm-internal tooling (fsutils/, doc/), comments in this script
# Usage: scripts/pat_services_check.sh   (exit code 1 if anything is found)
cd "$(dirname "$0")/.." || exit 2
hits=$(git grep -I -n -E '(firestormviewer|phoenixviewer)' -- . \
  ':!autobuild.xml' ':!fsutils' ':!doc' ':!scripts' ':!*.po' ':!indra/newview/fsdata.cpp' \
  ':!indra/llui/llurlentry.cpp' ':!indra/llui/llurlentry.h' ':!.github' \
  | grep -v -E 'www\.firestormviewer\.org\s*$|@phoenixviewer\.com|www\.phoenixviewer\.com|org\.firestormviewer\.|<PAT:|// <FS:|SecondLife\.com')
if [ -n "$hits" ]; then echo "$hits"; exit 1; fi
echo "OK: no Firestorm service links found."
