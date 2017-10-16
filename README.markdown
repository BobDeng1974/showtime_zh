Movian mediaplayer
==================

(c) 2006 - 2016 Lonelycoder AB

[![Build status](https://doozer.io/badge/andoma/movian/buildstatus/master)](https://doozer.io/user/andoma/movian)

For more information and latest versions, please visit:

[https://movian.tv/](https://movian.tv/)

## How to build for Linux

First you need to satisfy some dependencies:
For Ubuntu 12.04)

	sudo apt-get install libfreetype6-dev libfontconfig1-dev libxext-dev libgl1-mesa-dev libasound2-dev libasound2-dev libgtk2.0-dev libxss-dev libxxf86vm-dev libxv-dev libvdpau-dev yasm libpulse-dev libssl-dev curl libwebkitgtk-dev libsqlite3-dev

Then you need to configure:

	./configure

If your system lacks libwebkitgtk (Ubuntu 12.04 before 12.04.1) 
you can configure with

	./configure --disable-webkit

If any dependencies are missing the configure script will complain.
You then have the option to disable that particular module/subsystem.

	make

Build the binary, after build the binary resides in `./build.linux/`.
Thus, to start it, just type:

	./build.linux/showtime

Settings are stored in `~/.hts/showtime`

If you want to build with extra debugging options for development these options might be of interest:

	--cc=gcc-5 --extra-cflags=-fno-omit-frame-pointer --optlevel=g --sanitize=address --enable-bughunt


## How to build for Mac OS X

Install Xcode which includes Xcode IDE, gcc toolchain and much more. iPhone SDK also
includes Xcode and toolchain.

Install [MacPorts](http://www.macports.org)

Install pkg-config using MacPorts

	$ sudo port install pkgconfig

Now there are two possible ways to get a build environment, the MacPorts way
or the custome build scripts way. If you dont plan to build for different
architectures or SDKs as you are current running, or dont plan to compile with
fancy extensions, i would recommend the MacPorts way.

If you choose the custome script way, please continue to read support/osx/README

Now run configure

	$ ./configure

Or if you build for release

	$ ./configure --release

If configured successfully run:

	$ make

Run Movian binary from build directory

	$ build.osx/Showtime.app/Contents/MacOS/showtime

Run Movian application from build directory

	$ open build.osx/Showtime.app

Optionally you can build Showtime.dmg disk image. Note that you should
configure with `--release` to embed theme files or else the binary will
include paths to your local build tree.

	$ make dist

For more information read support/osx/README

TODO: universal binary, cant be done i one step as libav does not
build when using multiple arch arguments to gcc


## How to build for PS3 with PSL1GHT

$ ./Autobuild.sh -t ps3

## How to build for Raspberry Pi

First you need to satisfy some dependencies (For Ubuntu 12.04LTS 64bit):

	sudo apt-get install git-core build-essential autoconf bison flex libelf-dev libtool pkg-config texinfo libncurses5-dev libz-dev python-dev libssl-dev libgmp3-dev ccache zip squashfs-tools

$ ./Autobuild.sh -t rpi

To update Movian on rpi with compiled one, enable Binreplace in settings:dev and issue:

	curl --data-binary @build.rpi/showtime.sqfs http://rpi_ip_address:42000/showtime/replace
