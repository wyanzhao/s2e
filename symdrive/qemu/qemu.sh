#!/bin/bash

# Find typos - shows uninitialized variables
bash -n $0
set -u

# Store arguments in ALL_ARGS
ALL_ARGS=("$@")

##############################################################################
# Only supporting linux for now
#TARGETOS="freebsd"
TARGETOS="linux"
##############################################################################
#ARCH=arm
ARCH=i386
##############################################################################
# ubuntus2e
# adsl-q04
HOSTMACHINE=`hostname`
##############################################################################
DRIVER=
SUBDRIVER=

# Don't touch this unless you're really sure
if [ "$ARCH" = 'arm' ]; then
    BINARY="qemu-system-arm"
elif [ "$ARCH" = 'i386' ]; then
    #BINARY="qemu"
    BINARY="qemu-system-i386"
else
    echo "Wrong arch?"
    exit
fi

# Workaround QEMU audio bug
# export QEMU_AUDIO_DRV=alsa

QEMU_S2E_ENABLED_RELEASE=~/s2e/build/qemu-release/$ARCH-s2e-softmmu/$BINARY
QEMU_S2E_DISABLED_RELEASE=~/s2e/build/qemu-release/$ARCH-softmmu/$BINARY
QEMU_S2E_ENABLED_DEBUG=~/s2e/build/qemu-debug/$ARCH-s2e-softmmu/$BINARY
QEMU_S2E_DISABLED_DEBUG=~/s2e/build/qemu-debug/$ARCH-softmmu/$BINARY
QEMU_UNMODIFIED=`which qemu`

S2E_TOOL_VERSION="Release"

QEMU_S2E_ENABLED="$QEMU_S2E_ENABLED_RELEASE"
QEMU_S2E_DISABLED="$QEMU_S2E_DISABLED_RELEASE"
QEMU=""

QEMU_QCOW2_DISK="$ARCH/s2e_disk_$TARGETOS.qcow2"
QEMU_QCOW2_DISK_ERASING="$ARCH/s2e_disk_$TARGETOS.qcow2.erasing"
QEMU_QCOW2_BACKUP="$ARCH/s2e_disk_$TARGETOS.qcow2.backup"
QEMU_QCOW2_BACKUPCOPY_FLAG="$ARCH/s2e_disk_$TARGETOS.backup_copy.ready"
QEMU_QCOW2_BACKUPCOPY="$ARCH/s2e_disk_$TARGETOS.backup_copy"
##############################################################################
# Specify this on the command line now.
if [ "$ARCH" = 'arm' ]; then
    QEMU_NIC_MODEL=""
    QEMU_SOUNDHW=""
elif [ "$ARCH" = 'i386' ]; then
    # Options:  ne2k_pci,i82551,i82557b,i82559er,rtl8139,e1000,pcnet,virtio
    #
    QEMU_NIC_MODEL1="-net nic,model=ne2k_pci,macaddr=52:54:00:00:00:01" # USE ONLY WITH E1000
    QEMU_NIC_MODEL2="-net nic,model=rtl8139,macaddr=52:54:00:00:00:02" # This one does not work well
    QEMU_NIC_MODEL3="-net nic,model=e1000,macaddr=52:54:00:00:00:03" # Works OK
    QEMU_NIC_MODEL4="-net nic,model=pcnet,macaddr=52:54:00:00:00:04" # Does not work in FreeBSD
    QEMU_NIC_MODEL5="-net nic,model=i82551,macaddr=52:54:00:00:00:05" # Works w/ FreeBSD but NO IP address
    QEMU_NIC_MODEL6="-net nic,model=i82557b,macaddr=52:54:00:00:00:06" # Works OK.

    QEMU_SOUNDHW0=""
    QEMU_SOUNDHW1="-soundhw es1370"
    QEMU_SOUNDHW2="-soundhw pcspk"
    QEMU_SOUNDHW3="-soundhw sb16"
    QEMU_SOUNDHW4="-soundhw ac97"

    QEMU_NIC_MODEL=$QEMU_NIC_MODEL3  # Use otherwise
    QEMU_SOUNDHW=$QEMU_SOUNDHW0
