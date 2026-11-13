#!/bin/bash

SCRIPTSRC=`readlink -f "$0" || echo "$0"`
RUN_PATH=`dirname "${SCRIPTSRC}" || echo .`

install_prefix=$(readlink -f "${RUN_PATH}/..") # <PAT:Installer/> absolute path for Exec=

# <PAT:Rebrand> Install the icon into the hicolor theme (Icon=patalania)
function install_icons()
{
    local src_icon="$1/firestorm_icon.png"
    local icons_base="$2"

    if [ ! -f "$src_icon" ]; then
        echo " - Warning: icon file not found, skipping icon installation"
        return
    fi

    echo " - Installing icons in ${icons_base}"
    mkdir -p "${icons_base}/512x512/apps"
    cp "$src_icon" "${icons_base}/512x512/apps/patalania.png"

    for size in 256 128 64 48 32 16; do
        mkdir -p "${icons_base}/${size}x${size}/apps"
        if command -v convert &>/dev/null; then
            convert "$src_icon" -resize "${size}x${size}" "${icons_base}/${size}x${size}/apps/patalania.png" 2>/dev/null
        elif command -v python3 &>/dev/null && python3 -c "from PIL import Image" &>/dev/null 2>&1; then
            python3 -c "
import sys
from PIL import Image
Image.open(sys.argv[1]).resize((int(sys.argv[3]), int(sys.argv[3])), Image.LANCZOS).save(sys.argv[2])
" "$src_icon" "${icons_base}/${size}x${size}/apps/patalania.png" "$size"
        fi
    done

    gtk-update-icon-cache -f -t "${icons_base}" 2>/dev/null || true
}
# </PAT>

function install_desktop_entry()
{
    local installation_prefix="$1"
    local desktop_entries_dir="$2"

    local desktop_entry="\
[Desktop Entry]\n\
Name=Patalania\n\
Comment=Client for accessing 3D virtual worlds, with VR support\n\
Exec=\"${installation_prefix}/patalania\"\n\
Icon=patalania\n\
Terminal=false\n\
Type=Application\n\
Categories=Network;\n\
StartupNotify=true\n\
X-Desktop-File-Install-Version=3.0\n\
StartupWMClass=do-not-directly-run-patalania-bin"

    echo " - Installing menu entries in ${desktop_entries_dir}"
    mkdir -vp "${desktop_entries_dir}"
    echo -e "$desktop_entry" > "${desktop_entries_dir}/patalania.desktop" || "Failed to install application menu!"
}

if [ "$UID" == "0" ]; then
    # system-wide
    install_icons "$install_prefix" /usr/local/share/icons/hicolor
    install_desktop_entry "$install_prefix" /usr/local/share/applications
else
    # user-specific
    install_icons "$install_prefix" "$HOME/.local/share/icons/hicolor"
    install_desktop_entry "$install_prefix" "$HOME/.local/share/applications"
fi
