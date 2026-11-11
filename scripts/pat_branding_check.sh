#!/bin/bash
# <PAT:Rebrand> Searches for visible leftovers of the Firestorm/Phoenix branding.
#
# Scans only places a user can see (UI texts, setting descriptions, package/desktop
# files, string literals in C++), not internal identifiers (FS prefixes, file names,
# <FS:...> comments), see PLAN.md rule 5.
#
# Allowed on purpose (not reported):
#   - copyright/credit/"based on" statements (floater_about.xml, "Phoenix Firestorm Project")
#   - links to Firestorm services (firestormviewer.org, phoenixviewer.com): Phase 3
#   - Firestorm-specific service menu entries/notifications (Support Group, Blog, Wiki ...): Phase 3
#   - the "Phoenix" settings mode and the Phoenix beam (legacy feature names)
#   - client_list_v2.xml (list of foreign viewers' client tags)
#   - Windows/macOS-only resources (Info-Firestorm.plist, ...): not built yet
#
# Usage: scripts/pat_branding_check.sh   (exit code 1 if anything is found)
cd "$(dirname "$0")/.." || exit 2

python3 - <<'PYEOF'
import re, subprocess, sys, os

files = subprocess.check_output(['git', 'ls-files'], text=True).splitlines()

word = re.compile(r'(?<![\w.%/-])(firestorm|phoenix|fire storm)(?![\w])', re.I)

def scan_target(f):
    if not os.path.isfile(f):
        return None
    if re.match(r'indra/newview/skins/.*/xui/.*\.xml$', f) or f == 'indra/newview/skins/skins.xml':
        return 'xml'
    if re.match(r'indra/newview/app_settings/(settings.*|ignorable_dialogs|growl_notifications|commands|quick_preferences)\.xml$', f):
        return 'xml'
    if re.match(r'indra/newview/(viewer_manifest|fs_viewer_manifest)\.py$', f):
        return 'py'
    if re.match(r'indra/newview/linux_tools/', f):
        return 'txt'
    if f in ('indra/CMakeLists.txt',):
        return 'cmake'
    if f.endswith(('.cpp', '.h')) and re.match(r'indra/', f):
        return 'cpp'
    return None

# lines that are fine
ok_line = re.compile(
    r'firestormviewer|phoenixviewer|Phoenix Firestorm|Phoenix/Firestorm|The Phoenix|Firestorm Project|'
    r'basiert auf|based on (the )?Firestorm|<FS:|</FS|<FS |\[FIRESTORM\]|Copyright|copyright|\(C\)|'
    r'Linden Lab|Lizenz|license|License', re.I)
# Firestorm services (Phase 3)
service_line = re.compile(
    r'Firestorm[ _]?(Blog|Primfeed|Flickr|YouTube|Twitter|Plurk|MoTD|Wiki|Support|Classes|Events|Social|Tutorial|Group)|'
    r'Phoenix[ /]|Phoenix Support|SUPPORT_SITE|Support Portal|name="Phoenix"|mode_settings_phoenix|#Phoenix|'
    r'PhoenixBeam|Phoenix Shaped|"Phoenix"|InvFolder|FSBeamShape|<string>firestorm</string>|<string>Phoenix</string>|'
    r'^\s*Phoenix\s*$|FS_BRIDGE_NAME|FS_PF_USER_AGENT|ROOT_FIRESTORM_FOLDER|\[PHOENIX\]|LLCommandHandler\("firestorm"|'
    r'Firestorm\.app|dst_path_of\("Firestorm"\)|dmg_template_prefix', re.I)
skip_files = re.compile(r'floater_about\.xml$|floater_test_textbox\.xml$|client_list_v2\.xml$|/fs_static_assets/')

def comment_only(line, kind):
    s = line.strip()
    if kind == 'xml':
        return s.startswith('<!--') or s.endswith('-->')
    if kind in ('cpp',):
        return s.startswith('//') or s.startswith('*') or s.startswith('/*')
    if kind in ('py', 'cmake', 'txt'):
        return s.startswith('#')
    return False

hits = []
for f in files:
    kind = scan_target(f)
    if not kind or skip_files.search(f):
        continue
    try:
        text = open(f, encoding='utf-8', errors='replace').read().splitlines()
    except OSError:
        continue
    in_comment = False
    for n, line in enumerate(text, 1):
        if kind == 'xml':
            if '<!--' in line and '-->' not in line:
                in_comment = True
            if in_comment:
                if '-->' in line:
                    in_comment = False
                continue
        if not word.search(line):
            continue
        if comment_only(line, kind) or ok_line.search(line) or service_line.search(line):
            continue
        if kind == 'cpp':
            # only string literals count; strip trailing // comments first
            code = re.sub(r'//.*$', '', line)
            lits = re.findall(r'"(?:[^"\\]|\\.)*"', code)
            if not any(word.search(l) for l in lits):
                continue
            if any(word.search(l) and re.search(r'^"(Inv_|Firestorm Viewer Support)', l) for l in lits):
                continue
            # settings/control names and notification ids are internal
            if re.search(r'"(FirestormTagColor|FirestormClearSettingsPrompt|FirestormChatColor|Firestorm)"\s*[,)]', code) and \
               re.search(r'getColor|LLNotificationsUtil::add|ViewerFolderEntry', code):
                continue
        if kind in ('xml',) and re.search(r'\b(name|function|parameter|filename|class|help_topic|value|image\w*)\s*=\s*"[^"]*(firestorm|phoenix)', line, re.I) \
           and not re.search(r'\b(label|title|tool_tip|text|ignoretext)\s*=\s*"[^"]*(firestorm|phoenix)', line, re.I):
            continue
        hits.append('%s:%d: %s' % (f, n, line.strip()[:160]))

if hits:
    print('\n'.join(hits))
    print('\n%d possible visible leftovers.' % len(hits))
    sys.exit(1)
print('pat_branding_check: no visible leftovers found.')
PYEOF