fi
##############################################################################
# Choose one
QEMU_ENABLE_SERIAL="-serial pty"
#QEMU_ENABLE_SERIAL=""
##############################################################################
echo "Unknown user - setting defaults"
QEMU_PORT=20022
TELNET_PORT=4444
GDB_PORT=5022
QEMU_VNC_ENABLED="-display vnc=0.0.0.0:2 "
##############################################################################
QEMU_VNC_DISABLED=""
QEMU_VNC_FLAG="$QEMU_VNC_ENABLED"
QEMU_BASIC_FLAGS="-hda $QEMU_QCOW2_DISK \
        -m 256 \
        -redir tcp:$QEMU_PORT::22 \
        -rtc clock=vm \
        -net user \
        -monitor telnet:localhost:$TELNET_PORT,server,nowait \
        $QEMU_ENABLE_SERIAL"
##############################################################################
# Whether to suspend boot process and wait for GDB before
# continuing.  Useful for debugging bootup.
#QEMU_GDB_WAIT="-S"
QEMU_GDB_WAIT=""
##############################################################################
# Don't change this unless you're sure
# State that's updated depending on options selected
QEMU_VANILLA_FLAGS=""
QEMU_S2E_FLAGS=""
QEMU_GUEST_GDB_FLAGS=""
QEMU_VMM_GDB_FLAGS=""
QEMU_SUDO=""
QEMU_MAX_PROCESSES=1 # Parallelized S2E
##############################################################################

remove_temporary_files()
{
    echo "Removing s2e-out-*"
    rm -rf ./s2e-out-*
    rm -f ./s2e-last
    rm -f ./messages.txt
    rm -f nohup.out
}

print_status()
{
    echo "========================================================="
    echo "DRIVER: $DRIVER"
    echo "SUBDRIVER: $SUBDRIVER"
    echo "TARGETOS: $TARGETOS"
    echo "QEMU_QCOW2_DISK: $QEMU_QCOW2_DISK"
    echo "QEMU_QCOW2_BACKUP: $QEMU_QCOW2_BACKUP"
    echo "QEMU_QCOW2_BACKUPCOPY_FLAG: $QEMU_QCOW2_BACKUPCOPY_FLAG"
    echo "QEMU_QCOW2_BACKUPCOPY: $QEMU_QCOW2_BACKUPCOPY"
    echo "========================================================="
    echo "Guest GDB status: $QEMU_GUEST_GDB_FLAGS"
    echo "VMM GDB status: $QEMU_VMM_GDB_FLAGS"
    echo "QEMU Enabled: $QEMU_S2E_ENABLED"
    echo "QEMU Disabled: $QEMU_S2E_DISABLED"
    echo "QEMU VNC: $QEMU_VNC_FLAG"
    echo "QEMU Max processes: $QEMU_MAX_PROCESSES"
    echo "========================================================="
}

qemu_execute()
{
    print_status
    echo "Executing:"
    echo -n "$QEMU_SUDO $QEMU_VMM_GDB_FLAGS $QEMU $QEMU_GUEST_GDB_FLAGS"
    echo -n " $QEMU_BASIC_FLAGS $QEMU_VANILLA_FLAGS $QEMU_S2E_FLAGS"
    echo " $QEMU_NIC_MODEL $QEMU_SOUNDHW $QEMU_VNC_FLAG"
    echo ""
    $QEMU_SUDO $QEMU_VMM_GDB_FLAGS $QEMU $QEMU_GUEST_GDB_FLAGS \
        $QEMU_BASIC_FLAGS $QEMU_VANILLA_FLAGS $QEMU_S2E_FLAGS \
        $QEMU_NIC_MODEL $QEMU_SOUNDHW $QEMU_VNC_FLAG
}

# These two functions are complements
# Be sure they use only the flags of interest
# We intentionally erase all other flags
# since this allows more flexibility in the
# way menu options set up the flags.
qemu_s2e_disabled()
{
    QEMU="$QEMU_S2E_DISABLED"
    QEMU_S2E_FLAGS=""
    qemu_execute;
}

