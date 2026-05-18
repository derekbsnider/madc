// madc embedded zlib.h — minimal stub for z_stream pointer members.
// Full zlib functionality requires linking against libz.

typedef struct z_stream_s {
    void *next_in;
    unsigned int avail_in;
    unsigned long total_in;
    void *next_out;
    unsigned int avail_out;
    unsigned long total_out;
    void *msg;
    void *state;
    void *zalloc;
    void *zfree;
    void *opaque;
    int data_type;
    unsigned long adler;
    unsigned long reserved;
} z_stream;
