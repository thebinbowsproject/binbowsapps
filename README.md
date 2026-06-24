How to install

1. Install dependencies

sudo pacman -S base-devel cmake ninja gtk4 webkitgtk-6.0

2. Make folders

mkdir -p ~Documents/internet-exploader
cd ~Documents/internet-exploader

3. install

mkdir build && cd build
cmake -G Ninja ..
ninja
./internetexploader



