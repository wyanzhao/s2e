#!/bin/bash

# The purpose of this script is to enter the chroot environment

declare -a ext_dir
declare -a int_dir

ext_dir[${#ext_dir[*]}]="/home/$USER/s2e/symdrive/cil"
ext_dir[${#ext_dir[*]}]="/home/$USER/s2e/symdrive/test"
ext_dir[${#ext_dir[*]}]="/home/$USER/s2e/symdrive/gtf"

int_dir[${#int_dir[*]}]="/home/$USER/s2e/symdrive/debian32/root/cil"
int_dir[${#int_dir[*]}]="/home/$USER/s2e/symdrive/debian32/root/test"
int_dir[${#int_dir[*]}]="/home/$USER/s2e/symdrive/debian32/root/gtf"

int_proc="/home/$USER/s2e/symdrive/debian32/proc"

if [ "${#int_dir[*]}" -ne "${#ext_dir[*]}" ]; then
    echo "Counts not equal: ${#int_dir[*]} != ${#ext_dir[*]}"
    exit
fi

if [ $# -eq 0 ]; then
    sudo mount -t proc proc $int_proc
    for i in `seq 1 ${#int_dir[*]}`; do
        index=$((i - 1))
        echo "Executing mount -o bind ${ext_dir[$index]} ${int_dir[$index]}"
        sudo mount -o bind "${ext_dir[$index]}" "${int_dir[$index]}"
    done
    
    sudo setarch linux32 chroot \
        "/home/$USER/s2e/symdrive/debian32" \
        /usr/bin/env -i HOME=/root TERM="$TERM" SHELL="$SHELL" /bin/bash --login +h
    
    echo "Before ..."
    
    mount
    
    echo "Processing ..."
    sudo chmod 555 /home/$USER/s2e/symdrive/debian32/root
    for i in `seq 1 ${#int_dir[*]}`; do
        index=$((i - 1))
        cur_int_dir=${int_dir[$index]}
        cur_ext_dir=${ext_dir[$index]}
        
        echo "Executing lazy umount:"
        echo "sudo umount -l $cur_int_dir"
        sudo umount -l $cur_int_dir
        
        # Reset owner to $USER in all cases
        sudo chown -R $USER:$USER $cur_ext_dir
    done
    sudo umount -l $int_proc

    echo "After ..."
    mount
else
    echo "Executing umount only"
    for i in `seq 1 ${#int_dir[*]}`; do
        index=$((i - 1))
        cur_int_dir=${int_dir[$index]}
        cur_ext_dir=${ext_dir[$index]}
        
        echo "Executing lazy umount:"
        echo "sudo umount -l $cur_int_dir"
        sudo umount -l $cur_int_dir
        
        # Reset owner to $USER in all cases
        sudo chown -R $USER:$USER $cur_ext_dir
    done
    sudo umount -l $int_proc
fi
