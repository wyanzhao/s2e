# SymDrive

## Setting up the Environment and S2E

In our SymDrive setup, we use Ubuntu 12.04 x64, and the instructions here assume you've already managed to compile S2E. Once you can compile S2E, these instructions should work.

SymDrive uses a specific version of S2E. We would like to integrate as much as possible of SymDrive with S2E, but time constraints currently preclude this approach. In addition, SymDrive currently breaks some existing S2E functionality, which we would need to fix before being able to merge it. We hope to fix these problems in the future.

1. Install git using __sudo apt-get install git__
1. __cd ~__
1. Check out S2E using __git clone https://github.com/wyz7155/SymDrive.git__
1. Once the clone is finished, be sure you've installed all the packages listed on the S2E site as necessary to build S2E.

    __$ sudo apt-get install build-essential subversion gettext liblua5.1-0-dev libsdl1.2-dev libsigc++-2.0-dev binutils-dev python-docutils python-pygments nasm__

    __$ sudo apt-get build-dep llvm-2.7 qemu__

1. __(This step has some problme)__

   __mkdir ~/s2e/build__

   __cd ~s2e/build__

   __ln -s ../s2e/Makefile__

   __make --jobs=1__

Compile S2E with the integrated SymDrive patch per the instructions on their site. Note that to improve reliability, it may be worth using make JOBS=1; however, I've always had success with make JOBS=10 if I run it repeatedly. The Makefile seems to have a bug or two, so you may need to run "make" up to ~5 times before it completes successfully. It should not be necessary to change any source code.

Now, __if compilation fails__, first try running make several times to ensure that the problem isn't a Makefile dependency issue. If make consistently fails, and you're sure you can compile S2E without the SymDrive patch, then contact me.

## The chroot jail

The S2E website also includes information of setting up a 32-bit chroot jail. The purpose of this step is to simplify compilation of 32-bit kernel binaries on what is otherwise a 64-bit OS. SymDrive and S2E both work only with 32-bit guest operating systems, so to compile a driver for testing with SymDrive, the jail is necessary.

1. __sudo apt-get install debootstrap__.
1. Create the directory with the chroot environment: __mkdir ~/s2e/symdrive/debian32__.
1. Create the basic chroot environment. Pay attention to --arch __i386!__ It is crucial for correct compilation.
    1. __cd ~/s2e/symdrive/__
    1. __sudo debootstrap --arch i386 squeeze ./debian32 http://archive.debian.org/debian__

1. Execute __./debian32.sh__ from within the __~/s2e/symdrive/qemu__ directory. It will complain that some mount points are not available--these errors don't matter at this point.

1. From within the jail, execute __cd root__, and then __mkdir cil test gtf__. The eventual approach we use is to make SymDrive code available inside and outside the jail simultaneously. These directories will serve as mount points for the code.

1. Install build tools and developer's libraries for ncurses: __apt-get install build-essential kernel-package locales libncurses-dev__.

At this point, your jail should contain several directories: __/root, /root/cil, /root/test, /root/gtf__. You can exit the jail by logging out (CTRL-D), at which point the debian32.sh script will attempt to unmount some directories, and then return you to the normal command prompt.

## Building the Linux 3.1.1 Kernel

## Setting up SymGen

The steps are as follows:

