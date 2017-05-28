# Offsets

When gpu buffers are passed into kernels, we split the incoming virtual address, from the client hostcode, into two parts:
- the `cl_mem` object
- an offset, from this

We then pass these into the kernel separately, eg the kernel could look like:

```
kernel void myKernel(char *clmem0, unsigned int mystruct_offset) {
    global struct MyStruct *mystruct = (global struct MyStruct *)(clmem0 + mystruct_offset);
    // rest of kernel here:
    ...
}
```
All the code in the kernel above, including the code to add on the offset, is automatically generated by Coriander.

However, we have two possible types for the offset:
- `uint64_t` is the most general
- however, on beignet, I found that this gave undefined results, and/or crashes, so I tried `unsigned int`, and that worked ok

At the time of writing, the offset size is set by `#ifdef`s, but I think I shall change it to use an enviornemnt vairable, `32BIT_OFFSET`, means `unsigned int`, if set to `1`, otherwise the offsets will be `uint64_t`.

In terms of the various generation/parse processes:
- patch_hostside doesnt need to handle the offset: it will simply pass the virtual pointer to the coriander runtime, at runtime
- hostside_opencl_funcs will handle both:
  - generating the opencl, at runtime, includign writing out the kernel declarations, as above; and
  - splitting the incoming virtual pointers into `cl_mem` object, and offset, and therefore can control this side too

Therefore, going forward:
- the `#ifdef`s will be removed
- `hostside_opencl_funcs` will decide at runtime, according to the presence of `32BIT_OFFSET=1` or not, which offset type to use