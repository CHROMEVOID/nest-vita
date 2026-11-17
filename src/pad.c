/* Build-time padding. vita-elf-create needs a few KB free at the end of the
   code segment for the Vita's import/export tables; when the code happens to
   end right against the data segment, CI rebuilds with NEST_PAD set so this
   read-only array moves the boundary. */
#ifndef NEST_PAD
#define NEST_PAD 1
#endif
__attribute__((used)) const unsigned char nest_segment_pad[NEST_PAD] = {1};
