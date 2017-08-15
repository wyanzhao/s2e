# SymDrive
# Setting up the Environment and S2E
1. Install git using sudo apt-get install git.
2. Check out S2E using git clone https://github.com/wyz7155/SymDrive.git
3. Once the clone is finished, be sure you've installed all the packages listed on the S2E site as necessary to build S2E.
$ sudo apt-get install build-essential
$ sudo apt-get install subversion
$ sudo apt-get install git
$ sudo apt-get install gettext
$ sudo apt-get install liblua5.1-0-dev
$ sudo apt-get install libsdl1.2-dev
$ sudo apt-get install libsigc++-2.0-dev
$ sudo apt-get install binutils-dev
$ sudo apt-get install python-docutils
$ sudo apt-get install python-pygments
$ sudo apt-get install nasm
$ sudo apt-get build-dep llvm-2.7
$ sudo apt-get build-dep qemu
4. mkdir $S2E/build
   cd $S2E/build
   ln -s ../s2e/Makefile
   make --jobs=1
Compile S2E with the integrated SymDrive patch per the instructions on their site. Note that to improve reliability, it may be worth using make JOBS=1; however, I've always had success with make JOBS=10 if I run it repeatedly. The Makefile seems to have a bug or two, so you may need to run "make" up to ~5 times before it completes successfully. It should not be necessary to change any source code.

 

