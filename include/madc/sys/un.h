// madc embedded sys/un.h — UNIX domain socket support
// AF_UNIX / AF_LOCAL = 1 (defined in sys/socket.h)
// struct sockaddr_un access deferred (requires struct interop)
// Functions via dlsym fallback (same as sys/socket.h: socket, bind, connect, etc.)
