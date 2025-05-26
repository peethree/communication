# communication

This is a **work in progress** chat application implementing a router/dealer pattern with the help of [CZMQ](https://zeromq.org/languages/c/#czmq). The dealer runs on three threads, 1 for the main function and the raylib window that's being drawn on and two others for receiving and sending messages. This is due to the blocking nature of receiving messages. The router binds to a port and waits for clients (dealers) to connect. Once connected the dealer and router can send messages back and forth. Each dealer sets its identity and the router forwards the messages based on the dealer's identity. The communication is end-to-end encrypted using [openssl](https://openssl-library.org/) encryption. Dealers can decrypt each others' messages, whereas the router will receive encrypted hex values. For now it uses a dummy key and iv. I've experimented with a blocking and non-blocking router. For testing non-blocking is pleasant, but for performance the other option is better. 

Inside the raylib window a user can chat with another user that's connected to the router. Currently the usage works as follows:
```bash
./dealer user(you) friend
```
User input will be drawn and when committed by pressing enter, it will be saved in a chat log which will also be drawn on screen along with a timestamp. The text overflows off the screen when too much is written, which will need a solution in the future. Perhaps later down the line I want to save the chat log but as of right now it resets when closing the program.

#### build
The binaries are built using [nob](https://github.com/tsoding/nob.h). Instead of Cmake or a makefile, one can fill up a dynamic array of commands, compile it once. Then, running the executable will update the build script if changes were made as well as make new binaries.

#### dependencies 
1. raylib
2. czmq (libczmq)
3. nob.h
4. openssl/evp & openssl/rand

TODO: make it (more) cross platform, currently the binaries are dynamically linked on a linux machine. 