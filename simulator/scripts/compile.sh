#!/bin/bash
ZSIMPATH=$(pwd)
PINPATH="$ZSIMPATH/pin"
LIBCONFIGPATH="$ZSIMPATH/libconfig"
DRAMSIMPATH="$ZSIMPATH/DRAMSim2"
RAMULATORPATH="$ZSIMPATH/ramulator"
NUMCPUS=$(grep -c ^processor /proc/cpuinfo)


if [ "$1" = "z" ]
then
	echo "Compiling only ZSim ..."
        export PINPATH
        export RAMULATORPATH
        export LIBCONFIGPATH
		if [ "$2" = "d" ]
		then
        	scons -j$NUMCPUS --d
		else
			scons -j$NUMCPUS
		fi

elif [ "$1" = "r" ]
then
	echo "Compiling only Ramulator ..."
        export RAMULATORPATH
        cd $RAMULATORPATH
        make libramulator.so
        cd ..

else
	echo "Compiling all ..."
	export LIBCONFIGPATH
	cd $LIBCONFIGPATH
	./configure --prefix=$LIBCONFIGPATH && make install
	cd ..


	export RAMULATORPATH
	cd $RAMULATORPATH
	make libramulator.so
	cd ..

	export PINPATH
	scons -j$NUMCPUS
fi