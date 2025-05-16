mkdir -p _deps
mv build/_deps/*-src /_deps/
rm -rf build
mkdir -p build
mv _deps build/_deps
