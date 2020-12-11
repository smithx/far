mkdir %1
cd    %1
cmake -DBUILD_SHARED_LIBS=OFF -G "Visual Studio 15 2017" -A %2  ../..
cmake --build . --config %3