1. First, download the code, here: [cil.tbz](http://research.cs.wisc.edu/sonar/projects/symdrive/dist/cil.tbz). SymGen is based on CIL and includes a complete copy of that tool.

1. Decompress SymGen in to __~/s2e/symdrive/cil__. ___Don't___ decompress it within the jail. To do this, run __tar xvjf ./cil.tbz__ from )__~/s2e/symdrive__, which will create a directory called cil with the SymGen source code in it.

1. SymGen is written using CIL, which uses OCaml. The package linked here includes CIL, but you will need to install the OCaml compiler. __In your chroot jail__, install OCaml using aptitude search ocaml to find the package, and then assuming entries are listed, use __apt-get install ocaml__. You can test that it works using ocamlc --version.

1. You also need autoconf: run __apt-get install autoconf__ from inside the jail.

1. Now, if you've done these steps sequentially, first exit any open jail using CTRL-D. Then re-start the jail by re-running __debian32.sh__. This time, the cil directory should mount automatically. Change your current directory in the jail to __/root/cil__ and see if the files are listed. If they're not, verify that you decompressed them outside the jail into the directory listed previously, and that the debian32.sh script is mounting the directory properly.

1. Now that OCaml is installed, try to compile SymGen. First run __make configure__, then __./configure__ and then __make__.

1. Still in the chroot jail, __add /root/cil/bin to your PATH__ so that you can run SymGen using the cilly command.

1. Verify SymGen works by typing __cilly --help__ and search for the line __--dodrivers Enable device-driver analysis__. If this line is not present, then SymGen or CIL is not set up correctly, and you can contact us for feedback.

## Setting up Drivers

This section discusses how to set up a new driver to test with SymDrive. The idea is to compile the driver first with SymGen, and then copy it into your S2E-enabled virtual machine so that we can run it with SymDrive. The steps for compiling the driver are as follows:

1. Download the test framework and support library from here: [gtf.tbz](http://research.cs.wisc.edu/sonar/projects/symdrive/dist/gtf.tbz). Place it in __~/s2e/symdrive/__ and decompress it. gtf means "Generalized Test Framework" which simply refers to the fact that it compiles/runs on both Linux and FreeBSD. It's the same Test Framework as described in the paper, and contains the support library as well as all the checkers. After this step, you should have the directory __~/s2e/symdrive/gtf__ filled with files from the archive.

1. Download a sample driver directory hierarchy from here: [test.tbz](http://research.cs.wisc.edu/sonar/projects/symdrive/dist/test.tbz). Place it in __~/s2e/symdrive__ and decompress it. "test" file contains a single driver that we've already set up for use with SymDrive, namely the lp5523 Linux driver. This driver controls a chip that operates LEDs on embedded devices. After this step, you should have the directory __~/s2e/symdrive/test__. The __~/s2e/symdrive/test/lp5523__ directory contains a sample driver from Linux.

1. __Start the chroot jail__. You should notice when doing so that all three mount commands complete successfully now.

    * Executing mount -o bind /home/public/s2e/symdrive/cil /home/public/s2e/symdrive/debian32/root/cil

    * Executing mount -o bind /home/public/s2e/symdrive/test /home/public/s2e/symdrive/debian32/root/test

    * Executing mount -o bind /home/public/s2e/symdrive/gtf /home/public/s2e/symdrive/debian32/root/gtf

1. Change into the __/root/test__ directory and ensure that test_framework is a symbolic link pointing to __../gtf__. The reason for this complexity is that gtf supports Linux and FreeBSD whereas the test directory is for Linux only. If you wanted FreeBSD, you could have, e.g. test_freebsd, with corresponding drivers, and then link to the same gtf.

1. Verify that __/root/cil, /root/gtf, /root/test__ all contain the files that you extracted outside the chroot environment. If they don't, the directories are not being mounted in the jail properly and you should examine the debian32.sh script.

1. __(This step has problem)__ Run __./make.sh lp5523__ within __/root/test__. Odds are this will fail for some reason, but if you're lucky and things are set up correctly, then it will succeed. Contact us if it fails and you can't resolve the problem and we can help. The last two lines of output (saved to output.txt) should read:

    __cat lp5523/output.txt | grep undefined__

    __make: [lp5523.ko] Error 1 (ignored)__

    If you're seeing failures here, examine /root/test/lp5523/output.txt to see if that log sheds any light on it. The last line of this file should be "All done" but if things aren't working it may not be. Search the file for "error" and see if it becomes clear that way. Contact us if it's not working and you can't figure it out.

1. To ensure things are working, the next thing to check for after compiling is the test framework. Output for the test framework compilation appears in __/root/test/output.txt__. The test framework is in __/root/test/test_framework_lp5523__, where lp5523 is the name of whatever driver you're testing. The test_framework_lp5523 directory should contain test_framework_lp5523.ko, which is the actual kernel module.

SymDrive compiles a special test framework for each driver, since when testing existing kernel drivers, we found it easiest/most reliable to disable the checks that were failing on a per-driver basis. In the real world, this feature makes less sense, because the developer would usually instead fix the bug, or the checker may be faulty and need tweaking.

If things are working, this is what you should see in /root/test/lp5523:

    * output.txt: Compilation output. Use this to diagnose compile-time or SymGen problems.

    * lp5523.merged.c: An automatically-generated CIL intermediate file that should be identical to the original driver in terms of functionality, but is rewritten into a single "merged" file. This file does not contain instrumentation and is included only for debugging.

    * lp5523.sym.c: The main instrumented driver source file. This file contains the automatically-added SymDrive source annotations such as s2e_loop_before. When SymDrive executes, it occasionally prints out line numbers. These line numbers refer to the lines in this file, which can be confusing.

    * lp5523-stub.ko: Most importantly, there should be a kernel object file. This is the instrumented driver that you'll actually load in the S2E virtual machine. If things are working, you must have a *-stub.ko file.

## Getting the Disk Image

S2E runs a complete VM symbolically, so to test drivers, you'll need a VM image. This section outlines the necessary steps to getting the disk image we use with Linux 3.1.1.

1. __cd ~/s2e/symdrive/qemu__

1. Download the image from: [i386.tbz](http://research.cs.wisc.edu/sonar/projects/symdrive/dist/i386.tbz) into the __~/s2e/symdrive/qeme__. For reference, the username and password to use are __root__ and __rootme__, respectively.

1. From the qemu directory, execute tar __xvjf ./i386.tbz__. Ensure the file s2e_disk_linux.qcow2 is created afterwards

1. __cd ~/s2e/symdrive/qemu__.

1. Execute __./qemu.sh 14159265358979323846__. This command creates a backup of the s2e_disk_linux.qcow2 file. Check the i386 directory afterward to verify the backup is present.

1. At this point, you can use __./qemu.sh 4982__ to replace the current image with the one from the backup at any time. The first time you do, it will take a while. However, this command starts a background rsync process that makes a second backup, so in the future, if you need to replace the current image with the backup, there will be a copy available and the operation is immediate. For full details, please examine the file itself qemu.sh. If you have problems with this feature (./qemu.sh 4982 and ./qemu.sh 14159265358979323846), just don't use it. We've found it completely reliable and always use it, but don't blame us if you have problems with it.

We used the long string 14159265358979323846 to avoid accidentally running this command.

## Running a Driver

Once everything is compiled, the next step is to run it, and maybe find some bugs. In this section, all steps take place outside the virtual chroot environment and we'll assume you already have the __test_framework_drivername.ko__ and the driver-stub.ko object files.

1. It's first necessary to copy the object files to the virtual machine. Using the ./qemu.sh script, run __./qemu.sh 4982 drivernumber 10__, where drivernumber is the value set up in the script for the driver. Run qemu.sh to see a list of all options. In this case, the number for lp5523 is 44.

1. The qemu.sh script is designed to run QEMU from the console. All manual I/O takes place via VNC. If you're running everything with an X-server you may find it easier to disable VNC, in which case you can use the command __./qemu.sh 4982 98 44 10__. You can edit the qemu.sh script if you wish to make this change permanent. We use VNC to view the QEMU VM instead, but either approach works. The QEMU process itself runs the VNC server on port 5902, so to connect, you'll want to use X.X.X.X:5902 in your VNC setup, where X.X.X.X is the address of the machine that you ran the "qemu.sh" command on.

1. Open a new terminal, and use __./debian32.sh__ to start the chroot jail. Leave this terminal open for now.

1. After QEMU boots and you see the login prompt, in a third terminal, execute __./qemu.sh 1000__ from the ~/s2e/symdrive/qemu directory. This command should login to the VM automatically and change to the appropriate directory. It should then copy the necessary files into the VM automatically, and save the VM state. After this command finishes the VM will exit automatically. The password is rootme if you're prompted.

1. __(This step has problem)__ After saving the VM state in a snapshot using the script, restart it with ./qemu.sh drivernumber 11 where drivernumber is the same as in the last step. The purpose of this command is to re-start the VM using the symbolic-execution-enabled S2E runtime. Add 98 to the command if you want to disable VNC, so if you're using lp5523, the modified command line would look like this: __./qemu.sh 98 44 11__ . If you're leaving VNC enabled (the default), then you may need to reconnect your VNC client.

1. At this point, you should have a running VM again -- it should have resumed the one you saved previously. To test the driver, execute the following commands:
    1. __modprobe spi-bitbang__ # This could be compiled into the kernel
    1. __insmod ./test_framework.ko g_i2c_enable=1 g_i2c_chip_addr=0x30 g_i2c_names=lp5523__
    1. __insmod ./lp5523-stub.ko__

1. In the case of this specific driver, the lp5523, the driver should load and return to the command prompt. While the driver loads, you're going to get a huge amount of output. SymDrive prints a great deal of information to the console to make it obvious at any point where execution is. You can test other entry points by running cd /sys/devices/i2c-1/1-0030, followed by ls -la inside the VM. This driver exports a variety of interfaces via the sysfs file system.

1. The driver included in the download includes two annotations that correct bugs we found. You can search the leds-lp5523.c for ENABLE_MJR_SYMDRIVE to find the loacations that we modified. The driver is otherwise identical to the one distributed in Linux 3.1.1.

1. This driver represents a straightforward demonstration of what SymDrive can do: it allows developers to load drivers successfully, and then test individual entry points on a per-driver or per-driver-class basis.

## For more information, see [SymDrive](http://research.cs.wisc.edu/sonar/projects/symdrive/downloads.shtml)


