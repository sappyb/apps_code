../configure --with-online=/home/bhowmik/installSoftware/swm-kev \
             --with-boost=/home/bhowmik/buildSoftware/installSoftware/boost \
             --prefix=/home/bhowmik/installSoftware/codes-swm \
             CC=mpicc \
             CXX=mpicxx \
             CFLAGS='-O0 -I/home/bhowmik/buildSoftware/TensorflowC/include' \
             CXXFLAGS='-O0 --std=c++14 -I/home/bhowmik/buildSoftware/TensorflowC/include -I/home/bhowmik/buildSoftware/cppf/include' \
             LDFLAGS='-L/home/bhowmik/lib -L/home/bhowmik/buildSoftware/TensorflowC/lib' \
             LIBS='-ltensorflow' \
             PKG_CONFIG_PATH=/home/bhowmik/installSoftware/argobots/lib/pkgconfig:/home/bhowmik/installSoftware/ROSS-new/lib/pkgconfig:/home/bhowmik/installSoftware/swm-kev/lib/pkgconfig;
make clean;
make;
make install;
