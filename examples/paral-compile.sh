g++ paralsg4.cpp -o paralsg $(pkg-config --cflags --libs libbitcoin-system libsecp256k1) -std=c++2a -O0 -g -fconcepts -I /home/aa/src/deps/secp256k1/src/ -DSECP256K1_BUILD -DHAVE_CONFIG_H && ./paralsg
