#!/bin/bash

[ ! -d config ] && mkdir -p config

[ ! -e configure ] && ./autogen.sh

[ ! -d build ] && mkdir build

cd build

if [ ! -e Makefile ]; then
    echo "configuring..."
    ../configure \
        --prefix=$PREFIX \
        --with-libibverbs=$PREFIX \
        --with-libgdsync=$PREFIX \
        --with-cuda=$CUDA \
        --with-mpi=$MPI_HOME \
        --enable-tests
fi

make clean all install
