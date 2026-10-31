#ifndef __I_3DS_MEMORY_H__
#define __I_3DS_MEMORY_H__

// Old3DS CIA exheader requests 80MB; leave room for program and libc.
// No, 80MB is NOT 80MB of malloc. The loader, code, linear heap, etc live here too.
#ifdef FD_CIA_80MB
#define SRB2_3DS_STACK_SIZE       (2u * 1024u * 1024u)
#define SRB2_3DS_NORMAL_HEAP_SIZE (44u * 1024u * 1024u)
#define SRB2_3DS_LINEAR_HEAP_SIZE (24u * 1024u * 1024u)
#else
#define SRB2_3DS_STACK_SIZE       (768u * 1024u)
#define SRB2_3DS_NORMAL_HEAP_SIZE (34u * 1024u * 1024u)
#define SRB2_3DS_LINEAR_HEAP_SIZE (20u * 1024u * 1024u)

#endif // FD_CIA_80MB

#endif