qemu_s2e_enabled()
{
    QEMU="$QEMU_S2E_ENABLED"
    # DRIVER is not null
    QEMU_VANILLA_FLAGS=""
    if [ -n "$SUBDRIVER" ]; then
        QEMU_S2E_FLAGS+="-s2e-config-file ./configs-$TARGETOS/config_${DRIVER}_${SUBDRIVER}.lua "
    else
        QEMU_S2E_FLAGS+="-s2e-config-file ./configs-$TARGETOS/config_$DRIVER.lua "
    fi
    QEMU_S2E_FLAGS+="-loadvm 1 -s2e-verbose "
    QEMU_S2E_FLAGS+="-s2e-max-processes $QEMU_MAX_PROCESSES "
    QEMU_S2E_FLAGS+="-nographic "
    qemu_execute;
}

qemu_vanilla()
{
    QEMU="$QEMU_UNMODIFIED"
    qemu_execute;
}

print_choicelist()
{
    declare -a choicelist
    choicelist[${#choicelist[*]}]="10 - qemu s2e disabled"
    choicelist[${#choicelist[*]}]="11 - qemu s2e enabled"
    choicelist[${#choicelist[*]}]="17 - qemu vanilla"
    choicelist[${#choicelist[*]}]="18 - no driver, s2e binary vanilla"
    choicelist[${#choicelist[*]}]="19 - driver test_pci"
    choicelist[${#choicelist[*]}]="20 - driver pcnet32"
    choicelist[${#choicelist[*]}]="21 - driver e1000"
    choicelist[${#choicelist[*]}]="22a - driver 8139too/if_rl (8129)"
    choicelist[${#choicelist[*]}]="22b - driver 8139too/if_rl (8139)"
    choicelist[${#choicelist[*]}]="23 - driver 8139cp/if_re"
    choicelist[${#choicelist[*]}]="24 - driver be2net"
    choicelist[${#choicelist[*]}]="25a - driver ens1371 (1371)"
    choicelist[${#choicelist[*]}]="25b - driver ens1371 (1370)"
    choicelist[${#choicelist[*]}]="26 - driver me4000"
    choicelist[${#choicelist[*]}]="27 - driver es1938"
    choicelist[${#choicelist[*]}]="28 - driver dl2k"
    choicelist[${#choicelist[*]}]="29 - driver pluto2_i2c"
    choicelist[${#choicelist[*]}]="30 - driver forcedeth"
    choicelist[${#choicelist[*]}]="31 - driver et131x"
    choicelist[${#choicelist[*]}]="32 - driver phantom"
    choicelist[${#choicelist[*]}]="33 - driver econet"
    choicelist[${#choicelist[*]}]="34 - driver android_pca963x"
    choicelist[${#choicelist[*]}]="35 - driver android_wl127x"
    choicelist[${#choicelist[*]}]="36 - driver apds9802als"
    choicelist[${#choicelist[*]}]="37 - driver android_smc91x"
    choicelist[${#choicelist[*]}]="38 - driver android_mmc31xx"
    choicelist[${#choicelist[*]}]="39 - driver android_akm8975"
    choicelist[${#choicelist[*]}]="40 - driver ks8851"
    choicelist[${#choicelist[*]}]="41 - driver tle62x0"
    choicelist[${#choicelist[*]}]="42 - driver android_a1026"
    choicelist[${#choicelist[*]}]="43 - driver android_cyttsp"
    choicelist[${#choicelist[*]}]="44 - driver lp5523"
    choicelist[${#choicelist[*]}]="45 - driver hostap"
    choicelist[${#choicelist[*]}]="46 - driver orinoco"
    choicelist[${#choicelist[*]}]="47a - driver r8169/if_re (8129)"
    choicelist[${#choicelist[*]}]="47b - driver r8169/if_re (8169)"
    choicelist[${#choicelist[*]}]="48 - driver e100"
    choicelist[${#choicelist[*]}]="49 - driver ne2k_pci/if_ed"
    choicelist[${#choicelist[*]}]="50 - driver es1968/maestro"
    choicelist[${#choicelist[*]}]="51 - driver tg3/if_bge"
    choicelist[${#choicelist[*]}]="52 - driver dor"
    choicelist[${#choicelist[*]}]="71 - 1 QEMU process"
    choicelist[${#choicelist[*]}]="72 - 2 QEMU processes [not supported]"
    choicelist[${#choicelist[*]}]="73 - 3 QEMU processes [not supported]"
    choicelist[${#choicelist[*]}]="74 - 4 QEMU processes [not supported]"
    choicelist[${#choicelist[*]}]="80 - qemu s2e release [default]"
    choicelist[${#choicelist[*]}]="81 - qemu s2e debug"
    choicelist[${#choicelist[*]}]="82 - use release tool build [default]"
    choicelist[${#choicelist[*]}]="83 - use debug tool build"
    choicelist[${#choicelist[*]}]="90 - enable guest gdb"
    choicelist[${#choicelist[*]}]="91 - disable guest gdb [default]"
    choicelist[${#choicelist[*]}]="92 - enable vmm gdb"
    choicelist[${#choicelist[*]}]="93 - disable vmm gdb [default]"
    choicelist[${#choicelist[*]}]="97 - enable VNC [default]"
    choicelist[${#choicelist[*]}]="98 - disable VNC"
    choicelist[${#choicelist[*]}]="99 - guest gdb start"
    choicelist[${#choicelist[*]}]="101 - remove temporary files"
    choicelist[${#choicelist[*]}]="1000 - telnet basics"
    choicelist[${#choicelist[*]}]="1001 - copy to qemu"
    choicelist[${#choicelist[*]}]="1002 - copy from qemu"
    choicelist[${#choicelist[*]}]="1003 - save vm"
    choicelist[${#choicelist[*]}]="3001 - $QEMU_NIC_MODEL1 [use when testing e1000]"
    choicelist[${#choicelist[*]}]="3002 - $QEMU_NIC_MODEL2"
    choicelist[${#choicelist[*]}]="3003 - $QEMU_NIC_MODEL3 [default]"
    choicelist[${#choicelist[*]}]="3004 - $QEMU_NIC_MODEL4"
    choicelist[${#choicelist[*]}]="3005 - $QEMU_NIC_MODEL5"
    choicelist[${#choicelist[*]}]="3006 - $QEMU_NIC_MODEL6"
    choicelist[${#choicelist[*]}]="4982 - backup to qcow2 [CAUTION]"
    choicelist[${#choicelist[*]}]="14159265358979323846 - qcow2 to backup [CAUTION]"

    echo "Command options: "
    for i in "${choicelist[@]}"
    do
        echo $i
    done

    print_status
}

read_reply()
{
    echo "Press CTRL-C to stop"
    read -p "Command? " REPLY

    # first, strip underscores
    #REPLY=${REPLY//_/}
    # next, replace spaces with underscores
    #REPLY=${REPLY// /_}
    # now, clean out anything that's not numeric
    REPLY=${REPLY//[^0-9]/}
    # finally, lowercase with TR
    # REPLY=`echo -n $REPLY | tr A-Z a-z`
    if [ -z "$REPLY" ]; then
        REPLY=0
    fi

    echo "Command: $REPLY"
    echo "========================================================="
}

copy_to_qemu()
{
    #MORE_FLAGS=-a --delete

    echo "Default user -- using defaults?"
    QEMU_PORT=20022

    if [ "$TARGETOS" = "linux" ]; then
        echo "Copying files from our machine to QEMU"

        # mjr is the user inside the S2E VM
        # If you don't like that then you can change it here
        rsync -e "ssh -p $QEMU_PORT" -vc --copy-links \
            --include '*/' --include '*.ko' --include '*.sh' --include 's2e_*' \
            --include 'app_ep_control' --include 'econet_test' --include '*.test' \
            --include '*.wav' --include '*.mid' \
            --exclude '*' \
            ../debian32/root/test/* mjr@localhost:/home/mjr &

        #rsync -e "ssh -p $QEMU_PORT" -rvc --copy-links \
        #    ../debian32/root/test/me4000/test/examples mjr@localhost:/home/mjr &

        # scp -P $QEMU_PORT ../debian32/root/test/me4000/test/rc.me4000 mjr@localhost:/home/mjr &
    elif [ "$TARGETOS" = "freebsd" ]; then
        echo "Not supporting FreeBSD in here at present"
        exit
    else
        echo "Error"
    fi

    wait
}

save_qemu()
{
    echo -e "savevm 1\nquit" | ./telnet.sh
}

run_basics()
{
    # Login works with FreeBSD and Linux
    ./sendkey.py root | ./telnet.sh
    sleep 2
    ./sendkey.py rootme | ./telnet.sh
    sleep 0.5
    ./sendkey.py home | ./telnet.sh

    echo "Press enter to copy files.  Press CTRL-C if login failed, and re-run."
    read
    copy_to_qemu;

    echo "Press enter to save the VM.  Press CTRL-C if the VM isn't set up properly yet."
    read
    save_qemu;
}

# Works with FreeBSD and Linux
copy_from_qemu()
{
    echo "Default user -- using defaults"
    QEMU_PORT=20022

    if [ "$TARGETOS" = "linux" ]; then
        scp -P $QEMU_PORT root@localhost:/var/log/messages ./messages.txt
    elif [ "$TARGETOS" = "freebsd" ]; then
        echo "Not supporting FreeBSD here at the moment"
    else
        echo "Error"
    fi
}

choose_driver()
{
    # Driver is more important and should match driver name
    LINUX_DRIVER=$1
    FREEBSD_DRIVER=$2

    # Subdriver is only relevant in context of the lua files
    LINUX_SUBDRIVER=$3
    FREEBSD_SUBDRIVER=$4
    if [ "$TARGETOS" = "linux" ]; then
        DRIVER="$LINUX_DRIVER"
        SUBDRIVER="$LINUX_SUBDRIVER"
    elif [ "$TARGETOS" = "freebsd" ]; then
        DRIVER="$FREEBSD_DRIVER"
        SUBDRIVER="$FREEBSD_SUBDRIVER"
    else
        echo "FreeBSD or Linux"
        exit 1
    fi
}

execute_option()
{
    if [ "$REPLY" = "10" ]; then
        qemu_s2e_disabled;
        $BREAK;
    elif [ "$REPLY" = "11" ]; then
        qemu_s2e_enabled;
        $BREAK;
    elif [ "$REPLY" = "17" ]; then
        # Completely vanilla
        # Install:
        # qemu s2e_disk.qcow2 -cdrom debian-6.0.1a-i386-businesscard.iso -vnc :2
        # Execute:
        QEMU_SUDO=sudo
        qemu_vanilla;
        $BREAK;
    elif [ "$REPLY" = "18" ]; then
        # Vanilla with S2E binary
        DRIVER="vanilla"
        QEMU_VANILLA_FLAGS=""
    elif [ "$REPLY" = "19" ]; then
        DRIVER="test_pci"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100 \
            -fake-pci-resource-io 0x100"
    elif [ "$REPLY" = "20" ]; then
        DRIVER="pcnet32"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x1022 \
            -fake-pci-device-id 0x2000 \
            -fake-pci-class-code 2 \
            -fake-pci-revision-id 0x7 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x1000 \
            -fake-pci-resource-mem 0x1000"
    elif [ "$REPLY" = "21" ]; then
        DRIVER="e1000"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x8086 \
            -fake-pci-device-id 0x100E \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x2 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x8000"
    elif [ "$REPLY" = "22a" ]; then
        choose_driver "8139too" "if_rl" "8129" "8129"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x10ec \
            -fake-pci-device-id 0x8129 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x100 \
            -fake-pci-resource-mem 0x100"
        $BREAK;
    elif [ "$REPLY" = "22b" ]; then
        choose_driver "8139too" "if_rl" "8139" "8139"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x10ec \
            -fake-pci-device-id 0x8139 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x100 \
            -fake-pci-resource-mem 0x100"
        $BREAK;
    elif [ "$REPLY" = "23" ]; then
        choose_driver "8139cp" "if_re" "" ""
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x10ec \
            -fake-pci-device-id 0x8139 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x20 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x200 \
            -fake-pci-resource-mem 0x200"
    elif [ "$REPLY" = "24" ]; then
        DRIVER="be2net"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x19a2 \
            -fake-pci-device-id 0x0211 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x1000 \
            -fake-pci-resource-mem 0x1000 \
            -fake-pci-resource-mem 0x1000 \
            -fake-pci-resource-mem 0x1000 \
            -fake-pci-resource-mem 0x20000"
    elif [ "$REPLY" = "25a" ]; then
        choose_driver "ens1371" "es137x" "" "1371"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x1274 \
            -fake-pci-device-id 0x1371 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x1000"
    elif [ "$REPLY" = "25b" ]; then
        choose_driver "ens1371" "es137x" "" "1370"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x1274 \
            -fake-pci-device-id 0x5000 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x1000"
    elif [ "$REPLY" = "26" ]; then
        DRIVER="me4000"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x1402 \
            -fake-pci-device-id 0x4683 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-io 0x200"
    elif [ "$REPLY" = "27" ]; then
        DRIVER="es1938"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x125d \
            -fake-pci-device-id 0x1969 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-io 0x200"
    elif [ "$REPLY" = "28" ]; then
        DRIVER="dl2k"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x1186 \
            -fake-pci-device-id 0x4000 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x200 \
            -fake-pci-resource-mem 0x200"
    elif [ "$REPLY" = "29" ]; then
        DRIVER="pluto2_i2c"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x0432 \
            -fake-pci-device-id 0x0001 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x40 \
            -fake-pci-resource-mem 0x200"
    elif [ "$REPLY" = "30" ]; then
        DRIVER="forcedeth"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x10de \
            -fake-pci-device-id 0x01c3 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x1000"
    elif [ "$REPLY" = "31" ]; then
        DRIVER="et131x"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x11C1 \
            -fake-pci-device-id 0xED00 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x8000"
    elif [ "$REPLY" = "32" ]; then
        DRIVER="phantom"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x10b5 \
            -fake-pci-device-id 0x9050 \
            -fake-pci-ss-vendor-id 0x10b5 \
            -fake-pci-ss-id 0x9050 \
            -fake-pci-class-code 0x0680 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x1000 \
            -fake-pci-resource-mem 0x1000 \
            -fake-pci-resource-mem 0x1000 \
            -fake-pci-resource-mem 0x1000"
    elif [ "$REPLY" = "33" ]; then
        DRIVER="econet"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100 \
            -fake-pci-resource-io 0x100"
    elif [ "$REPLY" = "34" ]; then
        DRIVER="android_pca963x"
    elif [ "$REPLY" = "35" ]; then
        DRIVER="android_wl127x"
    elif [ "$REPLY" = "36" ]; then
        DRIVER="apds9802als"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000"
    elif [ "$REPLY" = "37" ]; then
        DRIVER="android_smc91x"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000"
    elif [ "$REPLY" = "38" ]; then
        DRIVER="android_mmc31xx"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000"
    elif [ "$REPLY" = "39" ]; then
        DRIVER="android_akm8975"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000"
    elif [ "$REPLY" = "40" ]; then
        DRIVER="ks8851"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000"
    elif [ "$REPLY" = "41" ]; then
        DRIVER="tle62x0"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000"
    elif [ "$REPLY" = "42" ]; then
        DRIVER="android_a1026"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000"
    elif [ "$REPLY" = "43" ]; then
        DRIVER="android_cyttsp"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000"
    elif [ "$REPLY" = "44" ]; then
        DRIVER="lp5523"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x9892 \
            -fake-pci-device-id 0x9893 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000 \
            -fake-pci-resource-mem 0x100000"
    elif [ "$REPLY" = "45" ]; then
        DRIVER="hostap"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x167d \
            -fake-pci-device-id 0xa000 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x1000"
    elif [ "$REPLY" = "46" ]; then
        DRIVER="orinoco"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x167d \
            -fake-pci-device-id 0xa000 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x1000"
    elif [ "$REPLY" = "47a" ]; then
        choose_driver "r8169" "if_re" "8129" "8129"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x10ec \
            -fake-pci-device-id 0x8129 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x100 \
            -fake-pci-resource-mem 0x100"
    elif [ "$REPLY" = "47b" ]; then
        choose_driver "r8169" "if_re" "8169" "8169"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x10ec \
            -fake-pci-device-id 0x8169 \
            -fake-pci-class-code 0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x1 \
            -fake-pci-cap-msi 0x1 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x100 \
            -fake-pci-resource-mem 0x100 \
            -fake-pci-resource-mem 0x100"
    elif [ "$REPLY" = "48" ]; then
        choose_driver "e100" "if_fxp" "" ""
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x8086 \
            -fake-pci-device-id 0x1229 \
            -fake-pci-class-code 0x200 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-mem 0x1000"
    elif [ "$REPLY" = "49" ]; then
        choose_driver "ne2k_pci" "if_ed" "" ""
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x10ec \
            -fake-pci-device-id 0x8029 \
            -fake-pci-class-code 0x0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x1000"
    elif [ "$REPLY" = "50" ]; then
        choose_driver "es1968" "maestro" "" ""
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x125d \
            -fake-pci-device-id 0x1978 \
            -fake-pci-class-code 0x400 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x0 \
            -fake-pci-cap-msi 0x0 \
            -fake-pci-cap-pcie 0x0 \
            -fake-pci-resource-io 0x1000"
    elif [ "$REPLY" = "51" ]; then
        choose_driver "tg3" "if_bge" "" ""
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x14E4 \
            -fake-pci-device-id 0x1648 \
            -fake-pci-class-code 0x0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-cap-pm 0x1 \
            -fake-pci-cap-msi 0x1 \
            -fake-pci-cap-pcie 0x1 \
            -fake-pci-resource-mem 0x20000 \
            -fake-pci-resource-mem 0x20000 \
            -fake-pci-resource-mem 0x20000"
    elif [ "$REPLY" = "52" ]; then
        DRIVER="dor"
        QEMU_VANILLA_FLAGS="-fake-pci-name ${DRIVER}f \
            -fake-pci-vendor-id 0x1234 \
            -fake-pci-device-id 0x5678 \
            -fake-pci-class-code 0x0 \
            -fake-pci-revision-id 0x0 \
            -fake-pci-resource-io 0x1000"
    elif [ "$REPLY" = "71" ]; then
        QEMU_MAX_PROCESSES=1
        $BREAK;
    elif [ "$REPLY" = "72" ]; then
        QEMU_MAX_PROCESSES=2
        $BREAK;
    elif [ "$REPLY" = "73" ]; then
        QEMU_MAX_PROCESSES=3
        $BREAK;
    elif [ "$REPLY" = "74" ]; then
        QEMU_MAX_PROCESSES=4
        $BREAK;
    elif [ "$REPLY" = "80" ]; then
        echo "Selecting Release QEMU ..."
        QEMU_S2E_ENABLED="$QEMU_S2E_ENABLED_RELEASE"
        QEMU_S2E_DISABLED="$QEMU_S2E_DISABLED_RELEASE"
    elif [ "$REPLY" = "81" ]; then
        echo "Selecting Debug QEMU ..."
        QEMU_S2E_ENABLED="$QEMU_S2E_ENABLED_DEBUG"
        QEMU_S2E_DISABLED="$QEMU_S2E_DISABLED_DEBUG"
    elif [ "$REPLY" = "82" ]; then
        echo "Use Release tool build ..."
        S2E_TOOL_VERSION="Release"
    elif [ "$REPLY" = "83" ]; then
        echo "Use Debug tool build ..."
        S2E_TOOL_VERSION="Debug"
    elif [ "$REPLY" = "90" ]; then
        echo "Not suppoorted"
    elif [ "$REPLY" = "91" ]; then
        echo "Disabling QEMU guest OS GDB support ..."
        QEMU_GUEST_GDB_FLAGS=""
    elif [ "$REPLY" = "92" ]; then
        # Enable GDB
        echo "Enabling QEMU VMM GDB support ..."
        QEMU_VMM_GDB_FLAGS="gdb --command=../test/gdb/gdb_init2 --args "
    elif [ "$REPLY" = "93" ]; then
        echo "Disabling QEMU VMM GDB support ..."
        QEMU_VMM_GDB_FLAGS=""
    elif [ "$REPLY" = "97" ]; then
        echo "Enabling VNC support ..."
        QEMU_VNC_FLAG="$QEMU_VNC_ENABLED"
    elif [ "$REPLY" = "98" ]; then
        echo "Disabling VNC support ..."
        QEMU_VNC_FLAG="$QEMU_VNC_DISABLED"
    elif [ "$REPLY" = "99" ]; then
        cd ../test
        ./gdb.sh
        exit
    elif [ "$REPLY" = "101" ]; then
        remove_temporary_files;
    elif [ "$REPLY" = "1000" ]; then
        run_basics;
    elif [ "$REPLY" = "1001" ]; then
        copy_to_qemu;
    elif [ "$REPLY" = "1002" ]; then
        copy_from_qemu;
    elif [ "$REPLY" = "1003" ]; then
        save_qemu;
    elif [ "$REPLY" = "3001" ]; then
        QEMU_NIC_MODEL=$QEMU_NIC_MODEL1
    elif [ "$REPLY" = "3002" ]; then
        QEMU_NIC_MODEL=$QEMU_NIC_MODEL2
    elif [ "$REPLY" = "3003" ]; then
        QEMU_NIC_MODEL=$QEMU_NIC_MODEL3
    elif [ "$REPLY" = "3004" ]; then
        QEMU_NIC_MODEL=$QEMU_NIC_MODEL4
    elif [ "$REPLY" = "3005" ]; then
        QEMU_NIC_MODEL=$QEMU_NIC_MODEL5
    elif [ "$REPLY" = "3006" ]; then
        QEMU_NIC_MODEL=$QEMU_NIC_MODEL6
    elif [ "$REPLY" = "4982" ]; then
        # This code is all rather complicated.  The idea is to
        # copy the S2E disk image in the background without
        # impacting system performance.  I've found this work completely
        # reliably on my machine.  However, if you're in a multi-user
        # environment, e.g. many people using SymDrive on the same
        # machine, this may break.  You can just use a straightforward
        # cp command if you want but the disk will be busy for a while.
        # Maybe I need an SSD.

        # Variable checks to see if rsync is running.
        APPCHK=$(ps aux | grep -v "grep" | grep -c "rsync")
        if [ "$APPCHK" -eq "0" ]; then
            if [ -e "$QEMU_QCOW2_BACKUPCOPY_FLAG" ]; then
                echo "Moving backup copy $QEMU_QCOW2_BACKUPCOPY to $QEMU_QCOW2_DISK..."
                mv $QEMU_QCOW2_DISK $QEMU_QCOW2_DISK_ERASING
                rm $QEMU_QCOW2_DISK_ERASING &
                mv $QEMU_QCOW2_BACKUPCOPY $QEMU_QCOW2_DISK
                rm $QEMU_QCOW2_BACKUPCOPY_FLAG
            else
                echo "Copying $QEMU_QCOW2_BACKUP to $QEMU_QCOW2_DISK"
                nice -n 20 ionice -c 3 cp $QEMU_QCOW2_BACKUP $QEMU_QCOW2_DISK
            fi

            echo "Starting $QEMU_QCOW2_BACKUP copy to $QEMU_QCOW2_BACKUPCOPY in background..."
            setsid nohup ./backup_qemu.sh $QEMU_QCOW2_BACKUP $QEMU_QCOW2_BACKUPCOPY $QEMU_QCOW2_BACKUPCOPY_FLAG &
            # Don't break, so we can restore and restart all in one command
        else
            echo "Copying $QEMU_QCOW2_BACKUP to $QEMU_QCOW2_DISK"
            nice -n 20 ionice -c 3 cp $QEMU_QCOW2_BACKUP $QEMU_QCOW2_DISK
        fi
    elif [ "$REPLY" = "14159265358979323846" ]; then
        # Variable checks to see if rsync is running.
        APPCHK=$(ps aux | grep -v "grep" | grep -c "rsync")
        if [ "$APPCHK" -eq "0" ]; then
            echo "Overwriting main image and removing backup copies if present..."
            nice -n 20 ionice -c 3 cp $QEMU_QCOW2_DISK $QEMU_QCOW2_BACKUP
            rm $QEMU_QCOW2_BACKUPCOPY_FLAG
            rm $QEMU_QCOW2_BACKUPCOPY
        else
            ps ux | grep -v "grep" | grep "rsync"
            echo "Rsync is running.  Terminate or wait until it's done."
        fi
        $BREAK;
    else
        echo "Invalid option specified: $REPLY"
        exit
    fi
}

declare -a choicelist

if [ "${#ALL_ARGS[@]}" -gt "0" ]; then
    echo "Additional command line parameters: $*"
    while [ "${#ALL_ARGS[@]}" -gt "0" ]; do
        echo "Num remaining: ${#ALL_ARGS[@]}"
        REPLY=${ALL_ARGS[0]}
        BREAK=

        unset ALL_ARGS[0]
        if [ "${#ALL_ARGS[@]}" -gt "0" ]; then
            ALL_ARGS=("${ALL_ARGS[@]}") # pack array
        fi

        execute_option;
    done
else
    echo "No additional command line params defined."
    print_choicelist;
    exit;
#    while [ 1 ]; do
#        print_choicelist;
#        BREAK=break
#        execute_option;
#
#        wait
#    done
fi
