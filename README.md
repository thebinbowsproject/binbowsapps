How to install

1. Install dependencies

sudo pacman -S base-devel cmake ninja gtk4 webkitgtk-6.0

2. Make folders

mkdir -p ~Documents/internet-exploader
cd ~Documents/internet-exploader

3. Install

mkdir build && cd build
cmake -G Ninja ..
ninja
./internetexploader

4. Assets
   
   Place Launchassets and Sites into the "Build" folder
